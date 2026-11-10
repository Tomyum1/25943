#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "socket_io.h"

#define MAX_CLIENTS 64

static const char *socket_path;

static void stop_server(int signum)
{
    /* При Ctrl-C ядро закроет дескрипторы, а мы уберём имя сокета. */
    (void)unlink(socket_path);
    _exit(128 + signum);
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static void accept_client(int listener, struct pollfd *slot)
{
    int client = accept(listener, NULL, NULL);

    if (client == -1) {
        if (errno != EINTR && !would_block()) perror("accept");
        return;
    }
    /* Даже если состояние сокета изменилось после poll, read не зависнет. */
    if (set_nonblocking(client) == -1) {
        perror("fcntl client");
        close(client);
        return;
    }
    slot->fd = client;
    slot->events = POLLIN;
    slot->revents = 0;
}

static int handle_client(struct pollfd *slot)
{
    unsigned char buffer[4096];
    ssize_t count;

    if (!(slot->revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
        return 0;
    }

    /* При POLLHUP в сокете ещё могут быть данные: сначала пробуем прочитать. */
    count = read(slot->fd, buffer, sizeof(buffer));
    if (count > 0) {
        for (ssize_t i = 0; i < count; i++) {
            buffer[i] = (unsigned char)toupper(buffer[i]);
        }
        if (write_all(STDOUT_FILENO, buffer, (size_t)count) == -1) {
            perror("write stdout");
            return -1;
        }
        /* Одна порция за проход, чтобы обслужить остальных готовых клиентов. */
        return 0;
    }
    if (count < 0) {
        if (errno == EINTR) return 0;
        if (would_block()) {
            if (!(slot->revents & (POLLHUP | POLLERR))) return 0;
        } else {
            perror("read socket");
        }
    }

    /* EOF или ошибка закрывает только это соединение. Сервер продолжает работу. */
    close(slot->fd);
    slot->fd = -1;
    return 0;
}

static int serve(int listener)
{
    struct pollfd fds[MAX_CLIENTS + 1];
    int status = EXIT_FAILURE;

    fds[0] = (struct pollfd){listener, POLLIN, 0};
    for (int i = 1; i <= MAX_CLIENTS; i++) {
        fds[i] = (struct pollfd){-1, POLLIN, 0};
    }

    for (;;) {
        int free_slot = 0;

        /* poll игнорирует fd = -1. При заполнении таблицы ждём свободное место. */
        for (int i = 1; i <= MAX_CLIENTS; i++) {
            if (fds[i].fd == -1) {
                free_slot = i;
                break;
            }
        }
        fds[0].events = free_slot ? POLLIN : 0;

        /* -1: спим до события без таймера. POLLIN у fds[0] означает подключение. */
        if (poll(fds, MAX_CLIENTS + 1, -1) == -1) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "Ошибка слушающего сокета.\n");
            break;
        }
        if (free_slot && (fds[0].revents & POLLIN)) {
            accept_client(listener, &fds[free_slot]);
        }
        for (int i = 1; i <= MAX_CLIENTS; i++) {
            if (fds[i].fd != -1 && handle_client(&fds[i]) == -1) {
                goto cleanup;
            }
        }
    }

cleanup:
    for (int i = 1; i <= MAX_CLIENTS; i++) {
        if (fds[i].fd != -1) close(fds[i].fd);
    }
    return status;
}

int main(int argc, char *argv[])
{
    struct sockaddr_un address;
    struct sigaction action = {0};
    int listener;
    int status = EXIT_FAILURE;

    if (argc > 2) {
        fprintf(stderr, "Использование: %s [путь-к-сокету]\n", argv[0]);
        return EXIT_FAILURE;
    }
    socket_path = argc == 2 ? argv[1] : DEFAULT_SOCKET;
    if (make_address(&address, socket_path) == -1) {
        perror("socket path");
        return EXIT_FAILURE;
    }
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        close(listener);
        return EXIT_FAILURE;
    }

    action.sa_handler = stop_server;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1 ||
        sigaction(SIGHUP, &action, NULL) == -1 ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal handler");
    } else if (set_nonblocking(listener) == -1) {
        perror("fcntl listener");
    } else if (listen(listener, MAX_CLIENTS) == -1) {
        perror("listen");
    } else {
        fprintf(stderr, "Сервер ждёт клиентов: %s (Ctrl-C — выход).\n", socket_path);
        status = serve(listener);
    }

    close(listener);
    if (unlink(socket_path) == -1) {
        perror("unlink socket");
        return EXIT_FAILURE;
    }
    return status;
}
