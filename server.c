#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PUERTO 12345
#define MAX_CLIENTES 100
#define BUFFER_SIZE 2048

int clientes[MAX_CLIENTES];
int contador_clientes = 0;
pthread_mutex_t clientes_mutex = PTHREAD_MUTEX_INITIALIZER;

void enviar_a_todos(char *mensaje, int emisor_fd) {
    pthread_mutex_lock(&clientes_mutex);
    for (int i = 0; i < contador_clientes; i++) {
        if (clientes[i] != emisor_fd) { // No reenviar al que lo mandó
            send(clientes[i], mensaje, strlen(mensaje), 0);
        }
    }
    pthread_mutex_unlock(&clientes_mutex);
}

void *manejar_cliente(void *arg) {
    int cliente_fd = *((int *)arg);
    free(arg);
    char buffer[BUFFER_SIZE];

    while (1) {
        int bytes_recibidos = recv(cliente_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_recibidos <= 0) {
            // Cliente desconectado
            close(cliente_fd);
            pthread_mutex_lock(&clientes_mutex);
            for (int i = 0; i < contador_clientes; i++) {
                if (clientes[i] == cliente_fd) {
                    clientes[i] = clientes[contador_clientes - 1];
                    contador_clientes--;
                    break;
                }
            }
            pthread_mutex_unlock(&clientes_mutex);
            break;
        }

        buffer[bytes_recibidos] = '\0';
        printf("Retransmitiendo: %s", buffer);
        enviar_a_todos(buffer, cliente_fd);
    }
    return NULL;
}

int main() {
    int servidor_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in direccion = { .sin_family = AF_INET, .sin_port = htons(PUERTO), .sin_addr.s_addr = INADDR_ANY };

    bind(servidor_fd, (struct sockaddr *)&direccion, sizeof(direccion));
    listen(servidor_fd, 10);
    printf("Servidor de Tux Messenger escuchando en el puerto %d...\n", PUERTO);

    while (1) {
        struct sockaddr_in cliente_dir;
        socklen_t tamano = sizeof(cliente_dir);
        int *nuevo_cliente = malloc(sizeof(int));
        *nuevo_cliente = accept(servidor_fd, (struct sockaddr *)&cliente_dir, &tamano);

        pthread_mutex_lock(&clientes_mutex);
        if (contador_clientes < MAX_CLIENTES) {
            clientes[contador_clientes++] = *nuevo_cliente;
            pthread_t hilo;
            pthread_create(&hilo, NULL, manejar_cliente, nuevo_cliente);
        } else {
            close(*nuevo_cliente);
            free(nuevo_cliente);
        }
        pthread_mutex_unlock(&clientes_mutex);
    }
    return 0;
}
