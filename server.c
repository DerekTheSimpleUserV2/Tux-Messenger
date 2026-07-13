#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PUERTO 12345
#define MAX_CLIENTES 100
#define BUFFER_SIZE 2048

typedef struct {
    int fd;
    char nombre[50];
} ClienteInfo;

ClienteInfo clientes[MAX_CLIENTES];
int contador_clientes = 0;
pthread_mutex_t clientes_mutex = PTHREAD_MUTEX_INITIALIZER;

int usuario_existe(const char *nombre) {
    for (int i = 0; i < contador_clientes; i++) {
        if (strlen(clientes[i].nombre) > 0 && strcmp(clientes[i].nombre, nombre) == 0) {
            return 1;
        }
    }
    return 0;
}

void enviar_a_todos(char *mensaje, int emisor_fd) {
    pthread_mutex_lock(&clientes_mutex);
    for (int i = 0; i < contador_clientes; i++) {
        if (clientes[i].fd != emisor_fd && strlen(clientes[i].nombre) > 0) {
            send(clientes[i].fd, mensaje, strlen(mensaje), 0);
        }
    }
    pthread_mutex_unlock(&clientes_mutex);
}

void *manejar_cliente(void *arg) {
    int cliente_fd = *((int *)arg);
    free(arg);
    char buffer[BUFFER_SIZE];
    char mi_nombre[50] = {0};
    int registrado = 0;

    while (1) {
        int bytes_recibidos = recv(cliente_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_recibidos <= 0) {
            close(cliente_fd);
            pthread_mutex_lock(&clientes_mutex);
            for (int i = 0; i < contador_clientes; i++) {
                if (clientes[i].fd == cliente_fd) {
                    clientes[i] = clientes[contador_clientes - 1];
                    contador_clientes--;
                    break;
                }
            }
            pthread_mutex_unlock(&clientes_mutex);
            break;
        }

        buffer[bytes_recibidos] = '\0';
        buffer[strcspn(buffer, "\r\n")] = 0;

        // FASE 1: REGISTRO
        if (!registrado) {
            if (strncmp(buffer, "REGISTRO:", 9) == 0) {
                char *nombre_intento = buffer + 9;
                pthread_mutex_lock(&clientes_mutex);
                if (usuario_existe(nombre_intento) || strcmp(nombre_intento, "Sistema") == 0) {
                    send(cliente_fd, "ERROR_USUARIO_OCUPADO\n", 22, 0);
                    pthread_mutex_unlock(&clientes_mutex);
                } else {
                    for (int i = 0; i < contador_clientes; i++) {
                        if (clientes[i].fd == cliente_fd) {
                            strncpy(clientes[i].nombre, nombre_intento, 49);
                            strncpy(mi_nombre, nombre_intento, 49);
                            break;
                        }
                    }
                    send(cliente_fd, "REGISTRO_OK\n", 12, 0);
                    registrado = 1;
                    pthread_mutex_unlock(&clientes_mutex);
                }
            }
            continue;
        }

        // FASE 2: COMANDOS Y MENSAJES
        if (strncmp(buffer, "DM:", 3) == 0) {
            char *resto = buffer + 3;
            char *delimitador = strchr(resto, ':');
            if (delimitador != NULL) {
                *delimitador = '\0';
                char *destinatario = resto;
                char *msg_cuerpo = delimitador + 1;

                pthread_mutex_lock(&clientes_mutex);
                int encontrado = 0;
                for (int i = 0; i < contador_clientes; i++) {
                    if (strcmp(clientes[i].nombre, destinatario) == 0) {
                        char msg_privado[BUFFER_SIZE];
                        snprintf(msg_privado, sizeof(msg_privado), "<b>[DM de %s]</b>: %s\n", mi_nombre, msg_cuerpo);
                        send(clientes[i].fd, msg_privado, strlen(msg_privado), 0);
                        encontrado = 1;
                        break;
                    }
                }
                pthread_mutex_unlock(&clientes_mutex);
                if (!encontrado) {
                    send(cliente_fd, "<b>Sistema</b>: Usuario no encontrado para DM.\n", 48, 0);
                }
            }
        } 
        else if (strncmp(buffer, "REPORTE:", 8) == 0) {
            char *resto = buffer + 8;
            char *delimitador = strchr(resto, ':');
            if (delimitador != NULL) {
                *delimitador = '\0';
                char *reportado = resto;
                char *motivo = delimitador + 1;

                FILE *f = fopen("reportes.txt", "a");
                if (f) {
                    fprintf(f, "[REPORTE] '%s' reportó a '%s'. Motivo: %s\n", mi_nombre, reportado, motivo);
                    fclose(f);
                }
                send(cliente_fd, "<b>Sistema</b>: Reporte recibido correctamente.\n", 48, 0);
            }
        } 
        else {
            char msg_global[BUFFER_SIZE];
            snprintf(msg_global, sizeof(msg_global), "%s\n", buffer); // Ya viene formateado del cliente
            enviar_a_todos(msg_global, cliente_fd);
        }
    }
    return NULL;
}

int main() {
    int servidor_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in direccion = { .sin_family = AF_INET, .sin_port = htons(PUERTO), .sin_addr.s_addr = INADDR_ANY };
    int opt = 1;
    setsockopt(servidor_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(servidor_fd, (struct sockaddr *)&direccion, sizeof(direccion)) < 0) return 1;
    listen(servidor_fd, 10);
    printf("Tux Messenger Server escuchando en el puerto %d...\n", PUERTO);

    while (1) {
        struct sockaddr_in c_dir;
        socklen_t tam = sizeof(c_dir);
        int *n_cliente = malloc(sizeof(int));
        *n_cliente = accept(servidor_fd, (struct sockaddr *)&c_dir, &tam);

        pthread_mutex_lock(&clientes_mutex);
        if (contador_clientes < MAX_CLIENTES) {
            clientes[contador_clientes].fd = *n_cliente;
            memset(clientes[contador_clientes].nombre, 0, 50);
            contador_clientes++;
            pthread_t hilo;
            pthread_create(&hilo, NULL, manejar_cliente, n_cliente);
        } else {
            close(*n_cliente);
            free(n_cliente);
        }
        pthread_mutex_unlock(&clientes_mutex);
    }
    return 0;
}
