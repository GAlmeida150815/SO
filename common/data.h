#ifndef DATA_H
#define DATA_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

// --- Constantes de Comunicação ---
#define PIPE_SERVER "/tmp/server_pipe"
#define PIPE_CLIENT_FMT "/tmp/cli_%d" 
#define PIPE_VEHICLE_FMT "/tmp/veic_%d"

#define BUFFER_SIZE 256

// --- Tipos de Pedidos ---
typedef enum {
    LOGIN_REQ,
    RIDE_REQ,
    CANCEL_REQ,
    CONSULT_REQ,
    TERMINATE_REQ
} RequestType;

// --- Estados do Serviço ---
typedef enum {
    STATUS_SCHEDULED = 0,
    STATUS_IN_PROGRESS = 1,
    STATUS_COMPLETED = 2,
    STATUS_CANCELLED = 3
} ServiceStatus;

// --- Estados do Cliente ---
typedef enum {
    CLIENT_WAITING = 0,
    CLIENT_ON_TRIP = 1
} ClientStatus;

// --- Estados do Veículo ---
typedef enum {
    VEHICLE_INACTIVE = 0,
    VEHICLE_ACTIVE = 1
} VehicleActiveStatus;

typedef enum {
    VEHICLE_OCCUPIED = 0,
    VEHICLE_AVAILABLE = 1
} VehicleAvailability;

// --- Estrutura Mensagem (Cliente -> Controlador) ---
typedef struct {
    pid_t client_pid;
    char client_name[50];
    RequestType type;
    char data[BUFFER_SIZE]; 
} ClientMessage;

// --- Estrutura Resposta (Controlador -> Cliente) ---
typedef struct {
    int success;
    char message[BUFFER_SIZE];
} ControllerResponse;

// --- Estruturas de Dados do Sistema ---
typedef struct {
    int pid;
    char name[50];
    ClientStatus status;
} ClientInfo;

typedef struct {
    int id;
    VehicleActiveStatus active;
    VehicleAvailability available;
    int progress_percent;  // 0-100
    int service_id;  // -1 se não atribuído
    pid_t process_pid;
    double total_km;
} VehicleInfo;

typedef struct {
    int id;
    char client_name[50];
    int client_pid;
    int scheduled_time;  // em segundos
    char origem[100];
    char destino[100];
    int vehicle_id;  // -1 se não atribuído
    ServiceStatus status;
    double distance_km;
} ServiceInfo;

#endif