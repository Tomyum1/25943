#ifndef TERMINAL_INPUT_H
#define TERMINAL_INPUT_H

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>

#include "socket_io.h"

#define LINE_LIMIT 80

enum escape_state { TEXT, ESC, CSI, CONTROL_STRING, STRING_ESC };

static volatile sig_atomic_t interrupted;

static void interrupt_input(int signum)
{
    interrupted = signum;
}

/* Стрелки, функциональные клавиши и управляющие строки не становятся текстом. */
static int skip_escape(enum escape_state *state, unsigned char key)
{
    if (*state != TEXT && (key == '\n' || key == '\r' || key == 4 ||
                          key == 21 || key == 8 || key == 127)) {
        *state = TEXT;
        return 0;
    }

    switch (*state) {
    case TEXT:
        if (key != 27) return 0;
        *state = ESC;
        return 1;
    case ESC:
        if (key == '[' || key == 'O') *state = CSI;
        else if (key == ']' || key == 'P' || key == '^' || key == '_' || key == 'X')
            *state = CONTROL_STRING;
        else if (key != 27) {
            *state = TEXT;
            return 0;
        }
        return 1;
    case CSI:
        if (key >= 0x40 && key <= 0x7e) {
            *state = TEXT;
            return 1;
        }
        if (key >= 0x20 && key <= 0x3f) return 1;
        *state = key == 27 ? ESC : TEXT;
        return 1;
    case CONTROL_STRING:
    case STRING_ESC:
        if (key == 7 || (*state == STRING_ESC && key == '\\')) *state = TEXT;
        else *state = key == 27 ? STRING_ESC : CONTROL_STRING;
        return 1;
    }
    return 0;
}

static void show_line(int client, const unsigned char *line, size_t length)
{
    const size_t prompt_width = client ? 11 : 0; /* «[Клиент 1] » */
    size_t width = 76;
    size_t start;

    if (!isatty(STDERR_FILENO)) return;
#ifdef TIOCGWINSZ
    {
        struct winsize window;

        if (ioctl(STDERR_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_col > 0)
            width = window.ws_col - 1;
    }
#endif
    width = width > prompt_width ? width - prompt_width : 0;
    /* Показываем хвост строки, если она не помещается по ширине окна. */
    start = length > width ? length - width : 0;
    fputs("\r\033[2K", stderr);
    if (client) fprintf(stderr, "[Клиент %d] ", client);
    fprintf(stderr, "%.*s", (int)(length - start), (const char *)line + start);
    fflush(stderr);
}

static int terminal_in_background(void)
{
    pid_t foreground = tcgetpgrp(STDIN_FILENO);
    return foreground != -1 && foreground != getpgrp();
}

static int wait_for_foreground(void)
{
    while (!interrupted && terminal_in_background()) {
        if (raise(SIGSTOP) != 0) return -1;
    }
    return 0;
}

static int set_terminal(const struct termios *settings)
{
    int result;

    do {
        result = tcsetattr(STDIN_FILENO, TCSAFLUSH, settings);
    } while (result == -1 && errno == EINTR);
    return result;
}

static int edit_input(int connection, int client, const struct termios *original,
                      const struct termios *settings)
{
    unsigned char line[LINE_LIMIT + 1] = {0};
    size_t length = 0;
    enum escape_state state = TEXT;

    if (client) show_line(client, line, length);
    while (!interrupted) {
        struct pollfd input = {STDIN_FILENO, POLLIN, 0};
        unsigned char key;
        ssize_t count;
        /* Готовые клавиши обрабатываются сразу. Тайм-аут нужен для проверки сигналов. */
        int ready = poll(&input, 1, 100);

        if (ready == 0) continue;
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("poll stdin");
            return EXIT_FAILURE;
        }
        if (interrupted) break;
        count = read(STDIN_FILENO, &key, 1);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            perror("read stdin");
            return EXIT_FAILURE;
        }

        if (key == 3) { interrupted = SIGINT; break; }
        if (skip_escape(&state, key)) continue;
        if (key == 26) { /* Ctrl-Z: возвращаем терминал Bash, сохраняя строку и сокет. */
            fputc('\n', stderr);
            if (set_terminal(original) == -1 || raise(SIGSTOP) != 0 ||
                wait_for_foreground() == -1) {
                perror("suspend client");
                return EXIT_FAILURE;
            }
            if (interrupted) break;
            if (set_terminal(settings) == -1) {
                perror("resume client");
                return EXIT_FAILURE;
            }
            show_line(client, line, length);
        } else if (key == 4) {
            if (length == 0) break;
            fputc('\a', stderr);
        } else if (key == 8 || key == 127 ||
                   (original->c_cc[VERASE] != _POSIX_VDISABLE &&
                    key == original->c_cc[VERASE])) {
            if (length > 0) length--;
            show_line(client, line, length);
        } else if (key == 21) {
            length = 0;
            show_line(client, line, length);
        } else if (key == '\n' || key == '\r') {
            fputc('\n', stderr);
            line[length++] = '\n';
            /* Сервер получает только готовую строку, без клавиш редактирования. */
            if (write_all(connection, line, length) == -1) {
                perror("write socket");
                return EXIT_FAILURE;
            }
            length = 0;
        } else if (key >= 32 && key <= 126) {
            /* Пробел — часть текста. Tab и прочие управляющие байты сюда не попадают. */
            if (length < LINE_LIMIT) {
                line[length++] = key;
                show_line(client, line, length);
            } else {
                fputc('\a', stderr);
            }
        }
        fflush(stderr);
    }
    fputc('\n', stderr);
    return interrupted ? 128 + (int)interrupted : EXIT_SUCCESS;
}

static int send_terminal_input(int connection, int client)
{
    struct termios original;
    struct termios settings;
    struct sigaction action = {0};
    int status;

    action.sa_handler = interrupt_input;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1 ||
        sigaction(SIGHUP, &action, NULL) == -1 ||
        sigaction(SIGQUIT, &action, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    if (wait_for_foreground() == -1) {
        perror("wait for terminal");
        return EXIT_FAILURE;
    }
    if (interrupted) return 128 + (int)interrupted;
    if (tcgetattr(STDIN_FILENO, &original) == -1) {
        perror("tcgetattr");
        return EXIT_FAILURE;
    }

    /* Отключаем встроенное редактирование терминала и читаем клавиши по байтам. */
    settings = original;
    settings.c_lflag &= (tcflag_t)~(ICANON | ECHO | ECHONL | ISIG | IEXTEN);
    settings.c_iflag &= (tcflag_t)~(IXON | IXOFF | ISTRIP | INLCR | IGNCR);
    settings.c_iflag |= ICRNL;
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;
    if (set_terminal(&settings) == -1) {
        perror("tcsetattr");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "Введите английский текст (до %d байт). Ctrl-Z — приостановить, "
                    "Ctrl-D на пустой строке — выход.\n",
            LINE_LIMIT);
    status = edit_input(connection, client, &original, &settings);

    /* Восстанавливаем настройки и убираем остаток ввода перед возвратом в shell. */
    if (!terminal_in_background() && set_terminal(&original) == -1) {
        perror("restore terminal");
        return EXIT_FAILURE;
    }
    return status;
}

#endif
