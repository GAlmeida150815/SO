#include "common/data.h"

// --- Variáveis Globais ---
char my_pipe_path[50];
int server_fd = -1;
int my_fd = -1;
pid_t my_pid;
char my_name[50];

// Controlo de Login e Threads
volatile int login_status = 0;
volatile int keep_running = 1;
pthread_t t_reader;

// --- Protótipos ---
void cleanup_and_exit(int signal);
void* server_response_listener(void* arg);
void send_request(RequestType type, char* data);

// --- Main ---
int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("[CLIENTE] Erro: Uso ./cliente <nome>\n");
        return 1;
    }

    my_pid = getpid();
    strcpy(my_name, argv[1]);
    
    // Tratamento de Sinais (CTRL+C)
    signal(SIGINT, cleanup_and_exit);

    // 1. Criar Pipe Próprio
    sprintf(my_pipe_path, PIPE_CLIENT_FMT, my_pid);
    if (mkfifo(my_pipe_path, 0666) == -1 && errno != EEXIST) {
        perror("[CLIENTE] Erro ao criar pipe próprio");
        exit(1);
    }

    printf("[CLIENTE %s] Iniciado (PID: %d)...\n", my_name, my_pid);

    // 2. Abrir Pipe Próprio 
    if (pthread_create(&t_reader, NULL, server_response_listener, NULL) != 0) {
        perror("[CLIENTE] Erro ao criar thread de leitura");
        unlink(my_pipe_path);
        exit(1);
    }

    // 3. Conectar ao Servidor
    server_fd = open(PIPE_SERVER, O_WRONLY);
    if (server_fd == -1) {
        printf("[CLIENTE] Erro: Controlador offline.\n");
        keep_running = 0;
        unlink(my_pipe_path);
        return 1;
    }

    // 4. Enviar Login
    send_request(LOGIN_REQ, "");

    // 5. Esperar Resposta do Login
    while (login_status == 0 && keep_running) {
        usleep(10000);
    }

    if (login_status == -1) {
        keep_running = 0;
        cleanup_and_exit(0);
    }

    // 6. Loop Principal (Interface)
    char buffer[256];
    while (keep_running) {
        printf("\r\033[KCMD> ");
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) break;
        buffer[strcspn(buffer, "\n")] = 0;

        if (strcmp(buffer, "terminar") == 0) {
            keep_running = 0;
        }
        else if (strncmp(buffer, "agendar ", 8) == 0) {
            // agendar <hora> <local> <distancia>
            send_request(RIDE_REQ, buffer + 8);
        }
        else if (strncmp(buffer, "cancelar ", 9) == 0) {
            // cancelar <id>
            send_request(CANCEL_REQ, buffer + 9);
        }
        else if (strcmp(buffer, "consultar") == 0) {
            // consultar
            send_request(CONSULT_REQ, "");
        }
        else if (strlen(buffer) > 0) {
            printf("[CLIENTE] Comandos disponíveis:\n");
            printf("  agendar <hora> <local> <distancia>\n");
            printf("  cancelar <id>\n");
            printf("  consultar\n");
            printf("  terminar\n");
        }
    }

    cleanup_and_exit(0);
    return 0;
}

// --- Thread que ouve o Controlador ---
void* server_response_listener(void* arg) {
    my_fd = open(my_pipe_path, O_RDWR); 
    if (my_fd == -1) {
        perror("[CLIENTE THREAD] Erro ao abrir pipe");
        exit(1);
    }

    ControllerResponse resp;
    while (keep_running) {
        if (read(my_fd, &resp, sizeof(ControllerResponse)) > 0) {
            if (strcmp(resp.message, "SERVER_SHUTDOWN") == 0) {
                printf("\n\r\033[K[CLIENTE] O Servidor encerrou. A sair...\n");
                keep_running = 0;
                if (server_fd != -1) close(server_fd);
                if (my_fd != -1) close(my_fd);
                unlink(my_pipe_path);
                exit(0);
            }

            if (login_status == 0) {
                if (resp.success) {
                    printf("\r\033[K[CLIENTE] Login Sucesso: %s\nCMD> ", resp.message);
                    login_status = 1;
                } else {
                    printf("\r\033[K[CLIENTE] Login Falhou: %s\n", resp.message);
                    login_status = -1;
                }
                fflush(stdout);
            } else {
                printf("\r\033[K[CLIENTE] Msg do Server: %s\nCMD> ", resp.message);
                fflush(stdout);
            }
        }
    }
    return NULL;
}

// --- Enviar Pedido ---
void send_request(RequestType type, char* data) {
    ClientMessage msg;
    msg.client_pid = my_pid;
    msg.type = type;
    strcpy(msg.client_name, my_name);
    strcpy(msg.data, data ? data : "");
    
    if (write(server_fd, &msg, sizeof(ClientMessage)) == -1) {
        perror("[CLIENTE] Erro ao enviar (Server morreu?)");
        keep_running = 0;
    }
}

// --- Limpeza e Saída ---
void cleanup_and_exit(int signal) {
    if (keep_running) {
        printf("\n[CLIENTE] A terminar sessão...\n");
        send_request(TERMINATE_REQ, "");
    }

    keep_running = 0;
    if (server_fd != -1) close(server_fd);
    if (my_fd != -1) close(my_fd);
    unlink(my_pipe_path);
    exit(0);
}