#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "socket_io.h"
#include "terminal_input.h"

static int send_input(int connection)
{
    unsigned char buffer[4096];

    for (;;) {
        /* Читаем из терминала или перенаправленного stdin одинаково. */
        ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));

        /* Ctrl-D на пустой строке даёт EOF при обычном режиме терминала. */
        if (count == 0) return EXIT_SUCCESS;
        if (count < 0) {
            /* EINTR означает, что вызов прерван сигналом и его можно повторить. */
            if (errno == EINTR) continue;
            perror("read stdin");
            return EXIT_FAILURE;
        }
        /* Передаём только фактически прочитанные байты, без завершающего нуля. */
        if (write_all(connection, buffer, (size_t)count) == -1) {
            perror("write socket");
            return EXIT_FAILURE;
        }
    }
}

int main(int argc, char *argv[])
{
    struct sockaddr_un address;
    const char *path;
    int connection;
    int status;

    if (argc > 2) {
        fprintf(stderr, "Использование: %s [путь-к-сокету]\n", argv[0]);
        return EXIT_FAILURE;
    }
    path = argc == 2 ? argv[1] : DEFAULT_SOCKET;
    if (make_address(&address, path) == -1) {
        perror("socket path");
        return EXIT_FAILURE;
    }
    /* Если сервер отключится, write вернёт ошибку вместо завершения по SIGPIPE. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }
    connection = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connection == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /* Даём серверу секунду на создание сокета и запуск listen(). */
    sleep(1);
    if (connect(connection, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("connect");
        close(connection);
        return EXIT_FAILURE;
    }
    /* Файлы и конвейеры передаём потоком; клавиатуру обрабатываем редактором. */
    status = isatty(STDIN_FILENO)
                 ? send_terminal_input(connection)
                 : send_input(connection);
    /* Закрываем соединение: сервер дочитает данные и получит EOF. */
    close(connection);
    return status;
}
