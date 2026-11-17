#define _POSIX_C_SOURCE 200809L

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "socket_io.h"
#include "terminal_input.h"

static int delay_ms(const char *text)
{
    char *end;
    long value = strtol(text, &end, 10);

    return end != text && *end == '\0' && value >= 0 && value <= 5000
               ? (int)value : -1;
}

static int send_demo(int connection, unsigned char letter, int pause, int offset)
{
    const char *first = letter == 'a' ? "Hello " : "nice ";
    const char *second = letter == 'a' ? "World\n" : "hat\n";

    /* Второй клиент начинает чуть позже первого. */
    if (offset > 0) (void)poll(NULL, 0, offset);
    for (int i = 0; i < 5; i++) {
        /* Пишем строку двумя порциями; перевод строки есть только во второй. */
        if (write_all(connection, (const unsigned char *)first, strlen(first)) == -1)
            goto failed;
        (void)poll(NULL, 0, pause);
        if (write_all(connection, (const unsigned char *)second, strlen(second)) == -1)
            goto failed;
        if (i < 4) (void)poll(NULL, 0, pause);
    }
    return EXIT_SUCCESS;

failed:
    perror("write socket");
    return EXIT_FAILURE;
}

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
    int demo = argc > 1 && strcmp(argv[1], "--demo") == 0;
    int pause = 0;
    int offset = 0;

    if ((!demo && argc > 2) ||
        (demo && (argc != 6 ||
                  (strcmp(argv[2], "a") != 0 && strcmp(argv[2], "b") != 0) ||
                  (pause = delay_ms(argv[4])) < 0 || (offset = delay_ms(argv[5])) < 0))) {
        fprintf(stderr, "Использование: %s [путь-к-сокету]\n"
                        "              %s --demo a|b путь пауза-мс сдвиг-мс (0–5000)\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }
    path = demo ? argv[3] : (argc == 2 ? argv[1] : DEFAULT_SOCKET);
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
    /* Подключаемся по тому же пути, который сервер передал в bind. */
    if (connect(connection, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("connect");
        close(connection);
        return EXIT_FAILURE;
    }
    /* Файлы и конвейеры передаём потоком; клавиатуру обрабатываем редактором. */
    if (demo) {
        status = send_demo(connection, (unsigned char)argv[2][0], pause, offset);
    } else {
        status = isatty(STDIN_FILENO)
                     ? send_terminal_input(connection)
                     : send_input(connection);
    }
    /* Закрываем соединение: сервер закроет только это соединение. */
    close(connection);
    return status;
}
