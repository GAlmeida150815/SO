# --- Variáveis ---
CC = gcc
CFLAGS = -Wall -pthread -g
OBJ_COMMON = common/data.h

# --- Targets ---
all: controlador cliente veiculo

controlador: controller.c $(OBJ_COMMON)
	$(CC) $(CFLAGS) controller.c -o controlador

cliente: client.c $(OBJ_COMMON)
	$(CC) $(CFLAGS) client.c -o cliente

veiculo: vehicle.c $(OBJ_COMMON)
	$(CC) $(CFLAGS) vehicle.c -o veiculo

clean:
	rm -f controlador cliente veiculo
	rm -f /tmp/taxi_*