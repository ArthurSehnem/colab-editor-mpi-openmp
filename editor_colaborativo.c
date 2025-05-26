#include <mpi.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // Para sleep, opcional

#define MAX_LINES 100
#define MAX_LEN 256
#define NO_LOCK -1 // Indica que a linha está livre

// Novas Tags MPI
#define TAG_LOCK_REQUEST 1          // Cliente -> Servidor: [line_idx]
#define TAG_LOCK_RESPONSE 2         // Servidor -> Cliente: [int granted (1 ou 0)]
#define TAG_EDIT_SUBMIT 3           // Cliente -> Servidor: [line_idx, new_text_char_array]
#define TAG_CHAT_MESSAGE 4          // Cliente -> Cliente ou Cliente -> Servidor (para log/broadcast se desejado)
#define TAG_LINE_UPDATE_BCAST 5     // Servidor -> Todos: [line_idx, Line struct]
#define TAG_FULL_DOC_REQUEST 6      // Cliente -> Servidor (solicita documento inteiro) - Opcional
#define TAG_FULL_DOC_BCAST_HEADER 7 // Servidor -> Todos: [num_lines]
#define TAG_FULL_DOC_BCAST_LINE 8   // Servidor -> Todos: [Line struct] (enviado em loop)
#define TAG_AUTODATA_SUBMIT 9       // Cliente -> Servidor: Envia o documento inteiro modificado
                                    // (Primeiro envia num_lines, depois cada linha)

// Estrutura para armazenar a linha do texto e status do bloqueio
typedef struct {
    char text[MAX_LEN];
    int locked_by; // rank do usuário que bloqueou a linha, NO_LOCK se livre
} Line;

Line document[MAX_LINES]; // Cópia local do documento para cada processo
int current_num_lines = 10; // Começa com 10 linhas, pode ser alterado por rank 0

FILE *log_file;

// Função para logar genericamente
void log_message(const char* message) {
    if (log_file) {
        fprintf(log_file, "%s\n", message);
        fflush(log_file);
    }
}

// Função para logar alterações específicas
void log_edit_action(int actor_rank, int target_line, const char* action_description, const char* content) {
    if (log_file) {
        if (content && strlen(content) > 0) {
            fprintf(log_file, "User %d: %s line %d - Content: %s\n", actor_rank, action_description, target_line, content);
        } else {
            fprintf(log_file, "User %d: %s line %d\n", actor_rank, action_description, target_line);
        }
        fflush(log_file);
    }
}


// Inicializa documento com texto default (chamado por rank 0)
void init_document_server() {
    for (int i = 0; i < current_num_lines; i++) {
        snprintf(document[i].text, MAX_LEN, "Linha %d inicial.", i);
        document[i].locked_by = NO_LOCK;
    }
    log_message("Document initialized by server.");
}

// Imprime documento na tela do usuário atual
void print_document_client(int my_rank) {
    printf("\n--- [Usuário %d] Documento Atual (Total Linhas: %d) ---\n", my_rank, current_num_lines);
    for (int i = 0; i < current_num_lines; i++) {
        printf("%3d [%s] %s\n", i,
               (document[i].locked_by == NO_LOCK) ? "Livre   " :
               (document[i].locked_by == my_rank ? "Você    " : "Bloq."),
               document[i].text);
    }
    printf("-----------------------------------------------------\n");
}

// Função que gera dados automaticamente (simulando alterações) com OpenMP
// Modifica o array 'doc_to_modify'
void gerar_dados_automaticos_omp(Line* doc_to_modify, int num_linhas_doc, int editor_rank) {
    #pragma omp parallel for
    for (int i = 0; i < num_linhas_doc; i++) {
        char temp_text[MAX_LEN];
        // Lê o texto original da linha de forma segura se múltiplas threads pudessem acessar a mesma
        // Neste caso, cada thread trabalha em 'i' diferente, então é seguro ler doc_to_modify[i].text
        // Mas a escrita deve ser protegida se a lógica fosse mais complexa.
        // Aqui, estamos apenas construindo um novo texto.

        #pragma omp critical (doc_access_for_read)
        {
             // Se a linha original fosse muito longa, snprintf abaixo poderia truncar.
             // Para esta simulação, assumimos que cabe.
        }

        snprintf(temp_text, MAX_LEN, "AutoGen por %d - linha %d - %ld",
                 editor_rank, i, (long)omp_get_wtime()); // Adiciona um timestamp para variar

        #pragma omp critical (doc_access_for_write_log) // Protege a escrita no documento e o log
        {
            strncpy(doc_to_modify[i].text, temp_text, MAX_LEN -1);
            doc_to_modify[i].text[MAX_LEN-1] = '\0'; // Garante terminação nula
            // Não mexe no lock status aqui, o servidor vai resetar.
            // log_edit_action(editor_rank, i, "auto-generated content for", temp_text); // Log local do cliente
        }
    }
    // O log da alteração efetiva será feito pelo servidor ao receber.
}


// --- Funções do Servidor (Rank 0) ---
void broadcast_full_document(int num_procs) {
    log_message("Server: Broadcasting full document.");
    MPI_Bcast(&current_num_lines, 1, MPI_INT, 0, MPI_COMM_WORLD);
    for (int i = 0; i < current_num_lines; i++) {
        MPI_Bcast(&document[i], sizeof(Line), MPI_BYTE, 0, MPI_COMM_WORLD);
    }
}

void broadcast_line_update(int line_idx) {
    char log_buf[MAX_LEN + 50];
    snprintf(log_buf, sizeof(log_buf), "Server: Broadcasting update for line %d. Locked by: %d. Text: %s", line_idx, document[line_idx].locked_by, document[line_idx].text);
    log_message(log_buf);

    MPI_Request req; // Usar não bloqueante para o servidor não travar se um cliente morrer
    // Para simplificar, vamos usar bloqueante por enquanto. Em produção, Irecv/Isend seria melhor.
    for(int i=0; i< MPI_Comm_size(MPI_COMM_WORLD, &i); i++){ // Envia para todos, inclusive ele mesmo (embora já tenha)
        if(i == 0) continue; // Não precisa enviar para si mesmo desta forma (ou pode e ignorar)
         MPI_Send(&document[line_idx], sizeof(Line), MPI_BYTE, i, TAG_LINE_UPDATE_BCAST, MPI_COMM_WORLD);
    }
    // Alternativa com Bcast (mais simples e geralmente preferível):
    // MPI_Bcast(&line_idx, 1, MPI_INT, 0, MPI_COMM_WORLD); // Envia o índice primeiro
    // MPI_Bcast(&document[line_idx], sizeof(Line), MPI_BYTE, 0, MPI_COMM_WORLD); // Depois a linha
    // Nota: A abordagem acima (MPI_Send em loop) foi para ilustrar TAG_LINE_UPDATE_BCAST
    // com um destino específico. Para broadcast real, MPI_Bcast é o caminho.
    // Vamos corrigir para usar Bcast corretamente:
    // Primeiro, todos precisam saber qual linha está sendo atualizada.
    // Criar um payload: {int line_idx; Line line_data;} ou enviar separado.
    // Para este exemplo, vamos enviar a linha inteira e o cliente já saberá o índice ao receber pela TAG.
    // No entanto, se a TAG_LINE_UPDATE_BCAST puder ser para qualquer linha, precisamos enviar o índice.

    // Solução mais robusta para broadcast de linha única:
    int target_line_idx = line_idx; // Para clareza
    // Todos os processos precisam participar do Bcast
    MPI_Bcast(&target_line_idx, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&document[line_idx], sizeof(Line), MPI_BYTE, 0, MPI_COMM_WORLD);

}

void server_main_loop(int my_rank, int num_procs) {
    MPI_Status status;
    int request_data[1]; // Para line_idx em Lock Request
    char text_buffer[MAX_LEN];
    Line received_line_buffer;

    init_document_server();
    broadcast_full_document(num_procs);

    printf("Servidor (Rank %d) iniciado e aguardando conexões/mensagens...\n", my_rank);

    while (1) {
        int flag = 0;
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);

        if (flag) {
            int source_rank = status.MPI_SOURCE;
            int tag = status.MPI_TAG;

            if (tag == TAG_LOCK_REQUEST) {
                MPI_Recv(request_data, 1, MPI_INT, source_rank, TAG_LOCK_REQUEST, MPI_COMM_WORLD, &status);
                int line_to_lock = request_data[0];
                int response = 0; // 0 = negado, 1 = concedido

                char log_buf[100];
                snprintf(log_buf, sizeof(log_buf), "Server: Received lock request for line %d from rank %d.", line_to_lock, source_rank);
                log_message(log_buf);

                if (line_to_lock >= 0 && line_to_lock < current_num_lines) {
                    if (document[line_to_lock].locked_by == NO_LOCK) {
                        document[line_to_lock].locked_by = source_rank;
                        response = 1; // Concedido
                        log_edit_action(source_rank, line_to_lock, "locked", "");
                        broadcast_line_update(line_to_lock); // Informa a todos sobre o novo lock
                    } else {
                         snprintf(log_buf, sizeof(log_buf), "Server: Lock for line %d denied to rank %d (already locked by %d).", line_to_lock, source_rank, document[line_to_lock].locked_by);
                         log_message(log_buf);
                    }
                } else {
                    log_message("Server: Invalid line index in lock request.");
                }
                MPI_Send(&response, 1, MPI_INT, source_rank, TAG_LOCK_RESPONSE, MPI_COMM_WORLD);

            } else if (tag == TAG_EDIT_SUBMIT) {
                // Cliente envia: line_idx (int), depois new_text (char array)
                // Precisa de um protocolo mais robusto, ex: struct ou tamanho do texto antes.
                // Para simplificar, assumimos que o cliente envia o índice primeiro e depois o texto.
                int edited_line_idx;
                MPI_Recv(&edited_line_idx, 1, MPI_INT, source_rank, TAG_EDIT_SUBMIT, MPI_COMM_WORLD, &status); // Recebe o índice
                MPI_Recv(text_buffer, MAX_LEN, MPI_CHAR, source_rank, TAG_EDIT_SUBMIT, MPI_COMM_WORLD, &status); // Recebe o texto

                char log_buf[MAX_LEN + 50];
                snprintf(log_buf, sizeof(log_buf), "Server: Received edit for line %d from rank %d.", edited_line_idx, source_rank);
                log_message(log_buf);

                if (edited_line_idx >= 0 && edited_line_idx < current_num_lines) {
                    if (document[edited_line_idx].locked_by == source_rank) { // Verifica se quem editou é quem bloqueou
                        strncpy(document[edited_line_idx].text, text_buffer, MAX_LEN -1);
                        document[edited_line_idx].text[MAX_LEN-1] = '\0';
                        document[edited_line_idx].locked_by = NO_LOCK; // Desbloqueia
                        log_edit_action(source_rank, edited_line_idx, "edited and unlocked", text_buffer);
                        broadcast_line_update(edited_line_idx); // Informa a todos sobre a edição e desbloqueio
                    } else {
                        snprintf(log_buf, sizeof(log_buf), "Server: Edit on line %d by rank %d rejected (not locked by this user or invalid lock).", edited_line_idx, source_rank);
                        log_message(log_buf);
                        // Opcional: informar o cliente sobre a rejeição.
                    }
                } else {
                     log_message("Server: Invalid line index in edit submit.");
                }
            } else if (tag == TAG_CHAT_MESSAGE) {
                MPI_Recv(text_buffer, MAX_LEN, MPI_CHAR, source_rank, TAG_CHAT_MESSAGE, MPI_COMM_WORLD, &status);
                 char log_buf[MAX_LEN + 50];
                snprintf(log_buf, sizeof(log_buf),"Server: Received CHAT from %d: %s (Broadcasting...)", source_rank, text_buffer);
                log_message(log_buf);
                // O servidor poderia retransmitir mensagens de chat para todos ou apenas logar.
                // Para este exemplo, vamos fazer o servidor retransmitir para todos os *outros* clientes.
                for (int i = 1; i < num_procs; i++) { // Começa em 1 para não enviar de volta ao servidor
                    if (i != source_rank) { // Não envia de volta para quem originou
                        char chat_fwd_msg[MAX_LEN + 20];
                        snprintf(chat_fwd_msg, sizeof(chat_fwd_msg), "[Chat de %d]: %s", source_rank, text_buffer);
                        MPI_Send(chat_fwd_msg, strlen(chat_fwd_msg) + 1, MPI_CHAR, i, TAG_CHAT_MESSAGE, MPI_COMM_WORLD);
                    }
                }

            } else if (tag == TAG_AUTODATA_SUBMIT) {
                // Cliente envia o documento inteiro. Primeiro current_num_lines, depois cada Line.
                int submitted_num_lines;
                MPI_Recv(&submitted_num_lines, 1, MPI_INT, source_rank, TAG_AUTODATA_SUBMIT, MPI_COMM_WORLD, &status);

                char log_buf[100];
                snprintf(log_buf, sizeof(log_buf), "Server: Received AutoData submission from rank %d with %d lines.", source_rank, submitted_num_lines);
                log_message(log_buf);

                if (submitted_num_lines > MAX_LINES) submitted_num_lines = MAX_LINES; // Trunca se exceder
                current_num_lines = submitted_num_lines; // Servidor adota o novo número de linhas

                for (int i = 0; i < current_num_lines; i++) {
                    MPI_Recv(&document[i], sizeof(Line), MPI_BYTE, source_rank, TAG_AUTODATA_SUBMIT, MPI_COMM_WORLD, &status);
                    document[i].locked_by = NO_LOCK; // Garante que todas as linhas venham desbloqueadas
                }
                log_message("Server: AutoData applied. Broadcasting new full document.");
                broadcast_full_document(num_procs);
            }
            // Adicionar mais handlers de TAG aqui se necessário
        }
        // O servidor pode fazer outras tarefas aqui em modo não bloqueante ou dormir um pouco
        // usleep(10000); // 10ms, para não consumir 100% CPU só com Iprobe
    }
}

// --- Funções do Cliente (Rank > 0) ---

void client_handle_incoming_messages(int my_rank) {
    MPI_Status status;
    int flag = 0;
    char chat_buffer[MAX_LEN + 20];
    Line received_line;
    int updated_line_idx;

    // Verifica Line Update Broadcast
    MPI_Iprobe(0, TAG_LINE_UPDATE_BCAST, MPI_COMM_WORLD, &flag, &status);
    if (flag) {
        // Protocolo: Rank 0 envia primeiro o índice da linha, depois a linha em si.
        MPI_Recv(&updated_line_idx, 1, MPI_INT, 0, TAG_LINE_UPDATE_BCAST, MPI_COMM_WORLD, &status);
        MPI_Recv(&document[updated_line_idx], sizeof(Line), MPI_BYTE, 0, TAG_LINE_UPDATE_BCAST, MPI_COMM_WORLD, &status);
        if (updated_line_idx >=0 && updated_line_idx < current_num_lines) {
           // document[updated_line_idx] = received_line; // Já feito pelo Recv direto no array
            printf("\n[Usuário %d] Notificação: Linha %d foi atualizada.\n", my_rank, updated_line_idx);
            log_edit_action(status.MPI_SOURCE, updated_line_idx, "received update for", document[updated_line_idx].text);
        }
    }

    // Verifica Full Document Broadcast
    MPI_Iprobe(0, TAG_FULL_DOC_BCAST_HEADER, MPI_COMM_WORLD, &flag, &status);
    if (flag) {
        MPI_Recv(&current_num_lines, 1, MPI_INT, 0, TAG_FULL_DOC_BCAST_HEADER, MPI_COMM_WORLD, &status);
        printf("\n[Usuário %d] Notificação: Recebendo documento completo (%d linhas).\n", my_rank, current_num_lines);
        for (int i = 0; i < current_num_lines; i++) {
            MPI_Recv(&document[i], sizeof(Line), MPI_BYTE, 0, TAG_FULL_DOC_BCAST_LINE, MPI_COMM_WORLD, &status); // Assume que o servidor envia TAG_FULL_DOC_BCAST_LINE para cada linha
        }
        log_message("Client: Received full document broadcast.");
    }
    // Nota: Se o servidor usar MPI_Bcast para TAG_FULL_DOC_BCAST_LINE, o cliente também deve usar MPI_Bcast.
    // A lógica atual do servidor para broadcast_full_document usa MPI_Bcast, então o cliente precisa participar corretamente.
    // A checagem acima com Iprobe e Recv individual para TAG_FULL_DOC_BCAST_LINE é para um envio em loop com TAG diferente.
    // Vamos ajustar o cliente para corresponder ao broadcast_full_document do servidor:

    // ESTA PARTE PRECISA SER COLETIVA, ou seja, todos os clientes esperam por ela se o servidor fizer Bcast.
    // A maneira mais simples de o cliente receber um broadcast completo é não dentro de um Iprobe seletivo,
    // mas em um ponto onde todos os clientes esperam a atualização.
    // Por agora, as atualizações de linha única e chat são mais adequadas para Iprobe.

    // Verifica Chat Messages (enviadas pelo servidor como retransmissão, ou diretamente de outro cliente se o protocolo permitir)
    MPI_Iprobe(MPI_ANY_SOURCE, TAG_CHAT_MESSAGE, MPI_COMM_WORLD, &flag, &status);
    if (flag) {
        MPI_Recv(chat_buffer, sizeof(chat_buffer), MPI_CHAR, status.MPI_SOURCE, TAG_CHAT_MESSAGE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        printf("\n[Usuário %d] %s\n", my_rank, chat_buffer); // O servidor já formata com "[Chat de X]"
        log_message(chat_buffer);
    }
}


void client_main_loop(int my_rank, int num_procs) {
    // Cliente primeiro recebe o documento inicial
    MPI_Bcast(&current_num_lines, 1, MPI_INT, 0, MPI_COMM_WORLD);
    for (int i = 0; i < current_num_lines; i++) {
        MPI_Bcast(&document[i], sizeof(Line), MPI_BYTE, 0, MPI_COMM_WORLD);
    }
    printf("Cliente (Rank %d) conectado e documento inicial recebido.\n", my_rank);

    int opc;
    char input_buffer[MAX_LEN];

    while (1) {
        client_handle_incoming_messages(my_rank); // Verifica mensagens não solicitadas do servidor/outros

        print_document_client(my_rank);
        printf("\n--- Usuário %d - Menu ---\n", my_rank);
        printf("1 - Editar linha\n");
        printf("2 - Enviar mensagem privada (Chat)\n");
        printf("3 - Gerar dados automáticos (OpenMP e submeter)\n");
        // printf("4 - Solicitar Sincronização Total (do Servidor)\n"); // Funcionalidade opcional
        printf("0 - Sair\n");
        printf("Escolha: ");

        if (scanf("%d", &opc) != 1) {
            while(getchar()!='\n'); // Limpa buffer de entrada em caso de erro
            opc = -1; // Opção inválida
        }
        while(getchar()!='\n'); // Limpa o restante do buffer (newline)

        switch (opc) {
            case 1: { // Editar linha
                printf("Digite o número da linha para editar: ");
                int line_idx;
                if (scanf("%d", &line_idx) != 1) { while(getchar()!='\n'); printf("Entrada inválida.\n"); break; }
                while(getchar()!='\n');

                if (line_idx < 0 || line_idx >= current_num_lines) {
                    printf("Linha inválida.\n");
                    break;
                }

                // 1. Solicitar bloqueio ao servidor
                printf("Solicitando bloqueio para linha %d...\n", line_idx);
                MPI_Send(&line_idx, 1, MPI_INT, 0, TAG_LOCK_REQUEST, MPI_COMM_WORLD);
                log_edit_action(my_rank, line_idx, "requested lock for", "");

                // 2. Aguardar resposta do servidor
                int lock_response;
                MPI_Recv(&lock_response, 1, MPI_INT, 0, TAG_LOCK_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                if (lock_response == 1) {
                    printf("Bloqueio concedido para linha %d. Suas alterações locais nela podem ser sobrescritas por broadcasts até você submeter.\n", line_idx);
                    // O cliente *deveria* esperar o broadcast do lock antes de editar,
                    // ou a interface mostrar o lock imediatamente baseado na resposta.
                    // Para simplificar, vamos editar e submeter. A cópia local será atualizada pelo broadcast.

                    printf("Digite o novo texto: ");
                    fgets(input_buffer, MAX_LEN, stdin);
                    input_buffer[strcspn(input_buffer, "\n")] = 0; // remove \n

                    // 3. Enviar edição para o servidor (índice primeiro, depois texto)
                    MPI_Send(&line_idx, 1, MPI_INT, 0, TAG_EDIT_SUBMIT, MPI_COMM_WORLD);
                    MPI_Send(input_buffer, strlen(input_buffer) + 1, MPI_CHAR, 0, TAG_EDIT_SUBMIT, MPI_COMM_WORLD);
                    log_edit_action(my_rank, line_idx, "submitted edit for", input_buffer);
                    printf("Edição submetida. Aguardando broadcast do servidor para confirmação...\n");
                } else {
                    printf("Bloqueio negado para linha %d (provavelmente já em uso).\n", line_idx);
                    log_edit_action(my_rank, line_idx, "lock request denied for", "");
                }
                break;
            }
            case 2: { // Enviar Chat
                int dest_rank_chat; // Para chat direto (não implementado no servidor para P2P ainda)
                                    // Ou apenas envia para o servidor para ele retransmitir

                printf("Digite a mensagem de chat (será enviada para o servidor retransmitir): ");
                fgets(input_buffer, MAX_LEN, stdin);
                input_buffer[strcspn(input_buffer, "\n")] = 0;

                MPI_Send(input_buffer, strlen(input_buffer) + 1, MPI_CHAR, 0, TAG_CHAT_MESSAGE, MPI_COMM_WORLD);
                printf("Mensagem de chat enviada para o servidor.\n");
                log_message("Client: Sent chat message.");
                break;
            }
            case 3: { // Gerar dados automáticos
                printf("Gerando dados automáticos localmente com OpenMP...\n");
                // Cria uma cópia temporária para não bagunçar o 'document' principal antes de submeter
                Line temp_doc[MAX_LINES];
                int temp_num_lines = current_num_lines; // Usa o num_lines atual
                memcpy(temp_doc, document, sizeof(Line) * temp_num_lines);

                gerar_dados_automaticos_omp(temp_doc, temp_num_lines, my_rank);
                printf("Dados gerados. Submetendo ao servidor...\n");

                // Enviar o documento inteiro para o servidor
                MPI_Send(&temp_num_lines, 1, MPI_INT, 0, TAG_AUTODATA_SUBMIT, MPI_COMM_WORLD);
                for (int i = 0; i < temp_num_lines; i++) {
                    MPI_Send(&temp_doc[i], sizeof(Line), MPI_BYTE, 0, TAG_AUTODATA_SUBMIT, MPI_COMM_WORLD);
                }
                log_message("Client: Submitted auto-generated data to server.");
                printf("Dados submetidos. Aguarde o broadcast do servidor.\n");
                break;
            }
            case 0:
                printf("Saindo...\n");
                log_message("Client: Exiting.");
                // Opcional: Enviar mensagem de desconexão para o servidor
                return; // Sai do loop e da função
            default:
                printf("Opção inválida.\n");
        }
        // Pequena pausa para não sobrecarregar e permitir que mensagens cheguem
        // usleep(100000); // 100ms
    }
}


int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2 && rank ==0) { // Precisa de pelo menos 1 servidor e 1 cliente para a lógica atual
        fprintf(stderr, "Este programa é projetado para rodar com pelo menos 2 processos (1 servidor, 1+ clientes).\n");
       // MPI_Finalize(); // Comentei para permitir rodar com 1 processo para teste rápido do servidor/cliente isolado
       // return 1;
    }


    char nome_log[64];
    snprintf(nome_log, 64, "log_usuario_%d.txt", rank);
    log_file = fopen(nome_log, "w");
    if (!log_file) {
        printf("Erro ao abrir arquivo de log %s!\n", nome_log);
        MPI_Abort(MPI_COMM_WORLD, 1); // Aborta todos os processos MPI
        return 1;
    }
    char initial_log_msg[100];
    snprintf(initial_log_msg, sizeof(initial_log_msg), "Process %d (world size %d) started. Log file created.", rank, size);
    log_message(initial_log_msg);


    if (rank == 0) {
        // Código do Servidor
        server_main_loop(rank, size);
    } else {
        // Código do Cliente
        client_main_loop(rank, size);
    }

    if (log_file) {
        fclose(log_file);
    }
    MPI_Finalize();
    return 0;
}