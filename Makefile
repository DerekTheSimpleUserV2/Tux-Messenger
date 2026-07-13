CC = gcc
CFLAGS = `pkg-config --cflags gtk4`
LIBS = `pkg-config --libs gtk4`
PREFIX = /usr

# Regla principal que compila ambos programas
all: server tuxmessenger

# Compilación del servidor (usa la librería de hilos pthread)
server: server.c
	$(CC) -o server server.c -lpthread

# Compilación del cliente Tux Messenger (usa las librerías de GTK4)
tuxmessenger: tuxmessenger.c
	$(CC) $(CFLAGS) -o tuxmessenger tuxmessenger.c $(LIBS)

# Regla para instalar los binarios en el sistema (usado por Flatpak/Alpine)
install:
	install -Dm755 tuxmessenger $(DESTDIR)$(PREFIX)/bin/tuxmessenger
	install -Dm755 server $(DESTDIR)$(PREFIX)/bin/tuxmessenger-server

# Limpieza de los archivos ejecutables compilados
clean:
	rm -f server tuxmessenger

.PHONY: all clean install
