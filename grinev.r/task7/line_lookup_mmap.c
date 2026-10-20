#ifndef __EXTENSIONS__
#define __EXTENSIONS__ 1
#endif
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define INITIAL_CAPACITY 16
#define MAX_INPUT_BYTES 80
#define ESC_WAIT_MS 100
#define INPUT_SECONDS 5

typedef struct LineInfo {
    size_t offset;
    size_t length;
} LineInfo;

enum InputResult {
    INPUT_TIMEOUT = -4,
    INPUT_SIGNAL = -3,
    INPUT_ERROR = -2,
    INPUT_INVALID = -1,
    INPUT_EOF = 0,
    INPUT_OK = 1
};

static struct termios saved_terminal;
static int terminal_was_changed = 0;
static int pending_byte = -1;
static int skip_line_feed = 0;
static volatile sig_atomic_t stopped_signal = 0;
static volatile sig_atomic_t alarm_expired = 0;

static void signal_handler(int signal_number)
{
    if (signal_number == SIGALRM) {
        alarm_expired = 1;
    } else {
        stopped_signal = signal_number;
    }
}

static void restore_terminal(void)
{
    if (terminal_was_changed) {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_terminal) == -1) {
            perror("restore terminal");
        } else {
            terminal_was_changed = 0;
        }
    }
}

static int setup_terminal(void)
{
    struct termios terminal;
    struct sigaction action;
    int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGALRM};
    size_t i;

    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);

    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (sigaction(signals[i], &action, NULL) == -1) {
            return -1;
        }
    }

    if (!isatty(STDIN_FILENO)) {
        return 0;
    }
    if (tcgetattr(STDIN_FILENO, &saved_terminal) == -1) {
        return -1;
    }

    terminal = saved_terminal;
    terminal.c_lflag &= (tcflag_t)~(ICANON | ECHO | ECHONL | ISIG | IEXTEN);
    terminal.c_iflag &=
        (tcflag_t)~(IXON | IXOFF | ICRNL | INLCR | IGNCR | ISTRIP);
    terminal.c_cc[VMIN] = 1;
    terminal.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &terminal) == -1) {
        return -1;
    }

    terminal_was_changed = 1;
    return 0;
}


static int read_byte(unsigned char *character, int timeout_ms)
{
    struct pollfd descriptor;
    ssize_t count;
    int result;

    descriptor.fd = STDIN_FILENO;
    descriptor.events = POLLIN;
    descriptor.revents = 0;

    while (!stopped_signal && !alarm_expired) {
        if (pending_byte != -1) {
            *character = (unsigned char)pending_byte;
            pending_byte = -1;
            return 1;
        }

        result = poll(&descriptor, 1, timeout_ms < 0 ? 100 : timeout_ms);

        if (stopped_signal || alarm_expired) {
            break;
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return INPUT_ERROR;
        }
        if (result == 0) {
            if (timeout_ms >= 0) {
                return 0;
            }
            continue;
        }

        count = read(STDIN_FILENO, character, 1);
        if (count >= 0) {
            return (int)count;
        }
        if (errno != EINTR) {
            return INPUT_ERROR;
        }
    }

    return stopped_signal ? INPUT_SIGNAL : INPUT_TIMEOUT;
}

static int skip_escape_sequence(void)
{
    unsigned char character;
    int result;
    int previous = 0;

    result = read_byte(&character, ESC_WAIT_MS);
    if (result != 1) {
        return result == 0 ? INPUT_OK : result;
    }

    if (character == ']' || character == 'P' || character == '_' ||
        character == '^' || character == 'X') {
        for (;;) {
            result = read_byte(&character, ESC_WAIT_MS);
            if (result != 1) {
                return result == 0 ? INPUT_INVALID : result;
            }
            if (character == 7 || (previous == 27 && character == '\\')) {
                return INPUT_OK;
            }
            if (character < 32 && character != 27) {
                pending_byte = character;
                return INPUT_INVALID;
            }
            previous = character;
        }
    }

    if (character != '[' && character != 'O') {
        pending_byte = character;
        return INPUT_OK;
    }

    for (;;) {
        result = read_byte(&character, ESC_WAIT_MS);
        if (result != 1) {
            return result == 0 ? INPUT_INVALID : result;
        }
        if (character >= 0x40U && character <= 0x7EU) {
            return INPUT_OK;
        }
        if (character < 0x20U || character > 0x3FU) {
            pending_byte = character;
            return INPUT_INVALID;
        }
    }
}

static int show_input(const char *text, int length)
{
    struct winsize window;
    int width = length;
    int start;

    if (!terminal_was_changed || !isatty(STDERR_FILENO)) {
        return 0;
    }

    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &window) == 0 &&
        window.ws_col > 3) {
        width = window.ws_col - 3;
    }

    start = length > width ? length - width : 0;
    if (fprintf(stderr, "\r\033[2K> %.*s", length - start,
                text + start) < 0 || fflush(stderr) == EOF) {
        return -1;
    }

    return 0;
}

static int read_line_number(long *line_number)
{
    char input[MAX_INPUT_BYTES + 1];
    char *begin;
    char *end;
    unsigned char character;
    long value;
    int length = 0;
    int invalid = 0;
    int result;

    for (;;) {
        result = read_byte(&character, -1);
        if (result < 0) {
            return result;
        }
        if (result == 0) {
            if (length == 0 && !invalid) {
                return INPUT_EOF;
            }
            break;
        }

        if (skip_line_feed) {
            skip_line_feed = 0;
            if (character == '\n') {
                continue;
            }
        }

        if (character == 3) {
            stopped_signal = SIGINT;
            return INPUT_SIGNAL;
        }

        if (character == '\n' || character == '\r' || character == 4) {
            if (character == 4 && length == 0 && !invalid) {
                return INPUT_EOF;
            }
            skip_line_feed = character == '\r';
            break;
        }

        if (invalid) {
            continue;
        }

        if (character == 27) {
            result = skip_escape_sequence();
            if (result < 0) {
                return result;
            }
            if (result == INPUT_INVALID) {
                invalid = 1;
            }
            continue;
        }

        if (character == 8 || character == 127) {
            if (length > 0) {
                length--;
            }
        } else if (character < 32 || character > 126 ||
                   length == MAX_INPUT_BYTES) {
            invalid = 1;
            continue;
        } else {
            input[length++] = (char)character;
        }

        input[length] = '\0';
        if (show_input(input, length) == -1) {
            return INPUT_ERROR;
        }
    }

    if (terminal_was_changed && isatty(STDERR_FILENO)) {
        fputc('\n', stderr);
    }
    if (invalid) {
        return INPUT_INVALID;
    }

    input[length] = '\0';
    begin = input;
    while (*begin == ' ') {
        begin++;
    }

    if (*begin < '0' || *begin > '9') {
        return INPUT_INVALID;
    }

    errno = 0;
    value = strtol(begin, &end, 10);
    if (errno == ERANGE) {
        return INPUT_INVALID;
    }

    while (*end == ' ') {
        end++;
    }
    if (*end != '\0') {
        return INPUT_INVALID;
    }

    *line_number = value;
    return INPUT_OK;
}

static int timed_read_line_number(long *line_number)
{
    unsigned char first_character;
    int result;

    alarm_expired = 0;
    alarm(INPUT_SECONDS);
    result = read_byte(&first_character, -1);
    alarm(0);

    if (result != 1) {
        return result;
    }

    alarm_expired = 0;
    pending_byte = first_character;
    return read_line_number(line_number);
}

static int append_line(LineInfo **table, size_t *count, size_t *capacity,
                       size_t offset, size_t length)
{
    LineInfo *new_table;
    size_t new_capacity;

    if (*count == *capacity) {
        new_capacity = *capacity == 0 ? INITIAL_CAPACITY : *capacity * 2;
        new_table = realloc(*table, new_capacity * sizeof(**table));

        if (new_table == NULL) {
            perror("realloc");
            return -1;
        }

        *table = new_table;
        *capacity = new_capacity;
    }

    (*table)[*count].offset = offset;
    (*table)[*count].length = length;
    (*count)++;
    return 0;
}

static int build_line_table(const char *mapping, size_t file_size,
                            LineInfo **table, size_t *line_count)
{
    size_t position;
    size_t line_start = 0;
    size_t capacity = 0;

    *table = NULL;
    *line_count = 0;

    for (position = 0; position < file_size; position++) {
        if (mapping[position] == '\n') {
            if (append_line(table, line_count, &capacity,
                            line_start, position - line_start) == -1) {
                free(*table);
                *table = NULL;
                *line_count = 0;
                return -1;
            }
            line_start = position + 1;
        }
    }

    if (line_start < file_size &&
        append_line(table, line_count, &capacity,
                    line_start, file_size - line_start) == -1) {
        free(*table);
        *table = NULL;
        *line_count = 0;
        return -1;
    }

    return 0;
}

static void print_line_table(const LineInfo *table, size_t line_count)
{
    size_t i;

    printf("Таблица строк:\n");
    printf("Номер\tОтступ\tДлина\n");

    for (i = 0; i < line_count; i++) {
        printf("%zu\t%zu\t%zu\n", i + 1,
               table[i].offset, table[i].length);
    }
}

static int print_requested_line(const char *mapping, const LineInfo *line)
{
    if (line->length > 0 &&
        fwrite(mapping + line->offset, 1, line->length, stdout) !=
            line->length) {
        perror("fwrite");
        return -1;
    }

    if (putchar('\n') == EOF) {
        perror("putchar");
        return -1;
    }

    return 0;
}

static int print_entire_file(const char *mapping, size_t file_size)
{
    if (file_size > 0 && fwrite(mapping, 1, file_size, stdout) != file_size) {
        perror("fwrite");
        return -1;
    }

    if (file_size == 0 || mapping[file_size - 1] != '\n') {
        if (putchar('\n') == EOF) {
            perror("putchar");
            return -1;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    int fd;
    struct stat file_status;
    char *mapping = NULL;
    size_t file_size;
    LineInfo *table;
    size_t line_count;
    long line_number;
    int input_result;
    int status = EXIT_SUCCESS;
    int first_input = 1;

    if (argc != 2) {
        fprintf(stderr, "Использование: %s текстовый-файл\n", argv[0]);
        return EXIT_FAILURE;
    }

    fd = open(argv[1], O_RDONLY);
    if (fd == -1) {
        perror("open");
        return EXIT_FAILURE;
    }

    if (fstat(fd, &file_status) == -1) {
        perror("fstat");
        close(fd);
        return EXIT_FAILURE;
    }

    if (file_status.st_size < 0) {
        fprintf(stderr, "Некорректный размер файла.\n");
        close(fd);
        return EXIT_FAILURE;
    }

    file_size = (size_t)file_status.st_size;

    if (file_size > 0) {
        mapping = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping == MAP_FAILED) {
            perror("mmap");
            close(fd);
            return EXIT_FAILURE;
        }
    }

    if (close(fd) == -1) {
        perror("close");
        if (mapping != NULL) {
            munmap(mapping, file_size);
        }
        return EXIT_FAILURE;
    }

    if (build_line_table(mapping, file_size, &table, &line_count) == -1) {
        if (mapping != NULL) {
            munmap(mapping, file_size);
        }
        return EXIT_FAILURE;
    }

    print_line_table(table, line_count);

    if (atexit(restore_terminal) != 0) {
        free(table);
        if (mapping != NULL) {
            munmap(mapping, file_size);
        }
        return EXIT_FAILURE;
    }

    if (setup_terminal() == -1) {
        perror("setup terminal");
        free(table);
        if (mapping != NULL) {
            munmap(mapping, file_size);
        }
        return EXIT_FAILURE;
    }

    for (;;) {
        printf("Введите номер строки%s (0 — выход):\n",
               first_input ? " за 5 секунд" : "");
        fflush(stdout);

        if (show_input("", 0) == -1) {
            input_result = INPUT_ERROR;
        } else if (first_input) {
            input_result = timed_read_line_number(&line_number);
            first_input = 0;
        } else {
            input_result = read_line_number(&line_number);
        }

        if (input_result == INPUT_TIMEOUT) {
            restore_terminal();
            printf("\nВремя вышло. Содержимое файла:\n");

            if (print_entire_file(mapping, file_size) == -1) {
                status = EXIT_FAILURE;
            }
            break;
        }

        if (input_result == INPUT_SIGNAL) {
            status = EXIT_FAILURE;
            break;
        }

        if (input_result == INPUT_EOF) {
            putchar('\n');
            break;
        }

        if (input_result == INPUT_ERROR) {
            status = EXIT_FAILURE;
            break;
        }

        if (input_result == INPUT_INVALID) {
            fprintf(stderr,
                    "Введите только цифры, максимум %d байт.\n",
                    MAX_INPUT_BYTES);
            continue;
        }

        if (line_number == 0) {
            break;
        }

        if (line_count == 0) {
            fprintf(stderr, "Файл не содержит строк.\n");
            continue;
        }

        if (line_number < 1 || (size_t)line_number > line_count) {
            fprintf(stderr,
                    "Строки с номером %ld нет. Допустимый диапазон: 1–%zu.\n",
                    line_number, line_count);
            continue;
        }

        if (print_requested_line(mapping, &table[line_number - 1]) == -1) {
            status = EXIT_FAILURE;
            break;
        }
    }

    restore_terminal();
    free(table);

    if (mapping != NULL && munmap(mapping, file_size) == -1) {
        perror("munmap");
        status = EXIT_FAILURE;
    }

    if (stopped_signal) {
        return 128 + stopped_signal;
    }

    return status;
}
