#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define INITIAL_CAPACITY 16
#define MAX_INPUT_BYTES 80
#define INPUT_SIZE (MAX_INPUT_BYTES + 2)

typedef struct LineInfo {
    size_t offset;
    size_t length;
} LineInfo;

enum InputResult {
    INPUT_ERROR = -1,
    INPUT_TIMEOUT = -2,
    INPUT_EOF = 0,
    INPUT_OK = 1,
    INPUT_INVALID = 2,
    INPUT_TOO_LONG = 3
};

static volatile sig_atomic_t alarm_expired = 0;
static struct termios saved_terminal;
static int terminal_was_changed = 0;

static void alarm_handler(int signal_number)
{
    (void)signal_number;
    alarm_expired = 1;
}

static void restore_terminal(void)
{
    if (terminal_was_changed) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_terminal);
        terminal_was_changed = 0;
    }
}

static int restrict_terminal_commands(void)
{
    struct termios restricted_terminal;

    if (!isatty(STDIN_FILENO)) {
        return 0;
    }

    if (tcgetattr(STDIN_FILENO, &saved_terminal) == -1) {
        perror("tcgetattr");
        return -1;
    }

    restricted_terminal = saved_terminal;

#ifdef VREPRINT
    restricted_terminal.c_cc[VREPRINT] = _POSIX_VDISABLE;
#endif
#ifdef VDISCARD
    restricted_terminal.c_cc[VDISCARD] = _POSIX_VDISABLE;
#endif
#ifdef VLNEXT
    restricted_terminal.c_cc[VLNEXT] = _POSIX_VDISABLE;
#endif

    if (tcsetattr(STDIN_FILENO, TCSANOW, &restricted_terminal) == -1) {
        perror("tcsetattr");
        return -1;
    }

    terminal_was_changed = 1;

    if (atexit(restore_terminal) != 0) {
        restore_terminal();
        fprintf(stderr, "Не удалось зарегистрировать восстановление терминала.\n");
        return -1;
    }

    return 0;
}

static size_t skip_escape_sequence(const unsigned char *input,
                                   size_t position)
{
    size_t i = position + 1;

    if (input[i] == '\0') {
        return i;
    }

    if (input[i] == '[') {
        i++;
        while (input[i] != '\0') {
            unsigned char character = input[i++];
            if (character >= 0x40U && character <= 0x7EU) {
                break;
            }
        }
        return i;
    }

    if (input[i] == ']' || input[i] == 'P' || input[i] == 'X' ||
        input[i] == '^' || input[i] == '_') {
        i++;
        while (input[i] != '\0') {
            if (input[i] == 0x07U) {
                return i + 1;
            }
            if (input[i] == 0x1BU && input[i + 1] == '\\') {
                return i + 2;
            }
            i++;
        }
        return i;
    }

    return i + 1;
}

static void sanitize_input(const char *input, char *output,
                           size_t output_size)
{
    const unsigned char *bytes = (const unsigned char *)input;
    size_t input_position = 0;
    size_t output_length = 0;

    while (bytes[input_position] != '\0') {
        unsigned char character = bytes[input_position];

        if (character == 0x1BU) {
            input_position = skip_escape_sequence(bytes, input_position);
            continue;
        }

        if (character == '\b' || character == 0x7FU) {
            if (output_length > 0) {
                output_length--;
            }
            input_position++;
            continue;
        }

        if (character == '\t') {
            character = ' ';
        } else if (character == '\r' || character < 0x20U) {
            input_position++;
            continue;
        }

        if (output_length + 1 < output_size) {
            output[output_length++] = (char)character;
        }
        input_position++;
    }

    output[output_length] = '\0';
}

static int discard_rest_of_line(void)
{
    int character;

    do {
        errno = 0;
        character = getchar();

        if (character == EOF && alarm_expired) {
            clearerr(stdin);
            return INPUT_TIMEOUT;
        }

        if (character == EOF && ferror(stdin)) {
            if (errno == EINTR) {
                clearerr(stdin);
                continue;
            }
            perror("getchar");
            clearerr(stdin);
            return INPUT_ERROR;
        }
    } while (character != '\n' && character != EOF);

    return INPUT_OK;
}

static int parse_line_number(const char *text, long *line_number)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);

    if (errno == ERANGE || end == text) {
        return -1;
    }

    while (*end == ' ' || *end == '\t') {
        end++;
    }

    if (*end != '\0') {
        return -1;
    }

    *line_number = value;
    return 0;
}

static int read_line_number(long *line_number)
{
    char input[INPUT_SIZE];
    char sanitized[INPUT_SIZE];
    size_t input_length;
    int has_newline;

    errno = 0;
    if (fgets(input, sizeof(input), stdin) == NULL) {
        if (alarm_expired || errno == EINTR) {
            clearerr(stdin);
            return INPUT_TIMEOUT;
        }
        if (ferror(stdin)) {
            perror("fgets");
            clearerr(stdin);
            return INPUT_ERROR;
        }
        return INPUT_EOF;
    }

    if (alarm_expired) {
        return INPUT_TIMEOUT;
    }

    input_length = strlen(input);
    has_newline = input_length > 0 && input[input_length - 1] == '\n';

    if (has_newline) {
        input[--input_length] = '\0';
    } else if (input_length > MAX_INPUT_BYTES || !feof(stdin)) {
        int discard_result = discard_rest_of_line();
        if (discard_result != INPUT_OK) {
            return discard_result;
        }
        return INPUT_TOO_LONG;
    }

    sanitize_input(input, sanitized, sizeof(sanitized));

    if (parse_line_number(sanitized, line_number) == -1) {
        return INPUT_INVALID;
    }

    return INPUT_OK;
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
    struct sigaction action;

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

    action.sa_handler = alarm_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if (sigaction(SIGALRM, &action, NULL) == -1) {
        perror("sigaction");
        free(table);
        if (mapping != NULL) {
            munmap(mapping, file_size);
        }
        return EXIT_FAILURE;
    }

    if (restrict_terminal_commands() == -1) {
        free(table);
        if (mapping != NULL) {
            munmap(mapping, file_size);
        }
        return EXIT_FAILURE;
    }

    for (;;) {
        printf("Введите номер строки за 5 секунд (0 — выход): ");
        fflush(stdout);

        alarm_expired = 0;
        alarm(5);
        input_result = read_line_number(&line_number);
        alarm(0);

        if (input_result == INPUT_TIMEOUT || alarm_expired) {
            if (isatty(STDIN_FILENO)) {
                tcflush(STDIN_FILENO, TCIFLUSH);
            }
            restore_terminal();
            printf("\nВремя вышло. Содержимое файла:\n");

            if (print_entire_file(mapping, file_size) == -1) {
                status = EXIT_FAILURE;
            }
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

        if (input_result == INPUT_TOO_LONG) {
            fprintf(stderr, "Ввод слишком длинный. Максимум %d байт.\n",
                    MAX_INPUT_BYTES);
            continue;
        }

        if (input_result == INPUT_INVALID) {
            fprintf(stderr, "Введите целый номер строки.\n");
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

    return status;
}
