#include "common/data.h"
#include <string.h>

// --- Constantes Internas ---
#define MAX_CLIENTS 10
#define MAX_VEHICLES 10
#define MAX_SERVICES 50

// --- Variáveis Globais ---
ClientInfo clients[MAX_CLIENTS];
VehicleInfo vehicles[MAX_VEHICLES];
ServiceInfo services[MAX_SERVICES];
int vehicle_telemetry_fds[MAX_VEHICLES];
int telemetry_pipe_read = -1;
int telemetry_pipe_write = -1;
int num_clients = 0;
int num_vehicles = 0;
int num_services = 0;
int next_service_id = 1;
int simulated_time = 0; // em segundos
int keep_running = 1;
pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER; 

// --- Protótipos ---
void* client_listener_thread(void* arg);
void* time_simulator_thread(void* arg);
void* scheduler_thread(void* arg);
void* vehicle_telemetry_thread(void* arg);
void process_admin_commands();
void handle_login(ClientMessage msg);
void handle_client_exit(ClientMessage msg);
void handle_ride_request(ClientMessage msg);
void handle_cancel_request(ClientMessage msg);
void handle_consult_request(ClientMessage msg);
void send_response(int client_pid, int success, char* text);
void broadcast_shutdown();
void cleanup_and_exit(int signal);
void init_vehicles();
void launch_vehicle(int service_index);
void process_vehicle_telemetry(char* line, int vehicle_id);
int find_available_vehicle();
void cmd_listar();
void cmd_utiliz();
void cmd_frota();
void cmd_cancelar(int service_id);
void cmd_km();
void cmd_hora();

// --- Main ---
int main(int argc, char *argv[]) {
    printf("[CONTROLADOR] A iniciar sistema...\n");

    // Tratamento de Sinais (CTRL+C)
    signal(SIGINT, cleanup_and_exit);

    // Criar pipe anónimo para telemetria
    int pipe_fds[2];
    if (pipe(pipe_fds) == -1) {
        perror("[CONTROLADOR] Erro ao criar pipe de telemetria");
        exit(1);
    }
    telemetry_pipe_read = pipe_fds[0];
    telemetry_pipe_write = pipe_fds[1];
    fcntl(telemetry_pipe_read, F_SETFL, O_NONBLOCK);

    // Validar ambiente
    if(getenv("NVEICULOS") == NULL) {
        printf("[CONTROLADOR] AVISO: NVEICULOS não definido. A usar padrão (%d).\n", MAX_VEHICLES);
        char nveiculos_str[12];
        sprintf(nveiculos_str, "%d", MAX_VEHICLES);
        setenv("NVEICULOS", nveiculos_str, 1);
    }

    // Inicializar veículos
    init_vehicles();

    // Criar Pipe Principal
    if (mkfifo(PIPE_SERVER, 0666) == -1 && errno != EEXIST) {
        perror("[CONTROLADOR] Erro ao criar pipe servidor");
        exit(1);
    }

    // Thread Clientes
    pthread_t t_client, t_time, t_scheduler;
    if (pthread_create(&t_client, NULL, client_listener_thread, NULL) != 0) {
        perror("[CONTROLADOR] Erro thread clientes");
        exit(1);
    }

    // Thread Tempo Simulado
    if (pthread_create(&t_time, NULL, time_simulator_thread, NULL) != 0) {
        perror("[CONTROLADOR] Erro thread tempo");
        exit(1);
    }

    // Thread Scheduler 
    if (pthread_create(&t_scheduler, NULL, scheduler_thread, NULL) != 0) {
        perror("[CONTROLADOR] Erro thread scheduler");
        exit(1);
    }

    // Thread Telemetria de Veículos
    pthread_t t_telemetry;
    if (pthread_create(&t_telemetry, NULL, vehicle_telemetry_thread, NULL) != 0) {
        perror("[CONTROLADOR] Erro thread telemetria");
        exit(1);
    }

    process_admin_commands();

    cleanup_and_exit(0);
    return 0;
}


// F--- Helpers ---
const char* get_request_type_name(RequestType type) {
    switch (type) {
        case LOGIN_REQ:     return "LOGIN";
        case RIDE_REQ:      return "TRANSPORTE";
        case CANCEL_REQ:    return "CANCELAR";
        case TERMINATE_REQ: return "TERMINAR";
        default:            return "DESCONHECIDO";
    }
}

// --- Thread de Leitura ---
void* client_listener_thread(void* arg) {
    int fd = open(PIPE_SERVER, O_RDWR); 
    if (fd == -1) return NULL;

    ClientMessage msg;
    while (keep_running) {
        if (read(fd, &msg, sizeof(ClientMessage)) > 0) {
            //!DEBUG
            //printf("\r\033[K[CONTROLADOR] Recebido pedido [%s] de %s (PID %d)\n", 
            //    get_request_type_name(msg.type), msg.client_name, msg.client_pid);
            //printf("CMD> "); fflush(stdout);
            
            // Processamento seguro com Mutex
            pthread_mutex_lock(&data_mutex);
            
            switch (msg.type) {
                case LOGIN_REQ:
                    handle_login(msg);
                    break;
                case RIDE_REQ:
                    handle_ride_request(msg);
                    break;
                case CANCEL_REQ:
                    handle_cancel_request(msg);
                    break;
                case CONSULT_REQ:
                    handle_consult_request(msg);
                    break;
                case TERMINATE_REQ:
                    handle_client_exit(msg);
                    break;
                default:
                    break;
            }
            
            pthread_mutex_unlock(&data_mutex);
        }
    }
    close(fd);
    return NULL;
}

// --- Lógica de Login ---
void handle_login(ClientMessage msg) {
    // 1. Verificar se já existe
    for (int i = 0; i < num_clients; i++) {
        if (strcmp(clients[i].name, msg.client_name) == 0) {
            send_response(msg.client_pid, 0, "Username em uso");
            printf("\r\033[K[CONTROLADOR] Login falhou para %s: Username em uso.\nCMD> ", msg.client_name);
            fflush(stdout);
            return;
        }
    }

    // 2. Verificar se cabe
    if (num_clients >= MAX_CLIENTS) {
        send_response(msg.client_pid, 0, "Servidor cheio");
        printf("\r\033[K[CONTROLADOR] Login falhou para %s: Servidor cheio.\nCMD> ", msg.client_name);
        fflush(stdout);
        return;
    }

    // 3. Adicionar no final do array
    clients[num_clients].pid = msg.client_pid;
    strcpy(clients[num_clients].name, msg.client_name);
    clients[num_clients].status = CLIENT_WAITING;
    
    num_clients++; 

    send_response(msg.client_pid, 1, "Bem-vindo!");
    printf("\r\033[K[CONTROLADOR] Cliente %s (PID %d) logado com sucesso. Ativos: %d\nCMD> ", 
        msg.client_name, msg.client_pid, num_clients);
    fflush(stdout);
}

// --- Lógica de Saída do Cliente ---
void handle_client_exit(ClientMessage msg) {
    int encontrado = 0;
    
    // 1. Vrificar se está em viageme
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].pid == msg.client_pid) {
            encontrado = 1;
            
            if (clients[i].status == CLIENT_ON_TRIP) {
                send_response(msg.client_pid, 0, "Não pode sair. Está em viagem!");
                printf("\r\033[K[CONTROLADOR] %s tentou sair mas está em viagem\nCMD> ", msg.client_name);
                fflush(stdout);
                return;
            }
            
            // 2. Cancelar serviços agendados
            int cancelled = 0;
            for (int s = 0; s < num_services; s++) {
                if (services[s].client_pid == msg.client_pid && services[s].status == STATUS_SCHEDULED) {
                    services[s].status = STATUS_CANCELLED;
                    cancelled++;
                }
            }
            
            if (cancelled > 0) {
                printf("\r\033[K[CONTROLADOR] %d serviço(s) agendado(s) cancelado(s) para %s\nCMD> ", 
                       cancelled, msg.client_name);
                fflush(stdout);
            }
            
            // 3. Remover cliente
            for (int j = i; j < num_clients - 1; j++) {
                clients[j] = clients[j + 1];
            }

            num_clients--;
            memset(&clients[num_clients], 0, sizeof(ClientInfo));
            
            send_response(msg.client_pid, 1, "Até breve!");
            printf("\r\033[K[CONTROLADOR] Cliente %s saiu. Ativos: %d\nCMD> ", msg.client_name, num_clients);
            fflush(stdout);
            return;
        }
    }
    
    if (!encontrado) {
        //!DEBUG
        // printf("\r\033[K[DEBUG] Tentativa de logout de PID não encontrado: %d\nCMD> ", msg.client_pid);
        // fflush(stdout);
    }
}

// --- Lógica de Agendamento ---
void handle_ride_request(ClientMessage msg) {
    // Parsear: agendar <hora> <local> <distancia>
    int hora;
    char local[100];
    double distancia;
    
    if (sscanf(msg.data, "%d %99s %lf", &hora, local, &distancia) != 3) {
        send_response(msg.client_pid, 0, "Formato inválido. Use: agendar <hora> <local> <distancia>");
        return;
    }
    
    if (num_services >= MAX_SERVICES) {
        send_response(msg.client_pid, 0, "Limite de serviços atingido");
        return;
    }

    if (hora < simulated_time) {
        char err_msg[BUFFER_SIZE];
        sprintf(err_msg, "Hora inválida. Deve ser no futuro. (Hora atual é %d)", simulated_time);
        send_response(msg.client_pid, 0, err_msg);
        return;
    }
    
    // Verificar se o cliente já tem uma viagem agendada ou em progresso
    for (int i = 0; i < num_services; i++) {
        if (services[i].client_pid == msg.client_pid && 
            (services[i].status == STATUS_SCHEDULED || services[i].status == STATUS_IN_PROGRESS)) {
            send_response(msg.client_pid, 0, "Já tem uma viagem agendada ou em progresso. Aguarde a conclusão.");
            return;
        }
    }
    
    // Criar serviço
    services[num_services].id = next_service_id++;
    strcpy(services[num_services].client_name, msg.client_name);
    services[num_services].client_pid = msg.client_pid;
    services[num_services].scheduled_time = hora;
    strcpy(services[num_services].origem, local);
    strcpy(services[num_services].destino, "");
    services[num_services].vehicle_id = -1;
    services[num_services].status = STATUS_SCHEDULED;
    services[num_services].distance_km = distancia;
    
    char resp[BUFFER_SIZE];
    sprintf(resp, "Serviço agendado com ID %d para %02d:%02d:%02d", 
            services[num_services].id, hora/3600, (hora%3600)/60, hora%60);
    send_response(msg.client_pid, 1, resp);
    
    printf("\r\033[K[CONTROLADOR] Serviço ID %d agendado para %s (hora: %d, dist: %.1fkm)\nCMD> ", 
           services[num_services].id, msg.client_name, hora, distancia);
    fflush(stdout);
    
    num_services++;
}

// --- Lógica de Cancelamento (Cliente) ---
void handle_cancel_request(ClientMessage msg) {
    int service_id = atoi(msg.data);
    
    if (service_id == 0) {
        // Cancelar todos os serviços do cliente
        int cancelled = 0;
        for (int i = 0; i < num_services; i++) {
            if (services[i].client_pid == msg.client_pid && 
                services[i].status == STATUS_SCHEDULED) {
                services[i].status = STATUS_CANCELLED;
                cancelled++;
            }
        }
        
        char resp[BUFFER_SIZE];
        sprintf(resp, "%d serviço(s) cancelado(s)", cancelled);
        send_response(msg.client_pid, 1, resp);
        printf("\r\033[K[CONTROLADOR] %s cancelou %d serviço(s)\nCMD> ", msg.client_name, cancelled);
    } else {
        // Cancelar serviço específico
        int found = 0;
        for (int i = 0; i < num_services; i++) {
            if (services[i].id == service_id && 
                services[i].client_pid == msg.client_pid) {
                found = 1;
                
                if (services[i].status != STATUS_SCHEDULED) {
                    send_response(msg.client_pid, 0, "Serviço não pode ser cancelado (já em execução ou concluído)");
                } else {
                    services[i].status = STATUS_CANCELLED;
                    send_response(msg.client_pid, 1, "Serviço cancelado com sucesso");
                    printf("\r\033[K[CONTROLADOR] Serviço ID %d cancelado por %s\nCMD> ", service_id, msg.client_name);
                }
                break;
            }
        }
        
        if (!found) {
            send_response(msg.client_pid, 0, "Serviço não encontrado ou não pertence a si");
        }
    }
    fflush(stdout);
}

// --- Lógica de Consulta ---
void handle_consult_request(ClientMessage msg) {
    char resp[BUFFER_SIZE * 4] = "[SERVIÇOS]\n";
    int count = 0;
    
    for (int i = 0; i < num_services; i++) {
        if (services[i].client_pid == msg.client_pid && 
            (services[i].status == STATUS_SCHEDULED || services[i].status == STATUS_IN_PROGRESS)) {
            char line[BUFFER_SIZE];
            const char* status_str = (services[i].status == STATUS_SCHEDULED) ? "AGENDADO" : "EM CURSO";
            sprintf(line, "ID:%d | %02d:%02d:%02d | %s (%.1fkm) | %s\n",
                    services[i].id,
                    services[i].scheduled_time/3600,
                    (services[i].scheduled_time%3600)/60,
                    services[i].scheduled_time%60,
                    services[i].origem,
                    services[i].distance_km,
                    status_str);
            strcat(resp, line);
            count++;
        }
    }
    
    if (count == 0) {
        strcpy(resp, "Não tem serviços agendados");
    }
    
    send_response(msg.client_pid, 1, resp);
}

// --- Lógica: Avisar Clientes do Encerramento ---
void broadcast_shutdown() {
    printf("[CONTROLADOR] A avisar clientes do encerramento...\n");
    pthread_mutex_lock(&data_mutex);
    for (int i = 0; i < num_clients; i++) {
        send_response(clients[i].pid, 0, "SERVER_SHUTDOWN");
    }
    pthread_mutex_unlock(&data_mutex);
}

// --- Envio de Resposta ---
void send_response(int client_pid, int success, char* text) {
    char pipe_client_path[50];
    sprintf(pipe_client_path, PIPE_CLIENT_FMT, client_pid);


    //!DEBUG
    //printf("\r\033[K[DEBUG:CONTROLADOR] A tentar abrir pipe: %s\nCMD> ", pipe_client_path);
    //fflush(stdout);

    int fd_cli = open(pipe_client_path, O_WRONLY);
    if (fd_cli == -1) {
        printf("\r\033[K[CONTROLADOR] Erro: Não consegui abrir pipe do cliente %d\nCMD> ", client_pid);
        fflush(stdout);
        return;
    }

    ControllerResponse resp;
    resp.success = success;
    strcpy(resp.message, text);
    
    write(fd_cli, &resp, sizeof(ControllerResponse));
    close(fd_cli);

    //!DEBUG
    //printf("\r\033[K[DEBUG:CONTROLADOR] Resposta enviada para %d: %s\nCMD> ", client_pid, text);
    //fflush(stdout);
}

// --- Inicialização de Veículos ---
void init_vehicles() {
    for (int i = 0; i < MAX_VEHICLES; i++) {
        vehicles[i].id = i + 1;
        vehicles[i].active = VEHICLE_INACTIVE;
        vehicles[i].available = VEHICLE_AVAILABLE;
        vehicles[i].progress_percent = 0;
        vehicles[i].service_id = -1;
        vehicles[i].process_pid = 0;
        vehicles[i].total_km = 0.0;
        vehicle_telemetry_fds[i] = -1;
        
        // Criar pipe de telemetria antecipadamente
        char pipe_path[50];
        sprintf(pipe_path, PIPE_VEHICLE_FMT, vehicles[i].id);
        if (mkfifo(pipe_path, 0666) == -1 && errno != EEXIST) {
            // Pipe já existe ou erro, ignorar
        }
    }
    num_vehicles = MAX_VEHICLES;
    printf("[CONTROLADOR] %d veículos inicializados.\n", num_vehicles);
}

// --- Thread Scheduler ---
void* scheduler_thread(void* arg) {
    while (keep_running) {
        sleep(1);
        
        pthread_mutex_lock(&data_mutex);
        
        // Verificar serviços agendados que devem ser iniciados
        for (int i = 0; i < num_services; i++) {
            if (services[i].status == STATUS_SCHEDULED &&
                services[i].scheduled_time <= simulated_time &&
                services[i].vehicle_id == -1) {
                
                // Encontrar veículo disponível
                int vehicle_idx = find_available_vehicle();
                if (vehicle_idx != -1) {
                    services[i].vehicle_id = vehicles[vehicle_idx].id;
                    services[i].status = STATUS_IN_PROGRESS;
                    vehicles[vehicle_idx].available = VEHICLE_OCCUPIED;
                    vehicles[vehicle_idx].service_id = services[i].id;
                    
                    // Atualizar cliente para em viagem
                    for (int c = 0; c < num_clients; c++) {
                        if (clients[c].pid == services[i].client_pid) {
                            clients[c].status = CLIENT_ON_TRIP;
                            break;
                        }
                    }

                    printf("\r\033[K[CONTROLADOR] Lançando veículo %d para serviço ID %d\nCMD> ", 
                           vehicles[vehicle_idx].id, services[i].id);
                    fflush(stdout);
                    
                    launch_vehicle(i);
                }
            }
        }
        
        pthread_mutex_unlock(&data_mutex);
    }
    return NULL;
}

// --- Encontrar Veículo Disponível ---
int find_available_vehicle() {
    for (int i = 0; i < num_vehicles; i++) {
        if (vehicles[i].available) {
            return i;
        }
    }
    return -1;
}

// --- Lançar Veículo ---
void launch_vehicle(int service_index) {
    ServiceInfo *srv = &services[service_index];
    
    // Criar named pipe para telemetria do veículo
    char pipe_vehicle_path[50];
    sprintf(pipe_vehicle_path, PIPE_VEHICLE_FMT, srv->vehicle_id);
    
    // Remover pipe antigo se existir
    unlink(pipe_vehicle_path);
    
    // Criar novo pipe
    if (mkfifo(pipe_vehicle_path, 0666) == -1) {
        perror("[CONTROLADOR] Erro ao criar pipe de veículo");
        return;
    }
    
    // Resetar o FD da telemetria para este veículo
    int vehicle_idx = srv->vehicle_id - 1;
    if (vehicle_telemetry_fds[vehicle_idx] != -1) {
        close(vehicle_telemetry_fds[vehicle_idx]);
    }
    vehicle_telemetry_fds[vehicle_idx] = -1;
    
    pid_t pid = fork();
    if (pid == -1) {
        perror("[CONTROLADOR] Erro ao fazer fork para veículo");
        return;
    }
    
    if (pid == 0) {
        // Processo filho (veículo)
        
        // Preparar argumentos
        char arg_id[20], arg_service[20], arg_client[20], arg_dist[20];
        sprintf(arg_id, "%d", srv->vehicle_id);
        sprintf(arg_service, "%d", srv->id);
        sprintf(arg_client, "%d", srv->client_pid);
        sprintf(arg_dist, "%.1f", srv->distance_km);
        
        // Executar veículo
        execl("./veiculo", "veiculo", arg_id, arg_service, arg_client, 
              srv->origem, arg_dist, NULL);
        
        perror("\r\033[K[VEICULO] Erro ao executar");
        exit(1);
    } else {
        // Processo pai (controlador)
        
        // Atualizar processo do veículo
        for (int i = 0; i < num_vehicles; i++) {
            if (vehicles[i].id == srv->vehicle_id) {
                vehicles[i].process_pid = pid;
                vehicles[i].active = VEHICLE_ACTIVE;
                break;
            }
        }
    }
}

// --- Thread de Telemetria de Veículos ---
void* vehicle_telemetry_thread(void* arg) {
    char buffer[BUFFER_SIZE];
    
    while (keep_running) {
        // Tentar abrir e ler de cada pipe de veículo (dinâmico)
        int has_data = 0;
        for (int i = 0; i < MAX_VEHICLES; i++) {
            int vehicle_num = i + 1;
            char pipe_path[50];
            sprintf(pipe_path, PIPE_VEHICLE_FMT, vehicle_num);
            
            // Se o FD ainda não foi aberto, tentar abrir
            if (vehicle_telemetry_fds[i] == -1) {
                vehicle_telemetry_fds[i] = open(pipe_path, O_RDONLY | O_NONBLOCK);
            }
            
            // Se o FD está aberto, tentar ler
            if (vehicle_telemetry_fds[i] != -1) {
                ssize_t n = read(vehicle_telemetry_fds[i], buffer, sizeof(buffer) - 1);
                if (n > 0) {
                    has_data = 1;
                    buffer[n] = '\0';
                    
                    // Processar cada linha
                    char* line = strtok(buffer, "\n");
                    while (line != NULL) {
                        if (strlen(line) > 0) {
                            process_vehicle_telemetry(line, vehicle_num);
                        }
                        line = strtok(NULL, "\n");
                    }
                } else if (n < 0 && errno == EBADF) {
                    // Pipe foi fechado
                    close(vehicle_telemetry_fds[i]);
                    vehicle_telemetry_fds[i] = -1;
                }
            }
        }
        
        // Se não houver dados, aguardar um pouco
        if (!has_data) {
            usleep(50000);  // 50ms
        }
    }
    
    // Fechar todos os file descriptors ao terminar
    for (int i = 0; i < MAX_VEHICLES; i++) {
        if (vehicle_telemetry_fds[i] != -1) {
            close(vehicle_telemetry_fds[i]);
            vehicle_telemetry_fds[i] = -1;
        }
    }
    
    if (telemetry_pipe_read != -1) {
        close(telemetry_pipe_read);
    }
    
    return NULL;
}

// --- Processar Telemetria do Veículo ---
void process_vehicle_telemetry(char* line, int vehicle_id) {
    // Formato: TIPO|vehicle_id|service_id|dados...
    char type[50];
    int vid, service_id;
    
    if (sscanf(line, "%49[^|]|%d|%d", type, &vid, &service_id) < 3) {
        if (strcmp(line, "CANCELLED") == 0) {
            strcpy(type, "CANCELLED");
        } else {
            return;
        }
    }
    
    pthread_mutex_lock(&data_mutex);

    if (strcmp(type, "TRIP_STARTED") == 0) {
        // Enviar mensagem ao cliente que a viagem iniciou
        for (int s = 0; s < num_services; s++) {
            if (services[s].id == service_id && services[s].status == STATUS_IN_PROGRESS) {
                send_response(services[s].client_pid, 1, "Viagem iniciada!");
                printf("\r\033[K[CONTROLADOR] Viagem iniciada!\nCMD> ");
                fflush(stdout);
                break;
            }
        }
    } else if (strcmp(type, "PROGRESS") == 0) {
        int percent;
        if (sscanf(line, "%*[^|]|%*d|%*d|%d", &percent) == 1) {
            for (int i = 0; i < num_vehicles; i++) {
                if (vehicles[i].id == vid) {
                    vehicles[i].progress_percent = percent;
                    break;
                }
            }
        }
    } else if (strcmp(type, "DISTANCE") == 0) {
        double km;
        if (sscanf(line, "%*[^|]|%*d|%*d|%lf", &km) == 1) {
            for (int i = 0; i < num_vehicles; i++) {
                if (vehicles[i].id == vid) {
                    double prev_km = vehicles[i].total_km;
                    vehicles[i].total_km = km;
                    
                    printf("\r\033[K[DEBUG] Veículo %d percorreu mais %.1f km. Total: %.1f km\nCMD> ",
                           vid, km - prev_km, km);
                    fflush(stdout);
                    break;
                }
            }
        }
    } else if (strcmp(type, "COMPLETED") == 0 || strcmp(type, "CANCELLED") == 0) {
        
        for (int i = 0; i < num_services; i++) {
            if (services[i].id == service_id) {
                services[i].status = (strcmp(type, "CANCELLED") == 0) ? STATUS_CANCELLED : STATUS_COMPLETED;
                
                for (int c = 0; c < num_clients; c++) {
                    if (clients[c].pid == services[i].client_pid) {
                        clients[c].status = CLIENT_WAITING;

                        char msg[BUFFER_SIZE];
                        if (strcmp(type, "COMPLETED") == 0) {
                            sprintf(msg, "Viagem concluída! Percorridos %.1f km.", services[i].distance_km);
                        } else {
                            sprintf(msg, "Viagem cancelada. Serviço ID %d", services[i].id);
                        }
                        send_response(clients[c].pid, 1, msg);
                        break;
                    }
                }
                break;
            }
        }
        
        for (int i = 0; i < num_vehicles; i++) {
            if (vehicles[i].id == vehicle_id) {
                vehicles[i].available = VEHICLE_AVAILABLE;
                vehicles[i].active = VEHICLE_INACTIVE;
                vehicles[i].progress_percent = 0;
                vehicles[i].service_id = -1;
                vehicles[i].process_pid = 0;
                vehicles[i].total_km = 0.0;  // Resetar KM para a próxima viagem
                
                // Fechar o FD da telemetria
                if (vehicle_telemetry_fds[i] != -1) {
                    close(vehicle_telemetry_fds[i]);
                    vehicle_telemetry_fds[i] = -1;
                }
                
                // Remover pipe
                char pipe_path[50];
                sprintf(pipe_path, PIPE_VEHICLE_FMT, vehicle_id);
                unlink(pipe_path);
                break;
            }
        }
    }
    
    pthread_mutex_unlock(&data_mutex);
}

// --- Thread de Simulação de Tempo ---
void* time_simulator_thread(void* arg) {
    while (keep_running) {
        sleep(1);
        pthread_mutex_lock(&data_mutex);
        simulated_time++;
        pthread_mutex_unlock(&data_mutex);
    }
    return NULL;
}

// --- Comandos Administrativos ---
void cmd_listar() {
    printf("[CONTROLADOR] == SERVIÇOS AGENDADOS ==\n");
    pthread_mutex_lock(&data_mutex);
    
    int count = 0;
    for (int i = 0; i < num_services; i++) {
        if (services[i].status == STATUS_SCHEDULED || services[i].status == STATUS_IN_PROGRESS) {
            const char* status_str = (services[i].status == STATUS_SCHEDULED) ? "AGENDADO" : "EM CURSO";
            printf("  [ID:%d] %s -> %s | Cliente: %s | Veículo: %d | Status: %s\n",
                services[i].id, services[i].origem, services[i].destino,
                services[i].client_name, services[i].vehicle_id, status_str);
            count++;
        }
    }
    
    if (count == 0) {
        printf("  (Nenhum serviço agendado ou em curso)\n");
    }
    
    pthread_mutex_unlock(&data_mutex);
}

void cmd_utiliz() {
    printf("[CONTROLADOR] == UTILIZADORES LIGADOS (%d / %d) ==\n", num_clients, MAX_CLIENTS);
    pthread_mutex_lock(&data_mutex);
    
    for (int i = 0; i < num_clients; i++) {
        const char* status = (clients[i].status == CLIENT_ON_TRIP) ? "EM VIAGEM" : "À ESPERA";
        printf("  - %s (PID: %d) [%s]\n", clients[i].name, clients[i].pid, status);
    }
    
    if (num_clients == 0) {
        printf("  (Nenhum utilizador ligado)\n");
    }
    
    pthread_mutex_unlock(&data_mutex);
}

void cmd_frota() {
    printf("[CONTROLADOR] == ESTADO DA FROTA ==\n");
    pthread_mutex_lock(&data_mutex);
    
    for (int i = 0; i < num_vehicles; i++) {
        if (vehicles[i].available == VEHICLE_AVAILABLE) {
            printf("  [Veículo %d] DISPONÍVEL\n", vehicles[i].id);
        } else {
            printf("  [Veículo %d] EM SERVIÇO - Progresso: %d%% (Serviço ID: %d)\n",
                vehicles[i].id, vehicles[i].progress_percent, vehicles[i].service_id);
        }
    }
    
    pthread_mutex_unlock(&data_mutex);
}

void cmd_cancelar(int service_id) {
    pthread_mutex_lock(&data_mutex);
    
    if (service_id == 0) {
        int cancelled = 0;
        for (int i = 0; i < num_services; i++) {
            if (services[i].status == STATUS_SCHEDULED || services[i].status == STATUS_IN_PROGRESS) {
                services[i].status = STATUS_CANCELLED;
                
                for (int c = 0; c < num_clients; c++) {
                    if (clients[c].pid == services[i].client_pid) {
                        clients[c].status = CLIENT_WAITING;
                        break;
                    }
                }
                
                if (services[i].vehicle_id > 0) {
                    for (int v = 0; v < num_vehicles; v++) {
                        if (vehicles[v].id == services[i].vehicle_id) {
                            vehicles[v].available = VEHICLE_AVAILABLE;
                            vehicles[v].progress_percent = 0;
                            vehicles[v].service_id = -1;
                            
                            if (vehicles[v].process_pid > 0) {
                                kill(vehicles[v].process_pid, SIGUSR1);
                                vehicles[v].process_pid = 0;
                            }
                            break;
                        }
                    }
                }
                
                send_response(services[i].client_pid, 0, "Serviço cancelado");
                cancelled++;
            }
        }
        printf("[CONTROLADOR] %d serviço(s) cancelado(s).\n", cancelled);
    } else {
        int found = 0;
        for (int i = 0; i < num_services; i++) {
            if (services[i].id == service_id && 
                (services[i].status == STATUS_SCHEDULED || services[i].status == STATUS_IN_PROGRESS)) {
                services[i].status = STATUS_CANCELLED;
                found = 1;
                
                // Atualizar cliente
                for (int c = 0; c < num_clients; c++) {
                    if (clients[c].pid == services[i].client_pid) {
                        clients[c].status = CLIENT_WAITING;
                        break;
                    }
                }
                
                if (services[i].vehicle_id > 0) {
                    for (int v = 0; v < num_vehicles; v++) {
                        if (vehicles[v].id == services[i].vehicle_id) {
                            vehicles[v].available = VEHICLE_AVAILABLE;
                            vehicles[v].progress_percent = 0;
                            vehicles[v].service_id = -1;
                            
                            if (vehicles[v].process_pid > 0) {
                                kill(vehicles[v].process_pid, SIGUSR1);
                                vehicles[v].process_pid = 0;
                            }
                            break;
                        }
                    }
                }
                
                send_response(services[i].client_pid, 0, "Serviço cancelado");
                printf("[CONTROLADOR] Serviço ID %d cancelado.\n", service_id);
                break;
            }
        }
        
        if (!found) {
            printf("[CONTROLADOR] Serviço ID %d não encontrado ou já finalizado.\n", service_id);
        }
    }
    
    pthread_mutex_unlock(&data_mutex);
}

void cmd_km() {
    pthread_mutex_lock(&data_mutex);
    
    double total_km = 0.0;
    for (int i = 0; i < num_vehicles; i++) {
        total_km += vehicles[i].total_km;
    }
    
    printf("[CONTROLADOR] Quilómetros totais percorridos: %.2f km\n", total_km);
    
    pthread_mutex_unlock(&data_mutex);
}

void cmd_hora() {
    pthread_mutex_lock(&data_mutex);
    
    int hours = simulated_time / 3600;
    int minutes = (simulated_time % 3600) / 60;
    int seconds = simulated_time % 60;
    
    printf("[CONTROLADOR] Tempo simulado: %02d:%02d:%02d (%d segundos)\n", hours, minutes, seconds, simulated_time);
    
    pthread_mutex_unlock(&data_mutex);
}

// --- Admin ---
void process_admin_commands() {
    char buffer[100];
    while (keep_running) {
        printf("CMD> ");
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) break;
        buffer[strcspn(buffer, "\n")] = 0;

        if (strcmp(buffer, "terminar") == 0) {
            keep_running = 0;
        }
        else if (strcmp(buffer, "listar") == 0) {
            cmd_listar();
        }
        else if (strcmp(buffer, "utiliz") == 0) {
            cmd_utiliz();
        }
        else if (strcmp(buffer, "frota") == 0) {
            cmd_frota();
        }
        else if (strncmp(buffer, "cancelar ", 9) == 0) {
            int service_id = atoi(buffer + 9);
            cmd_cancelar(service_id);
        }
        else if (strcmp(buffer, "km") == 0) {
            cmd_km();
        }
        else if (strcmp(buffer, "hora") == 0) {
            cmd_hora();
        }
        else if (strlen(buffer) > 0) {
            printf("[CONTROLADOR] Comando desconhecido. Comandos disponíveis:\n");
            printf("  listar, utiliz, frota, cancelar <id>, km, hora, terminar\n");
        }
    }
}

// --- Limpeza e Saída ---
void cleanup_and_exit(int signal) {
    if (keep_running) {
        printf("\n[CONTROLADOR] A terminar sistema...\n");
    }

    keep_running = 0;

    unlink(PIPE_SERVER);
    
    // Fechar pipes de telemetria
    if (telemetry_pipe_read != -1) {
        close(telemetry_pipe_read);
    }
    if (telemetry_pipe_write != -1) {
        close(telemetry_pipe_write);
    }
    
    broadcast_shutdown();
    printf("[CONTROLADOR] Encerrado.\n");
    exit(0);
}