#include "common/data.h"

// --- Variáveis Globais ---
volatile int running = 1;
volatile int service_cancelled = 0;
int vehicle_id;
char local_partida[100];
double distancia_km;
int client_pid;
int service_id;
int telemetry_fd = -1;

// --- Handlers ---
void trata_sinal(int sinal) {
    if (sinal == SIGUSR1) {
        service_cancelled = 1;
        running = 0;
    }
}

// --- Protótipos ---
void contact_client();
void send_telemetry(const char* message);
void open_telemetry_pipe();
void close_telemetry_pipe();

// --- Main ---
int main(int argc, char *argv[]) {
    // Argumentos: ./veiculo <id> <service_id> <client_pid> <local_partida> <distancia_km>
    if (argc != 6) {
        fprintf(stderr, "[VEICULO] Erro: Uso ./veiculo <id> <service_id> <client_pid> <local> <distancia>\n");
        return 1;
    }

    vehicle_id = atoi(argv[1]);
    service_id = atoi(argv[2]);
    client_pid = atoi(argv[3]);
    strcpy(local_partida, argv[4]);
    distancia_km = atof(argv[5]);

    // Configurar Sinais
    signal(SIGUSR1, trata_sinal);
    
    printf("\r\033[K[VEICULO %d] Iniciado para serviço ID %d (%.1f km)\nCMD> ", vehicle_id, service_id, distancia_km);
    fflush(stdout);

    // Abrir pipe de telemetria
    open_telemetry_pipe();

    // 1. Contactar cliente (chegou ao local de partida) e viagem inicia automaticamente
    contact_client();
    
    if (!running) {
        send_telemetry("CANCELLED");
        close_telemetry_pipe();
        return 0;
    }

    // 2. Cliente entra automaticamente - enviar notificação
    char start_msg[256];
    sprintf(start_msg, "TRIP_STARTED|%d|%d", vehicle_id, service_id);
    send_telemetry(start_msg);
    
    //TODO REMOVER , é o controladro que tem de dizer isto
    printf("\r\033[K[VEICULO %d] Viagem iniciada!\nCMD> ", vehicle_id);
    fflush(stdout);
    sleep(1);
    
    if (!running) {
        send_telemetry("CANCELLED");
        close_telemetry_pipe();
        return 0;
    }

    // 3. Simular viagem
    int percent = 0;
    double time_per_step = distancia_km / 10.0; 
    
    while (running && percent < 100) {
        sleep((int)time_per_step);
        
        if (!running) break;
        
        percent += 10;
        printf("\r\033[K[VEICULO %d] Progresso: %d%%\nCMD> ", vehicle_id, percent);
        fflush(stdout);
        
        // Enviar progresso ao controlador
        char progress_msg[256];
        sprintf(progress_msg, "PROGRESS|%d|%d|%d", vehicle_id, service_id, percent);
        send_telemetry(progress_msg);
        
        // Enviar quilómetros percorridos
        double km_done = (percent / 100.0) * distancia_km;
        char km_msg[256];
        //TODO
        sprintf(km_msg, "DISTANCE|%d|%d|%.2f", vehicle_id, service_id, km_done);
        send_telemetry(km_msg);
    }

    // 4. Reportar conclusão
    if (service_cancelled) {
        printf("\r\033[K[VEICULO %d] Serviço cancelado (progresso: %d%%)\nCMD> ", vehicle_id, percent);
        fflush(stdout);
        send_telemetry("CANCELLED");
    } else if (percent >= 100) {
        printf("\r\033[K[VEICULO %d] Viagem concluída! Total: %.1f km\nCMD> ", vehicle_id, distancia_km);
        fflush(stdout);
        char complete_msg[256];
        sprintf(complete_msg, "COMPLETED|%d|%d|%.1f", vehicle_id, service_id, distancia_km);
        send_telemetry(complete_msg);
    }
    
    close_telemetry_pipe();
    return 0;
}

// --- Contactar Cliente ---
void contact_client() {
    char pipe_client_path[50];
    sprintf(pipe_client_path, PIPE_CLIENT_FMT, client_pid);

    // Tentar contactar cliente via pipez
    int fd = open(pipe_client_path, O_WRONLY | O_NONBLOCK);
    if (fd != -1) {
        ControllerResponse msg;
        msg.success = 1;
        sprintf(msg.message, "Veículo %d chegou a '%s'. A viagem está a iniciar!", 
                vehicle_id, local_partida);
        write(fd, &msg, sizeof(ControllerResponse));
        close(fd);
        printf("\r\033[K[VEICULO %d] Cliente contactado (PID: %d)\nCMD> ", vehicle_id, client_pid);
    } else {
        printf("\r\033[K[VEICULO %d] Não foi possível contactar cliente (PID: %d)\nCMD> ", vehicle_id, client_pid);
    }
    fflush(stdout);
}

// --- Abrir Pipe de Telemetria ---
void open_telemetry_pipe() {
    char pipe_path[50];
    sprintf(pipe_path, PIPE_VEHICLE_FMT, vehicle_id);
    
    // Tentar abrir em modo bloqueante primeiro (aguarda leitor)
    // Se falhar, tentar não-bloqueante
    telemetry_fd = open(pipe_path, O_WRONLY);
    if (telemetry_fd == -1) {
        // Se falhar em modo bloqueante, tentar não-bloqueante
        telemetry_fd = open(pipe_path, O_WRONLY | O_NONBLOCK);
        if (telemetry_fd == -1) {
            printf("\r\033[K[VEICULO %d] AVISO: Não foi possível abrir pipe de telemetria\nCMD> ", vehicle_id);
            fflush(stdout);
        }
    }
}

// --- Fechar Pipe de Telemetria ---
void close_telemetry_pipe() {
    if (telemetry_fd != -1) {
        close(telemetry_fd);
        telemetry_fd = -1;
    }
}

// --- Enviar Telemetria ---
void send_telemetry(const char* message) {
    if (telemetry_fd != -1) {
        write(telemetry_fd, message, strlen(message));
        write(telemetry_fd, "\n", 1);
    }
}