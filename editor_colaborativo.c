#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <omp.h>
#include <unistd.h> // Para usleep
#include <time.h>   // Para logs com timestamp

#define MAX_LINHAS 50
#define MAX_TAM_LINHA 256
#define MAX_MSG_PRIVADA 200 // Tamanho máximo para mensagens privadas
#define LOG_BUFFER_SIZE (MAX_TAM_LINHA * 3) // Buffer para construir mensagens de log (aumentado para segurança)

// --- Definição de Tags para Comunicação MPI ---
#define TAG_SOLICITAR_EDICAO    10 // Cliente -> Servidor: Quero editar esta linha
#define TAG_RESPOSTA_EDICAO     11 // Servidor -> Cliente: Linha concedida/negada
#define TAG_ATUALIZACAO_TEXTO   12 // Servidor -> Todos (Bcast): Nova versão da linha
#define TAG_MSG_PRIVADA_SEND    13 // Cliente -> Outro Cliente/Servidor: Mensagem privada
#define TAG_LIBERAR_LINHA       14 // Cliente -> Servidor: Edição concluída, nova linha
#define TAG_SAIR                15 // Cliente -> Servidor: Cliente quer sair

// --- Estruturas de Mensagem ---

// Mensagem para solicitação/resposta de edição e liberação
typedef struct {
    int linha;
    char conteudo[MAX_TAM_LINHA]; // Usado para enviar o novo conteúdo ou apenas para solicitação
    int sucesso; // 1 = sucesso, 0 = falha (linha ocupada)
    int rank_solicitante; // Rank do processo que está solicitando/editando
} MensagemEdicao;

// Mensagem para comunicação privada
typedef struct {
    int remetente_rank;
    char mensagem[MAX_MSG_PRIVADA];
} MensagemPrivada;


// --- Variáveis Globais (compartilhadas por threads OpenMP, mas consistência MPI) ---
// O array 'texto' e 'linha_em_uso' são as cópias locais.
// A cópia "verdadeira" do estado de 'linha_em_uso' e do 'texto' está no servidor (rank 0).
char texto[MAX_LINHAS][MAX_TAM_LINHA];
// Para o servidor: 0 = livre, rank_do_cliente = ocupada
// Para os clientes: A cópia local 'linha_em_uso' não é a fonte da verdade sobre quem ocupa.
// Eles dependem da resposta do servidor. No entanto, é atualizada via broadcast do texto
// para manter a visualização.
int linha_em_uso[MAX_LINHAS];


// --- Funções Auxiliares ---

// Gera um grande volume de dados para teste usando OpenMP
void gerar_texto_com_openmp() {
    printf("[Servidor] Gerando texto inicial com OpenMP...\n");
    #pragma omp parallel for
    for (int i = 0; i < MAX_LINHAS; i++) {
        sprintf(texto[i], "Linha gerada automaticamente %d (thread %d)", i, omp_get_thread_num());
        linha_em_uso[i] = 0; // Inicialmente todas as linhas estão livres no servidor
    }
    printf("[Servidor] Geração de texto concluída.\n");
}

// Imprime o texto atual. Cada processo imprime sua cópia local.
void imprimir_texto(int rank) {
    printf("\n--- Texto Atual (P%d) ---\n", rank);
    for (int i = 0; i < MAX_LINHAS; i++) {
        // No servidor, mostra quem ocupa a linha. Nos clientes, apenas o texto.
        if (rank == 0 && linha_em_uso[i] != 0) {
            printf("[%02d]: %s (OCUPADA por P%d)\n", i, texto[i], linha_em_uso[i]);
        } else {
            printf("[%02d]: %s\n", i, texto[i]);
        }
    }
    printf("--------------------------\n");
}

// Imprime logs de alteração (servidor é o principal ponto de log)
void log_evento(const char* tipo, int rank, const char* mensagem_formatada) {
    time_t timer;
    char buffer_tempo[26];
    struct tm* tm_info;

    time(&timer);
    tm_info = localtime(&timer);
    strftime(buffer_tempo, 26, "%Y-%m-%d %H:%M:%S", tm_info);

    printf("[%s] [P%d] %s: %s\n", buffer_tempo, rank, tipo, mensagem_formatada);
    fflush(stdout); // Garante que o log seja impresso imediatamente
}

// --- Funções do Servidor ---
void servidor_loop(int rank, int size) {
    char log_buffer[LOG_BUFFER_SIZE]; // Buffer para construir mensagens de log

    gerar_texto_com_openmp(); // Geração de dados com OpenMP
    log_evento("Servidor", rank, "Texto inicial gerado.");
    imprimir_texto(rank);

    MPI_Bcast(texto, MAX_LINHAS * MAX_TAM_LINHA, MPI_CHAR, 0, MPI_COMM_WORLD);
    log_evento("Servidor", rank, "Texto inicial broadcastado para clientes.");

    MPI_Status status;
    MensagemEdicao msg_edicao;
    MensagemPrivada msg_priv;
    int running = 1;

    while (running) {
        int flag;
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);

        if (flag) {
            if (status.MPI_TAG == TAG_SOLICITAR_EDICAO) {
                MPI_Recv(&msg_edicao, sizeof(MensagemEdicao), MPI_BYTE, status.MPI_SOURCE, TAG_SOLICITAR_EDICAO, MPI_COMM_WORLD, &status);
                int linha = msg_edicao.linha;
                int cliente_rank = status.MPI_SOURCE;

                if (linha < 0 || linha >= MAX_LINHAS) {
                    msg_edicao.sucesso = 0; // Linha inválida
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Solicitação de edição de linha inválida (%d) recebida de P%d.", linha, cliente_rank);
                    log_evento("Servidor", rank, log_buffer);
                } else if (linha_em_uso[linha] != 0) { // Linha ocupada
                    msg_edicao.sucesso = 0; // Falha
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Requisição de edição da linha %d negada a P%d (ocupada por P%d).", linha, cliente_rank, linha_em_uso[linha]);
                    log_evento("Servidor", rank, log_buffer);
                } else { // Linha livre
                    linha_em_uso[linha] = cliente_rank; // Marca a linha como ocupada pelo cliente
                    msg_edicao.sucesso = 1; // Sucesso
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Concedeu edição da linha %d para P%d.", linha, cliente_rank);
                    log_evento("Servidor", rank, log_buffer);
                }
                MPI_Send(&msg_edicao, sizeof(MensagemEdicao), MPI_BYTE, cliente_rank, TAG_RESPOSTA_EDICAO, MPI_COMM_WORLD);

            } else if (status.MPI_TAG == TAG_LIBERAR_LINHA) {
                MPI_Recv(&msg_edicao, sizeof(MensagemEdicao), MPI_BYTE, status.MPI_SOURCE, TAG_LIBERAR_LINHA, MPI_COMM_WORLD, &status);
                int linha = msg_edicao.linha;
                int cliente_rank = status.MPI_SOURCE;

                if (linha_em_uso[linha] == cliente_rank) { // Verifica se quem libera é quem ocupa
                    strcpy(texto[linha], msg_edicao.conteudo); // Atualiza o texto
                    linha_em_uso[linha] = 0; // Libera a linha
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Linha %d atualizada por P%d: \"%s\"", linha, cliente_rank, msg_edicao.conteudo);
                    log_evento("Servidor", rank, log_buffer);

                    // --- Comunicação coletiva/em grupo no padrão MPI ---
                    // Notificar todos os clientes sobre a atualização
                    MPI_Bcast(&msg_edicao, sizeof(MensagemEdicao), MPI_BYTE, 0, MPI_COMM_WORLD);
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Broadcast de atualização da linha %d para todos os clientes.", linha);
                    log_evento("Servidor", rank, log_buffer);
                } else {
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Erro: P%d tentou liberar linha %d que não estava ocupando (ocupada por P%d).", cliente_rank, linha, linha_em_uso[linha]);
                    log_evento("Servidor", rank, log_buffer);
                }
            } else if (status.MPI_TAG == TAG_MSG_PRIVADA_SEND) {
                MPI_Recv(&msg_priv, sizeof(MensagemPrivada), MPI_BYTE, status.MPI_SOURCE, TAG_MSG_PRIVADA_SEND, MPI_COMM_WORLD, &status);
                snprintf(log_buffer, LOG_BUFFER_SIZE, "Mensagem privada de P%d: \"%s\" (destinada ao servidor)", msg_priv.remetente_rank, msg_priv.mensagem);
                log_evento("Servidor", rank, log_buffer);
                // Se o servidor deveria encaminhar a mensagem para outro cliente,
                // a lógica de MPI_Send para o destino real seria implementada aqui.
            } else if (status.MPI_TAG == TAG_SAIR) {
                MPI_Recv(&msg_edicao, sizeof(MensagemEdicao), MPI_BYTE, status.MPI_SOURCE, TAG_SAIR, MPI_COMM_WORLD, &status);
                snprintf(log_buffer, LOG_BUFFER_SIZE, "P%d solicitou sair.", status.MPI_SOURCE);
                log_evento("Servidor", rank, log_buffer);
            }
        }
        usleep(1000); // Pequena pausa para evitar spin-lock excessivo na CPU
    }
}

// --- Funções do Cliente ---
void cliente_loop(int rank, int size) {
    char log_buffer[LOG_BUFFER_SIZE]; // Buffer para construir mensagens de log

    // Recebe o texto inicial do servidor (Broadcast)
    MPI_Bcast(texto, MAX_LINHAS * MAX_TAM_LINHA, MPI_CHAR, 0, MPI_COMM_WORLD);
    log_evento("Cliente", rank, "Texto inicial recebido do servidor.");

    int running = 1;
    while (running) {
        printf("\n[Processo %d] Menu:\n", rank);
        printf("1. Editar linha\n2. Mandar msg privada\n3. Ver texto\n4. Sair\n> ");

        int opcao;
        if (scanf("%d", &opcao) != 1) { // Verifica se a leitura foi bem sucedida
            printf("[P%d] Entrada inválida. Digite um número.\n", rank);
            while (getchar() != '\n'); // Limpa o buffer de entrada
            continue;
        }
        getchar(); // Limpa o caractere de nova linha (\n) do buffer após scanf

        if (opcao == 1) { // Editar linha
            int linha;
            printf("[P%d] Digite o número da linha para editar (0 a %d): ", rank, MAX_LINHAS - 1);
            if (scanf("%d", &linha) != 1) {
                printf("[P%d] Entrada inválida para a linha.\n", rank);
                while (getchar() != '\n');
                continue;
            }
            getchar(); // Limpa o \n

            if (linha < 0 || linha >= MAX_LINHAS) {
                printf("[P%d] Linha inválida. Digite um número entre 0 e %d.\n", rank, MAX_LINHAS - 1);
                continue;
            }

            MensagemEdicao solicitacao;
            solicitacao.linha = linha;
            solicitacao.rank_solicitante = rank;

            MPI_Send(&solicitacao, sizeof(MensagemEdicao), MPI_BYTE, 0, TAG_SOLICITAR_EDICAO, MPI_COMM_WORLD);
            snprintf(log_buffer, LOG_BUFFER_SIZE, "Solicitou edição da linha %d ao servidor.", linha);
            log_evento("Cliente", rank, log_buffer);

            MPI_Recv(&solicitacao, sizeof(MensagemEdicao), MPI_BYTE, 0, TAG_RESPOSTA_EDICAO, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            if (solicitacao.sucesso) {
                printf("[P%d] Permissão concedida para editar a linha %d. Conteúdo atual: \"%s\"\n", rank, linha, texto[linha]);
                printf("[P%d] Digite o novo conteúdo: ", rank);
                char nova_linha[MAX_TAM_LINHA];
                fgets(nova_linha, MAX_TAM_LINHA, stdin);
                nova_linha[strcspn(nova_linha, "\n")] = 0; // Remove o \n

                strcpy(solicitacao.conteudo, nova_linha);
                solicitacao.linha = linha; // Garante que a linha está correta na mensagem de volta

                MPI_Send(&solicitacao, sizeof(MensagemEdicao), MPI_BYTE, 0, TAG_LIBERAR_LINHA, MPI_COMM_WORLD);
                snprintf(log_buffer, LOG_BUFFER_SIZE, "Enviou linha %d atualizada para o servidor.", linha);
                log_evento("Cliente", rank, log_buffer);
                strcpy(texto[linha], nova_linha); // Atualiza sua cópia local imediatamente
            } else {
                printf("[P%d] Não foi possível editar a linha %d. Ela está ocupada.\n", rank, linha);
            }
        } else if (opcao == 2) { // Mandar msg privada
            int destino;
            printf("[P%d] Digite o rank do destinatário (0 a %d, exceto %d): ", rank, size - 1, rank);
            if (scanf("%d", &destino) != 1 || destino < 0 || destino >= size || destino == rank) {
                printf("[P%d] Destinatário inválido.\n", rank);
                while (getchar() != '\n');
                continue;
            }
            getchar(); // Limpa o \n

            MensagemPrivada msg_para_enviar;
            msg_para_enviar.remetente_rank = rank;
            printf("[P%d] Digite a mensagem: ", rank);
            fgets(msg_para_enviar.mensagem, MAX_MSG_PRIVADA, stdin);
            msg_para_enviar.mensagem[strcspn(msg_para_enviar.mensagem, "\n")] = 0; // Remove o \n

            MPI_Send(&msg_para_enviar, sizeof(MensagemPrivada), MPI_BYTE, destino, TAG_MSG_PRIVADA_SEND, MPI_COMM_WORLD);
            snprintf(log_buffer, LOG_BUFFER_SIZE, "Mensagem enviada para P%d.", destino);
            log_evento("Cliente", rank, log_buffer);
        } else if (opcao == 3) { 
            imprimir_texto(rank);
        } else if (opcao == 4) { 
            snprintf(log_buffer, LOG_BUFFER_SIZE, "Encerrando...");
            log_evento("Cliente", rank, log_buffer);
            MensagemEdicao sair_msg; 
            sair_msg.rank_solicitante = rank;
            MPI_Send(&sair_msg, sizeof(MensagemEdicao), MPI_BYTE, 0, TAG_SAIR, MPI_COMM_WORLD);
            running = 0; 
        } else {
            printf("[P%d] Opção inválida. Tente novamente.\n", rank);
        }

        MPI_Status status;
        int flag;
        while (1) {
            MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
            if (!flag) break; 

            if (status.MPI_TAG == TAG_ATUALIZACAO_TEXTO) {
                MensagemEdicao atualizacao_recebida;
                MPI_Recv(&atualizacao_recebida, sizeof(MensagemEdicao), MPI_BYTE, 0, TAG_ATUALIZACAO_TEXTO, MPI_COMM_WORLD, &status);
                strcpy(texto[atualizacao_recebida.linha], atualizacao_recebida.conteudo);
                printf("\n[P%d - ATUALIZAÇÃO RECEBIDA] Linha %d agora é: \"%s\"\n", rank, atualizacao_recebida.linha, texto[atualizacao_recebida.linha]);
                snprintf(log_buffer, LOG_BUFFER_SIZE, "Linha %d atualizada por broadcast.", atualizacao_recebida.linha);
                log_evento("Cliente", rank, log_buffer);
            } else if (status.MPI_TAG == TAG_MSG_PRIVADA_SEND) {
                MensagemPrivada msg_recebida;
                MPI_Recv(&msg_recebida, sizeof(MensagemPrivada), MPI_BYTE, status.MPI_SOURCE, TAG_MSG_PRIVADA_SEND, MPI_COMM_WORLD, &status);
                printf("\n[P%d - PRIVADO de P%d]: %s\n", rank, msg_recebida.remetente_rank, msg_recebida.mensagem);
                snprintf(log_buffer, LOG_BUFFER_SIZE, "Mensagem privada recebida de P%d.", msg_recebida.remetente_rank);
                log_evento("Cliente", rank, log_buffer);
            }
            
        }
        usleep(1000); 
    }
}


int main(int argc, char** argv) {
    int rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2) {
        printf("Este programa requer no mínimo 2 processos MPI (1 servidor e 1 cliente).\n");
        MPI_Finalize();
        return 1;
    }

    if (rank == 0) {
        servidor_loop(rank, size);
    } else {
        cliente_loop(rank, size);
    }
    MPI_Finalize();
    return 0;
}