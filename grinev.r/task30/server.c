#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "socket_io.h"

static const char *socket_path;

static void stop_server(int signum)
{
    /* При прерывании убираем адрес сокета, чтобы сервер можно было запустить снова. */
    (void)unlink(socket_path);
    _exit(128 + signum);
}

static int print_uppercase(int client)
{
    unsigned char buffer[4096];

    for (;;) {
        /* read ждёт данные и возвращает их порциями до размера буфера. */
        ssize_t count = read(client, buffer, sizeof(buffer));

        /* Ноль означает EOF: клиент закрыл соединение, все данные прочитаны. */
        if (count == 0) return EXIT_SUCCESS;
        if (count < 0) {
            /* Сигнал мог прервать чтение; в этом случае пробуем ещё раз. */
            if (errno == EINTR) continue;
            perror("read socket");
            return EXIT_FAILURE;
        }
        /* Обрабатываем только прочитанные байты, включая возможную часть строки. */
        for (ssize_t i = 0; i < count; i++) {
            buffer[i] = (unsigned char)toupper(buffer[i]);
        }
        /* Пишем в stdout сервера, поэтому результат виден в его терминале. */
        if (write_all(STDOUT_FILENO, buffer, (size_t)count) == -1) {
            perror("write stdout");
            return EXIT_FAILURE;
        }
    }
}

int main(int argc, char *argv[])
{
    struct sockaddr_un address;
    struct sigaction action = {0};
    /* listener принимает подключения, client используется для чтения текста. */
    int listener;
    int client;
    int status = EXIT_FAILURE;

    if (argc > 2) {
        fprintf(stderr, "Использование: %s [путь-к-сокету]\n", argv[0]);
        return EXIT_FAILURE;
    }
    /* Без аргументов используем task30.sock в текущем каталоге. */
    socket_path = argc == 2 ? argv[1] : DEFAULT_SOCKET;
    if (make_address(&address, socket_path) == -1) {
        perror("socket path");
        return EXIT_FAILURE;
    }
    /* AF_UNIX — связь на одной машине, SOCK_STREAM — поток байтов. */
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }
    /* bind привязывает сокет к пути, по которому клиент найдёт сервер. */
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
    } else if (listen(listener, 1) == -1) {
        perror("listen");
    } else {
        fprintf(stderr, "Сервер ждёт клиента: %s\n", socket_path);
        /* accept ждёт клиента и возвращает отдельный дескриптор соединения. */
        do {
            client = accept(listener, NULL, NULL);
        } while (client == -1 && errno == EINTR);
        if (client == -1) {
            perror("accept");
        } else {
            /* По условию обрабатываем одного клиента и выходим после EOF. */
            status = print_uppercase(client);
            close(client);
        }
    }
    close(listener);
    /* close освобождает дескриптор, unlink удаляет имя сокета из каталога. */
    if (unlink(socket_path) == -1) {
        perror("unlink socket");
        return EXIT_FAILURE;
    }
    return status;
}
