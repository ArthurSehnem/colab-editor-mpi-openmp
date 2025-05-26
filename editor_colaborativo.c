#include <mpi.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define MAX_LINES 100
#define MAX_LEN 256
#define NO_LOCK -1

// Tags MPI
#define TAG_LOCK_REQUEST 1
#define TAG_LOCK_RESPONSE 2
#define TAG_EDIT_SUBMIT 3
#define TAG_CHAT_MESSAGE 4
#define TAG_LINE_UPDATE_BCAST 5
#define TAG_FULL_DOC_REQUEST 6
#define TAG_FULL_DOC_BCAST_HEADER 7
#define TAG_FULL_DOC_BCAST_LINE 8
#define TAG_AUTODATA_SUBMIT 9
#define TAG_PRIVATE_MESSAGE 10

// Estruturas
typedef struct {
    char text[MAX_LEN];
    int locked_by;
} Line;

typedef struct {
    int sender_rank;
    int target_rank;
    char message[MAX_LEN];
} PrivateMessage;

// Variáveis globais
Line document[MAX_LINES];
int current_num_lines = 10;
FILE *log_file;

// Funções de logging
void log_message(const char* message) {
    if (log_file) {
        time_t now = time(NULL);
        char* time_str = ctime(&now);
        time_str[strlen(time_str)-1] = '\0'; // Remove \n
        fprintf(log_file, "[%s] %s\n", time_str, message);
        fflush(log_file);
    }
}

void log_edit_action(int actor_rank, int target_line, const char* action_description, const char* content) {
    if (log_file) {
        char log_buf[MAX_LEN * 2];
        if (content && strlen(content) > 0) {
            snprintf(log_buf, sizeof(log_buf), "User %d: %s line %d - Content: %s", 
                    actor_rank, action_description, target_line, content);
        } else {
            snprintf(log_buf, sizeof(log_buf), "User %d: %s line %d", 
                    actor_rank, action_description, target_line);
        }
        log_message(log_buf);
    }
}

// Inicialização do documento (servidor)
void init_document_server() {
    for (int i = 0; i < current_num_lines; i++) {
        snprintf(document[i].text, MAX_LEN, "Linha %d - Conteudo inicial do documento colaborativo.", i);
        document[i].locked_by = NO_LOCK;
    }
    log_message("Document initialized by server.");
}

// Impressão do documento (cliente)
void print_document_client(int my_rank) {
    printf("\n=== [Usuario %d] Documento Atual (%d linhas) ===\n", my_rank, current_num_lines);
    for (int i = 0; i < current_num_lines; i++) {
        const char* status = (document[i].locked_by == NO_LOCK) ? "Livre   " :
                            (document[i].locked_by == my_rank ? "Editando" : "Bloqueado");
        printf("%3d [%s] %s\n", i, status, document[i].text);
    }
    printf("===============================================\n");
}

// Geração automática de dados com OpenMP
void gerar_dados_automaticos_omp(Line* doc_to_modify, int num_linhas_doc, int editor_rank) {
    printf("Gerando dados automaticamente com OpenMP...\n");
    
    #pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < num_linhas_doc; i++) {
        char temp_text[MAX_LEN];
        int thread_id = omp_get_thread_num();
        
        #pragma omp critical (doc_generation)
        {
            snprintf(temp_text, MAX_LEN, "AutoGen[User:%d Thread:%d] Linha %d - Timestamp:%ld", 
                    editor_rank, thread_id, i, time(NULL) + i);
            
            strncpy(doc_to_modify[i].text, temp_text, MAX_LEN - 1);
            doc_to_modify[i].text[MAX_LEN - 1] = '\0';
            doc_to_modify[i].locked_by = NO_LOCK;
        }
    }
    
    char log_buf[100];
    snprintf(log_buf, sizeof(log_buf), "Auto-generated %d lines using OpenMP with %d threads", 
            num_linhas_doc, omp_get_max_threads());
    log_message(log_buf);
}

// Broadcast do documento completo (servidor)
void broadcast_full_document(int num_procs) {
    log_message("Server: Broadcasting full document to all clients.");
    
    // Envia número de linhas
    MPI_Bcast(&current_num_lines, 1, MPI_INT, 0, MPI_COMM_WORLD);
    
    // Envia cada linha
    for (int i = 0; i < current_num_lines; i++) {
        MPI_Bcast(&document[i], sizeof(Line), MPI_BYTE, 0, MPI_COMM_WORLD);
    }
}

// Broadcast de atualização de linha específica (servidor)
void broadcast_line_update(int line_idx) {
    char log_buf[MAX_LEN + 100];
    snprintf(log_buf, sizeof(log_buf), "Server: Broadcasting update for line %d. Status: %s. Text: %s", 
            line_idx, 
            (document[line_idx].locked_by == NO_LOCK) ? "Unlocked" : "Locked",
            document[line_idx].text);
    log_message(log_buf);
    
    // Envia índice da linha e depois a linha
    MPI_Bcast(&line_idx, 1, MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&document[line_idx], sizeof(Line), MPI_BYTE, 0, MPI_COMM_WORLD);
}

// Loop principal do servidor
void server_main_loop(int my_rank, int num_procs) {
    MPI_Status status;
    int request_data[1];
    char text_buffer[MAX_LEN];
    
    init_document_server();
    broadcast_full_document(num_procs);
    
    printf("=== SERVIDOR (Rank %d) INICIADO ===\n", my_rank);
    printf("Aguardando conexoes de %d clientes...\n", num_procs - 1);
    printf("Logs sendo salvos em: log_usuario_%d.txt\n", my_rank);
    printf("=====================================\n");
    
    while (1) {
        int flag = 0;
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
        
        if (flag) {
            int source_rank = status.MPI_SOURCE;
            int tag = status.MPI_TAG;
            
            switch (tag) {
                case TAG_LOCK_REQUEST: {
                    MPI_Recv(request_data, 1, MPI_INT, source_rank, TAG_LOCK_REQUEST, MPI_COMM_WORLD, &status);
                    int line_to_lock = request_data[0];
                    int response = 0;
                    
                    char log_buf[150];
                    snprintf(log_buf, sizeof(log_buf), "Server: Lock request for line %d from user %d", 
                            line_to_lock, source_rank);
                    log_message(log_buf);
                    
                    if (line_to_lock >= 0 && line_to_lock < current_num_lines) {
                        if (document[line_to_lock].locked_by == NO_LOCK) {
                            document[line_to_lock].locked_by = source_rank;
                            response = 1;
                            log_edit_action(source_rank, line_to_lock, "LOCKED", "");
                            broadcast_line_update(line_to_lock);
                        } else {
                            snprintf(log_buf, sizeof(log_buf), "Server: Lock DENIED for line %d (locked by user %d)", 
                                    line_to_lock, document[line_to_lock].locked_by);
                            log_message(log_buf);
                        }
                    }
                    
                    MPI_Send(&response, 1, MPI_INT, source_rank, TAG_LOCK_RESPONSE, MPI_COMM_WORLD);
                    break;
                }
                
                case TAG_EDIT_SUBMIT: {
                    int edited_line_idx;
                    MPI_Recv(&edited_line_idx, 1, MPI_INT, source_rank, TAG_EDIT_SUBMIT, MPI_COMM_WORLD, &status);
                    MPI_Recv(text_buffer, MAX_LEN, MPI_CHAR, source_rank, TAG_EDIT_SUBMIT, MPI_COMM_WORLD, &status);
                    
                    char log_buf[MAX_LEN + 100];
                    snprintf(log_buf, sizeof(log_buf), "Server: Edit submission for line %d from user %d", 
                            edited_line_idx, source_rank);
                    log_message(log_buf);
                    
                    if (edited_line_idx >= 0 && edited_line_idx < current_num_lines &&
                        document[edited_line_idx].locked_by == source_rank) {
                        
                        strncpy(document[edited_line_idx].text, text_buffer, MAX_LEN - 1);
                        document[edited_line_idx].text[MAX_LEN - 1] = '\0';
                        document[edited_line_idx].locked_by = NO_LOCK;
                        
                        log_edit_action(source_rank, edited_line_idx, "EDITED and UNLOCKED", text_buffer);
                        broadcast_line_update(edited_line_idx);
                    } else {
                        snprintf(log_buf, sizeof(log_buf), "Server: Edit REJECTED for line %d from user %d (invalid permissions)", 
                                edited_line_idx, source_rank);
                        log_message(log_buf);
                    }
                    break;
                }
                
                case TAG_CHAT_MESSAGE: {
                    MPI_Recv(text_buffer, MAX_LEN, MPI_CHAR, source_rank, TAG_CHAT_MESSAGE, MPI_COMM_WORLD, &status);
                    
                    char log_buf[MAX_LEN + 50];
                    snprintf(log_buf, sizeof(log_buf), "Server: Public chat from user %d: %s", source_rank, text_buffer);
                    log_message(log_buf);
                    
                    // Retransmite para todos os outros clientes
                    for (int i = 1; i < num_procs; i++) {
                        if (i != source_rank) {
                            char chat_fwd_msg[MAX_LEN + 50];
                            snprintf(chat_fwd_msg, sizeof(chat_fwd_msg), "[Chat Publico - Usuario %d]: %s", source_rank, text_buffer);
                            MPI_Send(chat_fwd_msg, strlen(chat_fwd_msg) + 1, MPI_CHAR, i, TAG_CHAT_MESSAGE, MPI_COMM_WORLD);
                        }
                    }
                    break;
                }
                
                case TAG_AUTODATA_SUBMIT: {
                    int submitted_num_lines;
                    MPI_Recv(&submitted_num_lines, 1, MPI_INT, source_rank, TAG_AUTODATA_SUBMIT, MPI_COMM_WORLD, &status);
                    
                    char log_buf[150];
                    snprintf(log_buf, sizeof(log_buf), "Server: AutoData submission from user %d with %d lines", 
                            source_rank, submitted_num_lines);
                    log_message(log_buf);
                    
                    if (submitted_num_lines > MAX_LINES) submitted_num_lines = MAX_LINES;
                    current_num_lines = submitted_num_lines;
                    
                    for (int i = 0; i < current_num_lines; i++) {
                        MPI_Recv(&document[i], sizeof(Line), MPI_BYTE, source_rank, TAG_AUTODATA_SUBMIT, MPI_COMM_WORLD, &status);
                        document[i].locked_by = NO_LOCK;
                    }
                    
                    log_message("Server: AutoData applied successfully. Broadcasting new document.");
                    broadcast_full_document(num_procs);
                    break;
                }
            }
        }
        
        usleep(10000); // 10ms para não sobrecarregar CPU
    }
}

// Envio de mensagem privada P2P
void send_private_message(int my_rank, int target_rank, const char* message) {
    PrivateMessage pm;
    pm.sender_rank = my_rank;
    pm.target_rank = target_rank;
    strncpy(pm.message, message, MAX_LEN - 1);
    pm.message[MAX_LEN - 1] = '\0';
    
    MPI_Send(&pm, sizeof(PrivateMessage), MPI_BYTE, target_rank, TAG_PRIVATE_MESSAGE, MPI_COMM_WORLD);
    
    char log_buf[MAX_LEN + 100];
    snprintf(log_buf, sizeof(log_buf), "Sent PRIVATE message to user %d: %s", target_rank, message);
    log_message(log_buf);
}

// Tratamento de mensagens recebidas (cliente)
void client_handle_incoming_messages(int my_rank) {
    MPI_Status status;
    int flag = 0;
    char chat_buffer[MAX_LEN + 50];
    int updated_line_idx;
    PrivateMessage pm;
    
    // Verifica mensagens privadas P2P
    MPI_Iprobe(MPI_ANY_SOURCE, TAG_PRIVATE_MESSAGE, MPI_COMM_WORLD, &flag, &status);
    if (flag) {
        MPI_Recv(&pm, sizeof(PrivateMessage), MPI_BYTE, status.MPI_SOURCE, TAG_PRIVATE_MESSAGE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        
        printf("\n*** MENSAGEM PRIVADA de Usuario %d ***\n", pm.sender_rank);
        printf(">>> %s\n", pm.message);
        printf("*******************************************\n");
        
        char log_buf[MAX_LEN + 100];
        snprintf(log_buf, sizeof(log_buf), "Received PRIVATE message from user %d: %s", pm.sender_rank, pm.message);
        log_message(log_buf);
    }
    
    // Verifica atualizações de linha específica
    MPI_Iprobe(0, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
    if (flag && (status.MPI_TAG == TAG_LINE_UPDATE_BCAST || status.MPI_TAG == TAG_FULL_DOC_BCAST_HEADER)) {
        if (status.MPI_TAG == TAG_LINE_UPDATE_BCAST) {
            // Participa do broadcast de linha específica
            MPI_Bcast(&updated_line_idx, 1, MPI_INT, 0, MPI_COMM_WORLD);
            MPI_Bcast(&document[updated_line_idx], sizeof(Line), MPI_BYTE, 0, MPI_COMM_WORLD);
            
            printf("\n[NOTIFICACAO] Linha %d foi atualizada por outro usuario!\n", updated_line_idx);
            log_edit_action(0, updated_line_idx, "received update for", document[updated_line_idx].text);
        } else if (status.MPI_TAG == TAG_FULL_DOC_BCAST_HEADER) {
            // Participa do broadcast do documento completo
            MPI_Bcast(&current_num_lines, 1, MPI_INT, 0, MPI_COMM_WORLD);
            for (int i = 0; i < current_num_lines; i++) {
                MPI_Bcast(&document[i], sizeof(Line), MPI_BYTE, 0, MPI_COMM_WORLD);
            }
            printf("\n[NOTIFICACAO] Documento completo foi atualizado pelo servidor!\n");
            log_message("Client: Received full document update.");
        }
    }
    
    // Verifica mensagens de chat público
    MPI_Iprobe(0, TAG_CHAT_MESSAGE, MPI_COMM_WORLD, &flag, &status);
    if (flag) {
        MPI_Recv(chat_buffer, sizeof(chat_buffer), MPI_CHAR, 0, TAG_CHAT_MESSAGE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        printf("\n%s\n", chat_buffer);
        log_message(chat_buffer);
    }
}

// Loop principal do cliente
void client_main_loop(int my_rank, int num_procs) {
    // Recebe documento inicial
    MPI_Bcast(&current_num_lines, 1, MPI_INT, 0, MPI_COMM_WORLD);
    for (int i = 0; i < current_num_lines; i++) {
        MPI_Bcast(&document[i], sizeof(Line), MPI_BYTE, 0, MPI_COMM_WORLD);
    }
    
    printf("=== CLIENTE (Usuario %d) CONECTADO ===\n", my_rank);
    printf("Documento inicial recebido com %d linhas.\n", current_num_lines);
    printf("Logs sendo salvos em: log_usuario_%d.txt\n", my_rank);
    printf("====================================\n");
    
    int opc;
    char input_buffer[MAX_LEN];
    
    while (1) {
        client_handle_incoming_messages(my_rank);
        print_document_client(my_rank);
        
        printf("\n=== USUARIO %d - MENU PRINCIPAL ===\n", my_rank);
        printf("1 - Editar linha\n");
        printf("2 - Enviar mensagem publica (Chat)\n");
        printf("3 - Gerar dados automaticos (OpenMP)\n");
        printf("4 - Enviar mensagem privada\n");
        printf("0 - Sair\n");
        printf("===================================\n");
        printf("Escolha: ");
        
        if (scanf("%d", &opc) != 1) {
            while(getchar() != '\n');
            opc = -1;
        }
        while(getchar() != '\n');
        
        switch (opc) {
            case 1: { // Editar linha
                printf("Digite o numero da linha para editar (0-%d): ", current_num_lines - 1);
                int line_idx;
                if (scanf("%d", &line_idx) != 1) {
                    while(getchar() != '\n');
                    printf("Entrada invalida.\n");
                    break;
                }
                while(getchar() != '\n');
                
                if (line_idx < 0 || line_idx >= current_num_lines) {
                    printf("Linha invalida! Deve ser entre 0 e %d.\n", current_num_lines - 1);
                    break;
                }
                
                printf("Solicitando bloqueio para linha %d...\n", line_idx);
                MPI_Send(&line_idx, 1, MPI_INT, 0, TAG_LOCK_REQUEST, MPI_COMM_WORLD);
                log_edit_action(my_rank, line_idx, "requested lock for", "");
                
                int lock_response;
                MPI_Recv(&lock_response, 1, MPI_INT, 0, TAG_LOCK_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                
                if (lock_response == 1) {
                    printf("BLOQUEIO CONCEDIDO! Editando linha %d...\n", line_idx);
                    printf("Texto atual: %s\n", document[line_idx].text);
                    printf("Digite o novo texto: ");
                    fgets(input_buffer, MAX_LEN, stdin);
                    input_buffer[strcspn(input_buffer, "\n")] = 0;
                    
                    MPI_Send(&line_idx, 1, MPI_INT, 0, TAG_EDIT_SUBMIT, MPI_COMM_WORLD);
                    MPI_Send(input_buffer, strlen(input_buffer) + 1, MPI_CHAR, 0, TAG_EDIT_SUBMIT, MPI_COMM_WORLD);
                    
                    log_edit_action(my_rank, line_idx, "submitted edit for", input_buffer);
                    printf("Edicao submetida! Aguardando confirmacao...\n");
                } else {
                    printf("BLOQUEIO NEGADO! Linha %d ja esta sendo editada por outro usuario.\n", line_idx);
                    log_edit_action(my_rank, line_idx, "lock request DENIED for", "");
                }
                break;
            }
            
            case 2: { // Chat público
                printf("Digite a mensagem publica: ");
                fgets(input_buffer, MAX_LEN, stdin);
                input_buffer[strcspn(input_buffer, "\n")] = 0;
                
                MPI_Send(input_buffer, strlen(input_buffer) + 1, MPI_CHAR, 0, TAG_CHAT_MESSAGE, MPI_COMM_WORLD);
                printf("Mensagem publica enviada!\n");
                log_message("Client: Sent public chat message.");
                break;
            }
            
            case 3: { // Geração automática
                printf("Iniciando geracao automatica de dados com OpenMP...\n");
                printf("Numero de threads OpenMP disponiveis: %d\n", omp_get_max_threads());
                
                Line temp_doc[MAX_LINES];
                int temp_num_lines = current_num_lines;
                memcpy(temp_doc, document, sizeof(Line) * temp_num_lines);
                
                double start_time = omp_get_wtime();
                gerar_dados_automaticos_omp(temp_doc, temp_num_lines, my_rank);
                double end_time = omp_get_wtime();
                
                printf("Geracao concluida em %.4f segundos!\n", end_time - start_time);
                printf("Submetendo documento gerado ao servidor...\n");
                
                MPI_Send(&temp_num_lines, 1, MPI_INT, 0, TAG_AUTODATA_SUBMIT, MPI_COMM_WORLD);
                for (int i = 0; i < temp_num_lines; i++) {
                    MPI_Send(&temp_doc[i], sizeof(Line), MPI_BYTE, 0, TAG_AUTODATA_SUBMIT, MPI_COMM_WORLD);
                }
                
                log_message("Client: Submitted auto-generated data to server.");
                printf("Dados submetidos! Aguarde o broadcast do servidor.\n");
                break;
            }
            
            case 4: { // Mensagem privada
                printf("Digite o numero do usuario destinatario (1-%d, exceto voce %d): ", num_procs - 1, my_rank);
                int target_user;
                if (scanf("%d", &target_user) != 1) {
                    while(getchar() != '\n');
                    printf("Entrada invalida.\n");
                    break;
                }
                while(getchar() != '\n');
                
                if (target_user <= 0 || target_user >= num_procs || target_user == my_rank) {
                    printf("Usuario invalido! Deve ser entre 1 e %d (exceto voce: %d).\n", num_procs - 1, my_rank);
                    break;
                }
                
                printf("Digite a mensagem privada para o usuario %d: ", target_user);
                fgets(input_buffer, MAX_LEN, stdin);
                input_buffer[strcspn(input_buffer, "\n")] = 0;
                
                send_private_message(my_rank, target_user, input_buffer);
                printf("Mensagem privada enviada para usuario %d!\n", target_user);
                break;
            }
            
            case 0:
                printf("Saindo do editor colaborativo...\n");
                log_message("Client: Exiting collaborative editor.");
                return;
                
            default:
                printf("Opcao invalida! Tente novamente.\n");
        }
        
        printf("\nPressione ENTER para continuar...");
        getchar();
    }
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    
    if (size < 2) {
        if (rank == 0) {
            fprintf(stderr, "ERRO: Este programa precisa de pelo menos 2 processos!\n");
            fprintf(stderr, "Usage: mpirun -np <N> %s (onde N >= 2)\n", argv[0]);
        }
        MPI_Finalize();
        return 1;
    }
    
    // Configuração do OpenMP
    omp_set_num_threads(4); // Define 4 threads para OpenMP
    
    // Abrir arquivo de log
    char nome_log[64];
    snprintf(nome_log, 64, "log_usuario_%d.txt", rank);
    log_file = fopen(nome_log, "w");
    
    if (!log_file) {
        printf("ERRO: Nao foi possivel criar arquivo de log %s!\n", nome_log);
        MPI_Abort(MPI_COMM_WORLD, 1);
        return 1;
    }
    
    char initial_log_msg[200];
    snprintf(initial_log_msg, sizeof(initial_log_msg), 
            "=== EDITOR COLABORATIVO INICIADO ===\nProcess %d of %d started.\nOpenMP threads: %d\nLog file: %s", 
            rank, size, omp_get_max_threads(), nome_log);
    log_message(initial_log_msg);
    
    if (rank == 0) {
        server_main_loop(rank, size);
    } else {
        client_main_loop(rank, size);
    }
    
    if (log_file) {
        log_message("=== EDITOR COLABORATIVO FINALIZADO ===");
        fclose(log_file);
    }
    
    MPI_Finalize();
    return 0;
}