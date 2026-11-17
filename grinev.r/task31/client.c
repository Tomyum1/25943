#define _POSIX_C_SOURCE 200809L
#if defined(__sun) && !defined(__EXTENSIONS__)
#define __EXTENSIONS__ 1 /* Заголовок illumos открывает getline() в этом режиме. */
#endif

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>

#include "socket_io.h"
#include "terminal_input.h"

static int connect_server(const struct sockaddr_un *address, int retry)
{
    for (int attempt = 0; ; attempt++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        int error;

        if (fd == -1) return -1;
        if (connect(fd, (const struct sockaddr *)address, sizeof(*address)) == 0)
            return fd;
        error = errno;
        close(fd);
        /* Короткое демо можно запускать сразу вслед за фоновым сервером. */
        if (!retry || attempt == 20 || (error != ENOENT && error != ECONNREFUSED)) {
            errno = error;
            return -1;
        }
        (void)poll(NULL, 0, 100);
    }
}

static int delay_ms(const char *text)
{
    char *end;
    long value = strtol(text, &end, 10);

    return end != text && *end == '\0' && value >= 0 && value <= 5000
               ? (int)value : -1;
}

static int send_demo(int connection, unsigned char letter, int pause, int offset)
{
    unsigned char client_id = letter == 'a' ? 1 : 2;
    const char *first = letter == 'a' ? "Hello " : "nice ";
    const char *second = letter == 'a' ? "World\n" : "hat\n";

    /* Первый байт — номер демо-клиента, а не часть фразы. */
    if (write_all(connection, &client_id, 1) == -1) goto failed;
    if (pause < 0) srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    /* Второй клиент начинает чуть позже первого. */
    if (offset > 0) (void)poll(NULL, 0, offset);
    for (int i = 0; i < 5; i++) {
        /* Пишем строку двумя порциями; перевод строки есть только во второй. */
        if (write_all(connection, (const unsigned char *)first, strlen(first)) == -1)
            goto failed;
        (void)poll(NULL, 0, pause < 0 ? 120 + rand() % 201 : pause);
        if (write_all(connection, (const unsigned char *)second, strlen(second)) == -1)
            goto failed;
        if (i < 4) (void)poll(NULL, 0, pause < 0 ? 120 + rand() % 201 : pause);
    }
    return EXIT_SUCCESS;

failed:
    perror("write socket");
    return EXIT_FAILURE;
}

static int send_file(int connection, const char *path, int pause, int offset)
{
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    int next_delay = offset;
    int status = EXIT_SUCCESS;

    if (file == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }

    while ((length = getline(&line, &capacity, file)) >= 0) {
        size_t size = (size_t)length;
        size_t split = size / 2;

        /* По возможности делим строку между словами. */
        for (size_t i = split; i < size && line[i] != '\n'; i++) {
            if (line[i] == ' ') {
                split = i + 1;
                break;
            }
        }
        if (next_delay > 0) (void)poll(NULL, 0, next_delay);
        if (write_all(connection, (const unsigned char *)line, split) == -1)
            goto failed;
        if (pause > 0 && split > 0 && split < size)
            (void)poll(NULL, 0, pause);
        if (write_all(connection, (const unsigned char *)line + split,
                      size - split) == -1)
            goto failed;
        next_delay = pause;
    }
    if (!feof(file)) {
        perror(path);
        status = EXIT_FAILURE;
    }
    goto done;

failed:
    perror("write socket");
    status = EXIT_FAILURE;
done:
    free(line);
    fclose(file);
    return status;
}

static int wait_for_server(int connection)
{
    unsigned char ready;
    ssize_t count;

    do {
        count = read(connection, &ready, 1);
    } while (count < 0 && errno == EINTR);
    if (count == 1 && ready == 1) return 0;
    if (count >= 0) errno = EIO;
    return -1;
}

static int send_numbered_line(int connection, int number, int sync)
{
    static const unsigned char first[] = "Hello World\n";
    static const unsigned char second[] = "nice hat\n";
    unsigned char id = (unsigned char)(number + (sync ? 2 : 0));

    if (write_all(connection, &id, 1) == -1) goto failed;
    if (number == 1) {
        for (size_t i = 0; i < sizeof(first) - 1; i++) {
            if (write_all(connection, first + i, 1) == -1) goto failed;
            /* После первой буквы ждём, пока сервер выведет строку клиента 2. */
            if (sync && i == 0 && wait_for_server(connection) == -1) goto failed;
        }
    } else {
        if (sync && wait_for_server(connection) == -1) goto failed;
        if (write_all(connection, second, sizeof(second) - 1) == -1) goto failed;
    }
    return EXIT_SUCCESS;

failed:
    perror("numbered demo");
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

static int send_manual_input(int connection, unsigned char number)
{
    if (write_all(connection, &number, 1) == -1) {
        perror("write client number");
        return EXIT_FAILURE;
    }
    return send_terminal_input(connection, number);
}

static int run_auto_demo(int connection, const struct sockaddr_un *address)
{
    pid_t child = fork();
    pid_t waited;
    int child_status;
    int status;

    if (child == -1) {
        perror("fork");
        return EXIT_FAILURE;
    }
    if (child == 0) {
        int second;

        /* Потомок открывает собственное соединение для фразы nice hat. */
        close(connection);
        second = connect_server(address, 1);
        if (second == -1) {
            perror("connect");
            _exit(EXIT_FAILURE);
        }
        status = send_demo(second, 'b', -1, 60);
        close(second);
        _exit(status);
    }

    status = send_demo(connection, 'a', -1, 0);
    do {
        waited = waitpid(child, &child_status, 0);
    } while (waited == -1 && errno == EINTR);
    if (waited == -1) {
        perror("waitpid");
        return EXIT_FAILURE;
    }
    if (status != EXIT_SUCCESS || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != EXIT_SUCCESS) return EXIT_FAILURE;

    /* В ручном режиме переиспользуем соединение первого клиента. */
    if (isatty(STDIN_FILENO)) {
        if (terminal_in_background())
            fprintf(stderr, "\nДемо завершено. Введи fg для ручного ввода.\n");
        return send_terminal_input(connection, 1);
    }
    return send_input(connection);
}

int main(int argc, char *argv[])
{
    struct sockaddr_un address;
    const char *path;
    int connection;
    int status;
    int demo = argc > 1 && strcmp(argv[1], "--demo") == 0;
    int auto_demo = demo && argc == 2;
    int file_mode = argc > 1 && strcmp(argv[1], "--file") == 0;
    int pause = 0;
    int offset = 0;
    const char *name = strrchr(argv[0], '/');
    int named_client;
    int sync;
    int manual;

    name = name ? name + 1 : argv[0];
    named_client = strcmp(name, "client1") == 0 ? 1 :
                   strcmp(name, "client2") == 0 ? 2 : 0;
    sync = named_client && argc > 1 && strcmp(argv[1], "--sync") == 0;
    manual = named_client && argc > 1 && strcmp(argv[1], "--manual") == 0;
    if (named_client && (demo || file_mode || argc > ((sync || manual) ? 3 : 2))) {
        fprintf(stderr, "Использование: %s [--sync | --manual] [путь-к-сокету]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (manual && !isatty(STDIN_FILENO)) {
        fprintf(stderr, "Для --manual нужен терминал.\n");
        return EXIT_FAILURE;
    }
    if ((!named_client && !demo && !file_mode && argc > 2) ||
        (file_mode && (argc != 6 ||
                       (pause = delay_ms(argv[4])) < 0 || (offset = delay_ms(argv[5])) < 0)) ||
        (demo && !auto_demo && (argc != 6 ||
                  (strcmp(argv[2], "a") != 0 && strcmp(argv[2], "b") != 0) ||
                  (pause = delay_ms(argv[4])) < 0 || (offset = delay_ms(argv[5])) < 0))) {
        fprintf(stderr, "Использование: %s [путь-к-сокету]\n"
                        "              %s --demo\n"
                        "              %s --demo a|b путь пауза-мс сдвиг-мс (0–5000)\n"
                        "              %s --file файл путь пауза-мс сдвиг-мс (0–5000)\n",
                argv[0], argv[0], argv[0], argv[0]);
        return EXIT_FAILURE;
    }
    path = (sync || manual) ? (argc == 3 ? argv[2] : DEFAULT_SOCKET) :
           auto_demo ? DEFAULT_SOCKET :
           (demo || file_mode) ? argv[3] : (argc == 2 ? argv[1] : DEFAULT_SOCKET);
    if (make_address(&address, path) == -1) {
        perror("socket path");
        return EXIT_FAILURE;
    }
    /* Если сервер отключится, write вернёт ошибку вместо завершения по SIGPIPE. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }
    connection = connect_server(&address, auto_demo || named_client);
    if (connection == -1) {
        perror("connect");
        return EXIT_FAILURE;
    }
    /* Файлы и конвейеры передаём потоком; клавиатуру обрабатываем редактором. */
    if (manual) {
        status = send_manual_input(connection, (unsigned char)named_client);
    } else if (named_client) {
        status = send_numbered_line(connection, named_client, sync);
    } else if (auto_demo) {
        status = run_auto_demo(connection, &address);
    } else if (demo) {
        status = send_demo(connection, (unsigned char)argv[2][0], pause, offset);
    } else if (file_mode) {
        status = send_file(connection, argv[2], pause, offset);
    } else {
        status = isatty(STDIN_FILENO)
                     ? send_terminal_input(connection, 0)
                     : send_input(connection);
    }
    /* Закрываем соединение: сервер закроет только это соединение. */
    close(connection);
    return status;
}
