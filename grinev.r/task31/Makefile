CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g

all: server client

server: server.c socket_io.h
	$(CC) $(CFLAGS) server.c -o server $(LDLIBS)

client: client.c socket_io.h terminal_input.h
	$(CC) $(CFLAGS) client.c -o client $(LDLIBS)
