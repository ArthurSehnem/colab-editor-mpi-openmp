#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <omp.h>
#include <unistd.h>
#include <time.h>

#define MAX_LINHAS 10
#define MAX_TAM_LINHA 256
#define MAX_MSG_PRIVADA 200
#define LOG_BUFFER_SIZE (MAX_TAM_LINHA * 3)

#define TAG_SOLICITAR_EDICAO    10
#define TAG_RESPOSTA_EDICAO     11
#define TAG_ATUALIZACAO_TEXTO   12
#define TAG_MSG_PRIVADA_SEND    13
#define TAG_LIBERAR_LINHA       14
#define TAG_SAIR                15
#define TAG_TEXTO_INICIAL       16

typedef struct {
    int linha;
    char conteudo[MAX_TAM_LINHA];
    int sucesso;
    int rank_solicitante;
} MensagemEdicao;

typedef struct {
    int remetente_rank;
    int destino_rank;
    char mensagem[MAX_MSG_PRIVADA];
} MensagemPrivada;

char texto[MAX_LINHAS][MAX_TAM_LINHA];
int linha_em_uso[MAX_LINHAS];
int clientes_ativos;

void gerar_texto_com_openmp() {
    printf("[Servidor] Gerando texto inicial com OpenMP...\n");
    #pragma omp parallel for
    for (int i = 0; i < MAX_LINHAS; i++) {
        sprintf(texto[i], "Linha gerada automaticamente %d (thread %d)", i, omp_get_thread_num());
        linha_em_uso[i] = 0;
    }
    printf("[Servidor] Geração de texto concluída.\n");
}

void imprimir_texto(int rank) {
    printf("\n--- Texto Atual (P%d) ---\n", rank);
    for (int i = 0; i < MAX_LINHAS; i++) {
        if (rank == 0 && linha_em_uso[i] != 0) {
            printf("[%02d]: %s (OCUPADA por P%d)\n", i, texto[i], linha_em_uso[i]);
        } else {
            printf("[%02d]: %s\n", i, texto[i]);
        }
    }
    printf("--------------------------\n");
}

void log_evento(const char* tipo, int rank, const char* mensagem_formatada) {
    time_t timer;
    char buffer_tempo[26];
    struct tm* tm_info;

    time(&timer);
    tm_info = localtime(&timer);
    strftime(buffer_tempo, 26, "%Y-%m-%d %H:%M:%S", tm_info);

    printf("[%s] [P%d] %s: %s\n", buffer_tempo, rank, tipo, mensagem_formatada);
    fflush(stdout);
}

void servidor_loop(int rank, int size) {
    char log_buffer[LOG_BUFFER_SIZE];
    clientes_ativos = size - 1;

    gerar_texto_com_openmp();
    log_evento("Servidor", rank, "Texto inicial gerado.");
    imprimir_texto(rank);

    for (int i = 1; i < size; i++) {
        MPI_Send(texto, MAX_LINHAS * MAX_TAM_LINHA, MPI_CHAR, i, TAG_TEXTO_INICIAL, MPI_COMM_WORLD);
        snprintf(log_buffer, LOG_BUFFER_SIZE, "Texto inicial enviado para P%d.", i);
        log_evento("Servidor", rank, log_buffer);
    }

   //MPI_Bcast(texto, MAX_LINHAS * MAX_TAM_LINHA, MPI_CHAR, 0, MPI_COMM_WORLD);
   //log_evento("Servidor", rank, "Texto inicial broadcastado para clientes.");

    MPI_Status status;
    MensagemEdicao msg_edicao;
    MensagemPrivada msg_priv;

    while (clientes_ativos > 0) {
        int flag;
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);

        if (flag) {
            if (status.MPI_TAG == TAG_SOLICITAR_EDICAO) {
                MPI_Recv(&msg_edicao, sizeof(MensagemEdicao), MPI_BYTE, status.MPI_SOURCE, TAG_SOLICITAR_EDICAO, MPI_COMM_WORLD, &status);
                int linha = msg_edicao.linha;
                int cliente_rank = status.MPI_SOURCE;

                if (linha < 0 || linha >= MAX_LINHAS) {
                    msg_edicao.sucesso = 0;
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Solicitação de linha inválida (%d) de P%d.", linha, cliente_rank);
                    log_evento("Servidor", rank, log_buffer);
                } else if (linha_em_uso[linha] != 0) {
                    msg_edicao.sucesso = 0;
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Linha %d negada a P%d (ocupada por P%d).", linha, cliente_rank, linha_em_uso[linha]);
                    log_evento("Servidor", rank, log_buffer);
                } else {
                    linha_em_uso[linha] = cliente_rank;
                    msg_edicao.sucesso = 1;
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Linha %d concedida para P%d.", linha, cliente_rank);
                    log_evento("Servidor", rank, log_buffer);
                }
                MPI_Send(&msg_edicao, sizeof(MensagemEdicao), MPI_BYTE, cliente_rank, TAG_RESPOSTA_EDICAO, MPI_COMM_WORLD);

            } else if (status.MPI_TAG == TAG_LIBERAR_LINHA) {
                MPI_Recv(&msg_edicao, sizeof(MensagemEdicao), MPI_BYTE, status.MPI_SOURCE, TAG_LIBERAR_LINHA, MPI_COMM_WORLD, &status);
                int linha = msg_edicao.linha;
                int cliente_rank = status.MPI_SOURCE;

                if (linha >= 0 && linha < MAX_LINHAS && linha_em_uso[linha] == cliente_rank) {
                    strcpy(texto[linha], msg_edicao.conteudo);
                    linha_em_uso[linha] = 0;

                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Linha %d atualizada por P%d: \"%s\"", linha, cliente_rank, msg_edicao.conteudo);
                    log_evento("Servidor", rank, log_buffer);

                    MPI_Request request_atualizacao[size];
                    int num_requests = 0;

                    for (int i = 1; i < size; i++) {
                        MPI_Isend(&msg_edicao, sizeof(MensagemEdicao), MPI_BYTE, i, TAG_ATUALIZACAO_TEXTO, MPI_COMM_WORLD, &request_atualizacao[num_requests]);
                        num_requests++;
                    }
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Atualização da linha %d iniciada para todos os clientes (via Isend).", linha);
                    log_evento("Servidor", rank, log_buffer);

                } else {
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Erro: P%d tentou liberar ou atualizar linha %d de forma inválida (não era o proprietário ou linha fora do limite).", cliente_rank, linha);
                    log_evento("Servidor", rank, log_buffer);
                }

            } else if (status.MPI_TAG == TAG_MSG_PRIVADA_SEND) {
                MPI_Recv(&msg_priv, sizeof(MensagemPrivada), MPI_BYTE, status.MPI_SOURCE, TAG_MSG_PRIVADA_SEND, MPI_COMM_WORLD, &status);

                snprintf(log_buffer, LOG_BUFFER_SIZE, "Mensagem privada de P%d para P%d: \"%s\"", msg_priv.remetente_rank, msg_priv.destino_rank, msg_priv.mensagem);
                log_evento("Servidor", rank, log_buffer);

                if (msg_priv.destino_rank > 0 && msg_priv.destino_rank < size && msg_priv.destino_rank != rank) {
                    MPI_Send(&msg_priv, sizeof(MensagemPrivada), MPI_BYTE, msg_priv.destino_rank, TAG_MSG_PRIVADA_SEND, MPI_COMM_WORLD);
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Mensagem encaminhada de P%d para P%d.", msg_priv.remetente_rank, msg_priv.destino_rank);
                    log_evento("Servidor", rank, log_buffer);
                } else {
                    snprintf(log_buffer, LOG_BUFFER_SIZE, "Destinatário inválido (P%d) para mensagem de P%d. Mensagem descartada.", msg_priv.destino_rank, msg_priv.remetente_rank);
                    log_evento("Servidor", rank, log_buffer);
                }

            } else if (status.MPI_TAG == TAG_SAIR) {
                MPI_Recv(&msg_edicao, sizeof(MensagemEdicao), MPI_BYTE, status.MPI_SOURCE, TAG_SAIR, MPI_COMM_WORLD, &status);
                clientes_ativos--;
                snprintf(log_buffer, LOG_BUFFER_SIZE, "P%d solicitou sair. Clientes ativos restantes: %d.", status.MPI_SOURCE, clientes_ativos);
                log_evento("Servidor", rank, log_buffer);
            }
        }
        usleep(1000);
    }
    log_evento("Servidor", rank, "Todos os clientes saíram. Encerrando o servidor.");
}

void cliente_loop(int rank, int size) {
    char log_buffer[LOG_BUFFER_SIZE];
    MPI_Status status;

    MPI_Recv(texto, MAX_LINHAS * MAX_TAM_LINHA, MPI_CHAR, 0, TAG_TEXTO_INICIAL, MPI_COMM_WORLD, &status);
    log_evento("Cliente", rank, "Texto inicial recebido do servidor.");

    int sair = 0;

    #pragma omp parallel sections
    {
        #pragma omp section
        {
            while (!sair) {
                int flag;
                MPI_Status status;

                MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
                if (flag) {
                    if (status.MPI_TAG == TAG_ATUALIZACAO_TEXTO) {
                        MensagemEdicao atualizacao;
                        MPI_Recv(&atualizacao, sizeof(MensagemEdicao), MPI_BYTE, 0, TAG_ATUALIZACAO_TEXTO, MPI_COMM_WORLD, &status);
                        if (atualizacao.linha >= 0 && atualizacao.linha < MAX_LINHAS) {
                            strcpy(texto[atualizacao.linha], atualizacao.conteudo);
                            printf("\n*** [P%d] ATUALIZAÇÃO em tempo real: Linha %02d = \"%s\" ***\n", rank, atualizacao.linha, atualizacao.conteudo);
                            snprintf(log_buffer, LOG_BUFFER_SIZE, "Linha %d atualizada via servidor.", atualizacao.linha);
                            log_evento("Cliente", rank, log_buffer);
                        }
                    } else if (status.MPI_TAG == TAG_MSG_PRIVADA_SEND) {
                        MensagemPrivada msg_recebida;
                        MPI_Recv(&msg_recebida, sizeof(MensagemPrivada), MPI_BYTE, 0, TAG_MSG_PRIVADA_SEND, MPI_COMM_WORLD, &status);
                        printf("\n*** [P%d] MENSAGEM PRIVADA de P%d: %s ***\n", rank, msg_recebida.remetente_rank, msg_recebida.mensagem);
                        snprintf(log_buffer, LOG_BUFFER_SIZE, "Mensagem privada recebida de P%d.", msg_recebida.remetente_rank);
                        log_evento("Cliente", rank, log_buffer);
                    }
                }

                usleep(1000);
            }
        }

        #pragma omp section
        {
            while (!sair) {
                int opcao;
                char input_buffer[20];

                printf("\n======= [Processo %d] Menu =======\n", rank);
                printf("1. Editar linha\n");
                printf("2. Mandar msg privada\n");
                printf("3. Ver texto\n");
                printf("4. Sair\n");
                printf("Opção > ");

                if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) continue;
                if (sscanf(input_buffer, "%d", &opcao) != 1) continue;

                if (opcao == 1) {
                    printf("\n--- Texto Atual (sua cópia) ---\n");
                    for (int i = 0; i < MAX_LINHAS; i++) {
                        printf("[%02d]: %s\n", i, texto[i]);
                    }

                    int linha;
                    printf("[P%d] Número da linha para editar (0 a %d): ", rank, MAX_LINHAS - 1);
                    if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL || sscanf(input_buffer, "%d", &linha) != 1) continue;
                    if (linha < 0 || linha >= MAX_LINHAS) continue;

                    MensagemEdicao solicitacao;
                    solicitacao.linha = linha;
                    solicitacao.rank_solicitante = rank;
                    MPI_Send(&solicitacao, sizeof(MensagemEdicao), MPI_BYTE, 0, TAG_SOLICITAR_EDICAO, MPI_COMM_WORLD);
                    log_evento("Cliente", rank, "Solicitou edição da linha.");

                    MPI_Recv(&solicitacao, sizeof(MensagemEdicao), MPI_BYTE, 0, TAG_RESPOSTA_EDICAO, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                    if (solicitacao.sucesso) {
                        printf("[P%d] ✓ Permissão concedida para linha %02d! Conteúdo atual: \"%s\"\n", rank, linha, texto[linha]);
                        printf("[P%d] Novo conteúdo para linha %02d: ", rank, linha);

                        char nova_linha[MAX_TAM_LINHA];
                        fgets(nova_linha, MAX_TAM_LINHA, stdin);
                        nova_linha[strcspn(nova_linha, "\n")] = 0;

                        strcpy(solicitacao.conteudo, nova_linha);
                        solicitacao.linha = linha;
                        MPI_Send(&solicitacao, sizeof(MensagemEdicao), MPI_BYTE, 0, TAG_LIBERAR_LINHA, MPI_COMM_WORLD);
                        log_evento("Cliente", rank, "Enviou linha atualizada.");
                        printf("[P%d] ✓ Linha enviada para o servidor!\n", rank);
                    } else {
                        printf("[P%d] ✗ Linha %02d está ocupada. Tente novamente depois.\n", rank, linha);
                    }

                } else if (opcao == 2) {
                    int destino;
                    printf("[P%d] Rank do destinatário (1 a %d): ", rank, size - 1);
                    if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL || sscanf(input_buffer, "%d", &destino) != 1) continue;
                    if (destino <= 0 || destino >= size || destino == rank) continue;

                    MensagemPrivada msg;
                    msg.remetente_rank = rank;
                    msg.destino_rank = destino;

                    printf("[P%d] Digite a mensagem: ", rank);
                    fgets(msg.mensagem, MAX_MSG_PRIVADA, stdin);
                    msg.mensagem[strcspn(msg.mensagem, "\n")] = 0;

                    MPI_Send(&msg, sizeof(MensagemPrivada), MPI_BYTE, 0, TAG_MSG_PRIVADA_SEND, MPI_COMM_WORLD);
                    log_evento("Cliente", rank, "Mensagem privada enviada.");
                    printf("[P%d] ✓ Mensagem enviada!\n", rank);

                } else if (opcao == 3) {
                    imprimir_texto(rank);

                } else if (opcao == 4) {
                    log_evento("Cliente", rank, "Solicitando sair...");
                    MensagemEdicao sair_msg;
                    sair_msg.rank_solicitante = rank;
                    MPI_Send(&sair_msg, sizeof(MensagemEdicao), MPI_BYTE, 0, TAG_SAIR, MPI_COMM_WORLD);
                    sair = 1;
                }
                usleep(1000);
            }
        }
    }
}

int main(int argc, char** argv) {
    int rank, size;

    //MPI_Init(&argc, &argv);
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        printf("Erro: MPI não suporta múltiplas threads nesse ambiente.\n");
        MPI_Finalize();
        return 1;
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2) {
        printf("Este programa requer no mínimo 2 processos MPI (1 servidor e pelo menos 1 cliente).\n");
        MPI_Finalize();
        return 1;
    }

    printf("[P%d] Iniciando... (Total: %d processos)\n", rank, size);

    if (rank == 0) {
        printf("[P%d] Executando como SERVIDOR\n", rank);
        servidor_loop(rank, size);
    } else {
        printf("[P%d] Executando como CLIENTE\n", rank);
        cliente_loop(rank, size);
    }

    printf("[P%d] Finalizando...\n", rank);
    MPI_Finalize();
    return 0;
}
