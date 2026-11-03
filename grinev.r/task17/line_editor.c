#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define WIDTH 40

static struct termios original;
static int terminal_changed;
static volatile sig_atomic_t stopped;
static char line[WIDTH][5];
static size_t length;

static void restore(void)
{
    if (terminal_changed) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        terminal_changed = 0;
    }
}

static void on_signal(int number)
{
    stopped = number;
}

static int get_byte(unsigned char *byte, int timeout)
{
    struct pollfd fd;
    int result;
    ssize_t count;

    fd.fd = STDIN_FILENO;
    fd.events = POLLIN;
    while (!stopped) {
        fd.revents = 0;
        result = poll(&fd, 1, timeout < 0 ? 100 : timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) {
            if (timeout >= 0) return 0;
            continue;
        }
        count = read(STDIN_FILENO, byte, 1);
        if (count == 1) return 1;
        if (count == 0) return 0;
        if (errno != EINTR) return -1;
    }
    return -1;
}

static void bell(void)
{
    putchar('\a');
    fflush(stdout);
}

static void redraw(void)
{
    size_t i;
    fputs("\r\033[2K", stdout);
    for (i = 0; i < length; i++) fputs(line[i], stdout);
    fflush(stdout);
}

/* Consume a complete arrow/function-key sequence, without printing it. */
static void skip_escape(void)
{
    unsigned char byte;
    if (get_byte(&byte, 100) != 1) return;
    if (byte != '[' && byte != 'O') return;
    while (get_byte(&byte, 100) == 1) {
        if (byte >= 0x40 && byte <= 0x7e) return;
    }
}

/* One UTF-8 character occupies one buffer element (Russian is supported). */
static int read_character(unsigned char first, char text[5])
{
    unsigned char byte;
    unsigned long code;
    int size;
    int i;

    memset(text, 0, 5);
    text[0] = (char)first;
    if (first < 128) return first >= 32 && first != 127;
    if (first >= 0xc2 && first <= 0xdf) {
        size = 2; code = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
        size = 3; code = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
        size = 4; code = first & 7;
    } else return 0;
    for (i = 1; i < size; i++) {
        if (get_byte(&byte, 100) != 1 || (byte & 0xc0) != 0x80)
            return 0;
        text[i] = (char)byte;
        code = (code << 6) | (byte & 0x3f);
    }
    if ((size == 3 && code < 0x800) ||
        (size == 4 && code < 0x10000) ||
        (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff ||
        (code >= 0x80 && code <= 0x9f)) return 0;
    return 1;
}

static void insert_character(const char text[5])
{
    size_t split;
    size_t tail;

    if (length == WIDTH) {
        if (text[0] == ' ') {
            putchar('\n');
            length = 0;
            fflush(stdout);
            return;
        }
        split = length;
        while (split > 0 && line[split - 1][0] != ' ') split--;
        if (split == 0) {
            /* A word longer than 40 characters cannot fit on one line. */
            bell();
            return;
        }
        tail = length - split;
        length = split;
        while (length > 0 && line[length - 1][0] == ' ') length--;
        redraw();
        putchar('\n');
        memmove(line, line + split, tail * sizeof(line[0]));
        length = tail;
    }
    memcpy(line[length++], text, 5);
    redraw();
}

int main(void)
{
    struct termios settings;
    struct sigaction action;
    unsigned char byte;
    char text[5];
    int result;
    int status = EXIT_SUCCESS;

    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "Запустите программу в терминале.\n");
        return EXIT_FAILURE;
    }
    if (tcgetattr(STDIN_FILENO, &original) == -1) {
        perror("tcgetattr");
        return EXIT_FAILURE;
    }
    if (atexit(restore) != 0) return EXIT_FAILURE;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1 ||
        sigaction(SIGHUP, &action, NULL) == -1 ||
        sigaction(SIGQUIT, &action, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    settings = original;
    settings.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
    settings.c_iflag &= (tcflag_t)~(ISTRIP | IXON | IXOFF | INLCR | IGNCR);
    settings.c_iflag |= ICRNL;
    settings.c_cflag &= (tcflag_t)~(CSIZE | PARENB);
    settings.c_cflag |= CS8;
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &settings) == -1) {
        perror("tcsetattr");
        return EXIT_FAILURE;
    }
    terminal_changed = 1;
    puts("Редактор: 40 символов в строке. Ctrl-D в начале строки — выход.");
    fflush(stdout);
    for (;;) {
        result = get_byte(&byte, -1);
        if (result <= 0) {
            if (result < 0 && !stopped) { perror("read/poll"); status = EXIT_FAILURE; }
            break;
        }
        if (byte == 4) {
            if (length == 0) break;
            bell();
        } else if (original.c_cc[VERASE] != _POSIX_VDISABLE &&
                   byte == original.c_cc[VERASE]) {
            if (length > 0) { length--; redraw(); }
        } else if (original.c_cc[VKILL] != _POSIX_VDISABLE &&
                   byte == original.c_cc[VKILL]) {
            length = 0;
            redraw();
        } else if (byte == 23) {
            while (length > 0 && line[length - 1][0] == ' ') length--;
            while (length > 0 && line[length - 1][0] != ' ') length--;
            redraw();
        } else if (byte == '\n' || byte == '\r') {
            putchar('\n');
            length = 0;
            fflush(stdout);
        } else if (byte == 27) {
            bell();
            skip_escape();
        } else if (read_character(byte, text)) {
            insert_character(text);
        } else bell();
    }
    putchar('\n');
    restore();
    return stopped ? 128 + (int)stopped : status;
}
