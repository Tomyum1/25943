CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g
LDLIBS = `if test "\`uname -s\`" = SunOS; then printf '%s' '-lsocket -lnsl'; fi`

all: server client client1 client2

server: server.c socket_io.h
	$(CC) $(CFLAGS) server.c -o server $(LDLIBS)

client: client.c socket_io.h terminal_input.h
	$(CC) $(CFLAGS) client.c -o client $(LDLIBS)

client1 client2: client
	cp client $@

.PHONY: demo
demo: all
	bash demo.sh
