#include <mpi.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINES 100
#define MAX_LEN 256
#define LOCKED -1

// Mensagens MPI tags
#define TAG_EDIT 1
#define TAG_REQUEST 2
#define TAG_RESPONSE 3
#define TAG_BROADCAST 4

// Estrutura para armazenar a linha do texto e status do bloqueio
typedef struct {
    char text[MAX_LEN];
    int locked_by; // rank do usuário que bloqueou a linha, -1 se livre
} Line;

Line document[MAX_LINES];
int num_lines = 10; // começa com 10 linhas

FILE *log_file;

// Função para logar alterações
void log_change(int user, int line, const char* new_text) {
    fprintf(log_file, "User %d edited line %d: %s\n", user, line, new_text);
    fflush(log_file);
}

// Inicializa documento com texto default
void init_document() {
    for (int i=0; i<num_lines; i++) {
        snprintf(document[i].text, MAX_LEN, "Linha %d inicial.", i);
        document[i].locked_by = LOCKED;
    }
}

// Imprime documento na tela
void print_document(int rank) {
    printf("\n[Usuário %d] Documento Atual:\n", rank);
    for (int i=0; i<num_lines; i++) {
        printf("%3d [%s] %s\n", i, (document[i].locked_by == LOCKED) ? "Livre" : (document[i].locked_by == rank ? "Você" : "Bloqueado"), document[i].text);
    }
}

// Função que gera dados automaticamente (simulando alterações) com OpenMP
void gerar_dados_automaticos(int rank) {
    #pragma omp parallel for
    for (int i=0; i<num_lines; i++) {
        // Simula edição: adiciona " (auto-edit)" em cada linha
        char new_text[MAX_LEN];
        snprintf(new_text, MAX_LEN, "%s (auto-edit por %d)", document[i].text, rank);
        #pragma omp critical
        {
            strcpy(document[i].text, new_text);
            document[i].locked_by = LOCKED;
            log_change(rank, i, new_text);
        }
    }
}

// Função para enviar broadcast do documento para todos usuários
void broadcast_document(int root, int size) {
    for (int i=0; i<num_lines; i++) {
        MPI_Bcast(document[i].text, MAX_LEN, MPI_CHAR, root, MPI_COMM_WORLD);
        MPI_Bcast(&document[i].locked_by, 1, MPI_INT, root, MPI_COMM_WORLD);
    }
}

// Recebe broadcast para atualizar documento localmente
void receive_broadcast_document(int root) {
    for (int i=0; i<num_lines; i++) {
        MPI_Bcast(document[i].text, MAX_LEN, MPI_CHAR, root, MPI_COMM_WORLD);
        MPI_Bcast(&document[i].locked_by, 1, MPI_INT, root, MPI_COMM_WORLD);
    }
}

// Envia pedido privado para outro usuário (ex: "Faça ajuste na linha X: zzz")
void enviar_pedido_privado(int meu_rank, int total_users) {
    int dest;
    char msg[MAX_LEN];
    printf("Digite o rank do usuário para enviar o pedido (0-%d, exceto %d): ", total_users-1, meu_rank);
    scanf("%d", &dest);
    if (dest == meu_rank || dest < 0 || dest >= total_users) {
        printf("Destino inválido!\n");
        return;
    }
    getchar(); // limpa buffer
    printf("Digite o pedido: ");
    fgets(msg, MAX_LEN, stdin);
    MPI_Send(msg, strlen(msg)+1, MPI_CHAR, dest, TAG_REQUEST, MPI_COMM_WORLD);
    printf("Pedido enviado para usuário %d\n", dest);
}

// Recebe pedidos privados
void receber_pedidos_privados(int meu_rank) {
    MPI_Status status;
    int flag;
    char msg[MAX_LEN];

    MPI_Iprobe(MPI_ANY_SOURCE, TAG_REQUEST, MPI_COMM_WORLD, &flag, &status);
    while(flag) {
        MPI_Recv(msg, MAX_LEN, MPI_CHAR, status.MPI_SOURCE, TAG_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        printf("\n[Usuário %d] Pedido privado de %d: %s\n", meu_rank, status.MPI_SOURCE, msg);
        MPI_Iprobe(MPI_ANY_SOURCE, TAG_REQUEST, MPI_COMM_WORLD, &flag, &status);
    }
}

// Função para editar linha (tentando bloquear e editar)
void editar_linha(int meu_rank) {
    int linha;
    printf("Digite o número da linha para editar: ");
    scanf("%d", &linha);
    if (linha < 0 || linha >= num_lines) {
        printf("Linha inválida\n");
        return;
    }

    if (document[linha].locked_by != LOCKED) {
        printf("Linha já está bloqueada por usuário %d!\n", document[linha].locked_by);
        return;
    }

    // Bloqueia linha para edição
    document[linha].locked_by = meu_rank;
    printf("Linha %d bloqueada para edição.\n", linha);

    getchar(); // limpa buffer
    printf("Digite o novo texto: ");
    char novo_texto[MAX_LEN];
    fgets(novo_texto, MAX_LEN, stdin);
    novo_texto[strcspn(novo_texto, "\n")] = 0;  // remove \n

    // Atualiza texto e desbloqueia
    strcpy(document[linha].text, novo_texto);
    document[linha].locked_by = LOCKED;
    log_change(meu_rank, linha, novo_texto);

    printf("Linha %d atualizada e desbloqueada.\n", linha);
}

// Função para sincronizar documento entre todos usuários (broadcast pelo rank 0)
void sincronizar_documento(int rank, int size) {
    if (rank == 0) {
        broadcast_document(rank, size);
    } else {
        receive_broadcast_document(0);
    }
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    // Abre arquivo de log (cada processo cria seu próprio)
    char nome_log[64];
    snprintf(nome_log, 64, "log_usuario_%d.txt", rank);
    log_file = fopen(nome_log, "w");
    if (!log_file) {
        printf("Erro ao abrir arquivo de log!\n");
        MPI_Finalize();
        return 1;
    }

    if (rank == 0) {
        init_document();
    }

    // Sincroniza documento inicial para todos
    sincronizar_documento(rank, size);

    int opc;
    while (1) {
        receber_pedidos_privados(rank);

        print_document(rank);
        printf("\nUsuário %d - Menu:\n", rank);
        printf("1 - Editar linha\n");
        printf("2 - Enviar pedido privado\n");
        printf("3 - Gerar dados automáticos (OpenMP)\n");
        printf("4 - Sincronizar documento\n");
        printf("0 - Sair\n");
        printf("Escolha: ");
        scanf("%d", &opc);

        switch (opc) {
            case 1:
                editar_linha(rank);
                sincronizar_documento(0, size);
                break;
            case 2:
                enviar_pedido_privado(rank, size);
                break;
            case 3:
                gerar_dados_automaticos(rank);
                sincronizar_documento(0, size);
                break;
            case 4:
                sincronizar_documento(rank, size);
                break;
            case 0:
                fclose(log_file);
                MPI_Finalize();
                exit(0);
                break;
            default:
                printf("Opção inválida\n");
        }
    }

    return 0;
}