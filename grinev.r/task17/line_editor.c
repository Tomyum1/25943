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

static int get_byte(unsigned char *byte)
{
    struct pollfd fd;
    int result;
    ssize_t count;

    fd.fd = STDIN_FILENO;
    fd.events = POLLIN;
    while (!stopped) {
        fd.revents = 0;
        result = poll(&fd, 1, -1);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
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
static int escape_byte(unsigned char byte, int *state)
{
    if (*state == 1) {
        if (byte == '[' || byte == 'O') { *state = 2; return 1; }
        if (byte == ']' || byte == 'P' || byte == '^' ||
            byte == '_' || byte == 'X') { *state = 3; return 1; }
        *state = 0;
        return 0;
    }
    if (*state == 2) {
        if (byte >= 0x40 && byte <= 0x7e) { *state = 0; return 1; }
        if (byte >= 0x20 && byte <= 0x3f) return 1;
        *state = 0;
        return 0;
    }
    if (*state == 3 || *state == 4) {
        if (byte == 7 || (*state == 4 && byte == '\\')) *state = 0;
        else *state = byte == 27 ? 4 : 3;
        return 1;
    }
    return 0;
}

/* One UTF-8 character occupies one buffer element (Russian is supported). */
static int character_byte(unsigned char byte, char text[5])
{
    static unsigned long code;
    static int expected;
    static int used;

    if (used && (byte & 0xc0) != 0x80) {
        used = 0;
        bell();
    }
    if (!used) {
        memset(text, 0, 5);
        text[0] = (char)byte;
        if (byte < 128) return byte >= 32 && byte != 127 ? 1 : -1;
        if (byte >= 0xc2 && byte <= 0xdf) {
            expected = 2; code = byte & 0x1f;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            expected = 3; code = byte & 0x0f;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            expected = 4; code = byte & 7;
        } else return -1;
        used = 1;
        return 0;
    }
    text[used++] = (char)byte;
    code = (code << 6) | (byte & 0x3f);
    if (used < expected) return 0;
    used = 0;
    if ((expected == 3 && code < 0x800) ||
        (expected == 4 && code < 0x10000) ||
        (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff ||
        (code >= 0x80 && code <= 0x9f)) return -1;
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
            /* Split a word that is longer than the whole line. */
            putchar('\n');
            length = 0;
            memcpy(line[length++], text, 5);
            redraw();
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
    int escape_state = 0;
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
    settings.c_cc[VMIN] = 1; ///////
    settings.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &settings) == -1) {
        perror("tcsetattr");
        return EXIT_FAILURE;
    }
    terminal_changed = 1;
    puts("Редактор: 40 символов в строке. Ctrl-D в начале строки — выход.");
    fflush(stdout);
    for (;;) {
        result = get_byte(&byte);
        if (result <= 0) {
            if (result < 0 && !stopped) { perror("read/poll"); status = EXIT_FAILURE; }
            break;
        }
        if (escape_byte(byte, &escape_state)) continue;
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
            escape_state = 1;
        } else {
            result = character_byte(byte, text);
            if (result == 1) insert_character(text);
            else if (result < 0) bell();
        }
    }
    putchar('\n');
    restore();
    return stopped ? 128 + (int)stopped : status;
}
