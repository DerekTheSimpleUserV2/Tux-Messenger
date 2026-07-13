CC = gcc
CFLAGS = `pkg-config --cflags gtk4`
LIBS = `pkg-config --libs gtk4` -lpthread
PREFIX = /usr

all: tuxmessenger server

tuxmessenger: tuxmessenger.c
	$(CC) $(CFLAGS) -o tuxmessenger tuxmessenger.c $(LIBS)

server: server.c
	$(CC) -o server server.c -lpthread

install:
	install -Dm755 tuxmessenger $(DESTDIR)$(PREFIX)/bin/tuxmessenger
	install -Dm755 server $(DESTDIR)$(PREFIX)/bin/tuxmessenger-server

clean:
	rm -f tuxmessenger server
