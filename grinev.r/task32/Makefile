CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g
LDLIBS = `if test "\`uname -s\`" = SunOS; then printf '%s' '-lsocket -lnsl -lrt'; fi`

all: server client

server: server.c socket_io.h
	$(CC) $(CFLAGS) server.c -o server -pthread $(LDLIBS)

client: client.c socket_io.h terminal_input.h
	$(CC) $(CFLAGS) client.c -o client $(LDLIBS)

.PHONY: demo
demo: all
	bash demo.sh
