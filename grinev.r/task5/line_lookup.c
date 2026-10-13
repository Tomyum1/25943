#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define INITIAL_CAPACITY 16
#define MAX_INPUT_BYTES 80
#define INPUT_SIZE (MAX_INPUT_BYTES + 2)

typedef struct LineInfo
{
    off_t offset;
    size_t length;
} LineInfo;

enum InputResult
{
    INPUT_ERROR = -1,
    INPUT_EOF = 0,
    INPUT_OK = 1,
    INPUT_INVALID = 2,
    INPUT_TOO_LONG = 3
};

static struct termios saved_terminal;
static int terminal_was_changed = 0;

static void restore_terminal(void)
{
    if (terminal_was_changed)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_terminal);
        terminal_was_changed = 0;
    }
}

static int restrict_terminal_commands(void)
{
    struct termios restricted_terminal;

    if (!isatty(STDIN_FILENO))
    {
        return 0;
    }

    if (tcgetattr(STDIN_FILENO, &saved_terminal) == -1)
    {
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

    if (tcsetattr(STDIN_FILENO, TCSANOW, &restricted_terminal) == -1)
    {
        perror("tcsetattr");
        return -1;
    }

    terminal_was_changed = 1;

    if (atexit(restore_terminal) != 0)
    {
        restore_terminal();
        fprintf(stderr, "Не удалось зарегистрировать восстановление терминала.\n");
        return -1;
    }

    return 0;
}

static void discard_rest_of_line(void)
{
    int character;

    do
    {
        character = getchar();
    }
    while (character != '\n' && character != EOF);
}

static size_t skip_escape_sequence(const unsigned char *input,
    size_t position)
{
    size_t i;

    i = position + 1;

    if (input[i] == '\0')
    {
        return i;
    }

    if (input[i] == '[')
    {
        i++;

        while (input[i] != '\0')
        {
            unsigned char character;

            character = input[i++];

            if (character >= 0x40U && character <= 0x7EU)
            {
                break;
            }
        }

        return i;
    }

    if (input[i] == ']' || input[i] == 'P' ||
        input[i] == 'X' || input[i] == '^' || input[i] == '_')
    {
        i++;

        while (input[i] != '\0')
        {
            if (input[i] == 0x07U)
            {
                i++;
                break;
            }

            if (input[i] == 0x1BU && input[i + 1] == '\\')
            {
                i += 2;
                break;
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
    const unsigned char *bytes;
    size_t input_position;
    size_t output_length;

    bytes = (const unsigned char *)input;
    input_position = 0;
    output_length = 0;

    while (bytes[input_position] != '\0')
    {
        unsigned char character;

        character = bytes[input_position];

        if (character == 0x1BU)
        {
            input_position = skip_escape_sequence(bytes, input_position);
            continue;
        }

        if (character == '\b' || character == 0x7FU)
        {
            if (output_length > 0)
            {
                output_length--;
            }

            input_position++;
            continue;
        }

        if (character == '\t')
        {
            character = ' ';
        }
        else if (character == '\r' || character < 0x20U)
        {
            input_position++;
            continue;
        }

        if (output_length + 1 < output_size)
        {
            output[output_length++] = (char)character;
        }

        input_position++;
    }

    output[output_length] = '\0';
}

static int append_line(LineInfo **table,
    size_t *count,
    size_t *capacity,
    off_t offset,
    size_t length)
{
    LineInfo *new_table;
    size_t new_capacity;

    if (*count == *capacity)
    {
        if (*capacity == 0)
        {
            new_capacity = INITIAL_CAPACITY;
        }
        else
        {
            new_capacity = *capacity * 2;
        }

        new_table = realloc(*table,
            new_capacity * sizeof(**table));

        if (new_table == NULL)
        {
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

static int build_line_table(int fd,
    LineInfo **table,
    size_t *line_count)
{
    char character;
    ssize_t bytes_read;
    off_t line_start;
    off_t current_position;
    off_t line_length;
    size_t capacity;

    *table = NULL;
    *line_count = 0;
    capacity = 0;
    line_start = 0;

    if (lseek(fd, 0L, SEEK_SET) == (off_t)-1)
    {
        perror("lseek");
        return -1;
    }

    for (;;)
    {
        bytes_read = read(fd, &character, 1);

        if (bytes_read == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("read");
            free(*table);
            *table = NULL;
            *line_count = 0;
            return -1;
        }

        if (bytes_read == 0)
        {
            break;
        }

        current_position = lseek(fd, 0L, SEEK_CUR);

        if (current_position == (off_t)-1)
        {
            perror("lseek");
            free(*table);
            *table = NULL;
            *line_count = 0;
            return -1;
        }

        if (character == '\n')
        {
            line_length = (current_position - 1) - line_start;

            if (line_length < 0)
            {
                fprintf(stderr, "Некорректная позиция в файле\n");
                free(*table);
                *table = NULL;
                *line_count = 0;
                return -1;
            }

            if (append_line(table,
                    line_count,
                    &capacity,
                    line_start,
                    (size_t)line_length) == -1)
            {
                free(*table);
                *table = NULL;
                *line_count = 0;
                return -1;
            }

            line_start = current_position;
        }
    }

    current_position = lseek(fd, 0L, SEEK_CUR);

    if (current_position == (off_t)-1)
    {
        perror("lseek");
        free(*table);
        *table = NULL;
        *line_count = 0;
        return -1;
    }

    if (current_position > line_start)
    {
        line_length = current_position - line_start;

        if (append_line(table,
                line_count,
                &capacity,
                line_start,
                (size_t)line_length) == -1)
        {
            free(*table);
            *table = NULL;
            *line_count = 0;
            return -1;
        }
    }

    return 0;
}

static void print_line_table(const LineInfo *table, size_t line_count)
{
    size_t i;

    printf("Таблица строк:\n");
    printf("Номер\tОтступ\tДлина\n");

    for (i = 0; i < line_count; i++)
    {
        printf("%zu\t%lld\t%zu\n",
            i + 1,
            (long long)table[i].offset,
            table[i].length);
    }
}

static int read_exactly(int fd, char *buffer, size_t length)
{
    size_t total;
    ssize_t bytes_read;

    total = 0;

    while (total < length)
    {
        bytes_read = read(fd, buffer + total, length - total);

        if (bytes_read == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("read");
            return -1;
        }

        if (bytes_read == 0)
        {
            fprintf(stderr, "Неожиданный конец файла\n");
            return -1;
        }

        total += (size_t)bytes_read;
    }

    return 0;
}

static int print_requested_line(int fd, const LineInfo *line)
{
    char *buffer;

    buffer = malloc(line->length + 1);

    if (buffer == NULL)
    {
        perror("malloc");
        return -1;
    }

    if (lseek(fd, line->offset, SEEK_SET) == (off_t)-1)
    {
        perror("lseek");
        free(buffer);
        return -1;
    }

    if (read_exactly(fd, buffer, line->length) == -1)
    {
        free(buffer);
        return -1;
    }

    buffer[line->length] = '\0';
    printf("%s\n", buffer);

    free(buffer);
    return 0;
}

static int parse_line_number(const char *text, long *line_number)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);

    if (errno == ERANGE || end == text)
    {
        return -1;
    }

    while (*end == ' ' || *end == '\t')
    {
        end++;
    }

    if (*end != '\n' && *end != '\0')
    {
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

    if (fgets(input, sizeof(input), stdin) == NULL)
    {
        if (ferror(stdin))
        {
            perror("fgets");
            return INPUT_ERROR;
        }

        return INPUT_EOF;
    }

    input_length = strlen(input);
    has_newline = input_length > 0 && input[input_length - 1] == '\n';

    if (has_newline)
    {
        input[--input_length] = '\0';
    }
    else if (input_length > MAX_INPUT_BYTES || !feof(stdin))
    {
        discard_rest_of_line();
        return INPUT_TOO_LONG;
    }

    sanitize_input(input, sanitized, sizeof(sanitized));

    if (parse_line_number(sanitized, line_number) == -1)
    {
        return INPUT_INVALID;
    }

    return INPUT_OK;
}

int main(int argc, char *argv[])
{
    int fd;
    LineInfo *table;
    size_t line_count;
    long line_number;
    int input_result;
    int status;

    if (argc != 2)
    {
        fprintf(stderr, "Использование: %s текстовый-файл\n", argv[0]);
        return EXIT_FAILURE;
    }

    fd = open(argv[1], O_RDONLY);

    if (fd == -1)
    {
        perror("open");
        return EXIT_FAILURE;
    }

    if (build_line_table(fd, &table, &line_count) == -1)
    {
        close(fd);
        return EXIT_FAILURE;
    }

    print_line_table(table, line_count);

    status = EXIT_SUCCESS;

    if (restrict_terminal_commands() == -1)
    {
        free(table);
        close(fd);
        return EXIT_FAILURE;
    }

    for (;;)
    {
        printf("Введите номер строки (0 — выход): ");
        fflush(stdout);

        input_result = read_line_number(&line_number);

        if (input_result == INPUT_EOF)
        {
            putchar('\n');
            break;
        }

        if (input_result == INPUT_ERROR)
        {
            status = EXIT_FAILURE;
            break;
        }

        if (input_result == INPUT_TOO_LONG)
        {
            fprintf(stderr,
                "Ввод слишком длинный. Максимум %d байт.\n",
                MAX_INPUT_BYTES);
            continue;
        }

        if (input_result == INPUT_INVALID)
        {
            fprintf(stderr, "Введите целый номер строки.\n");
            continue;
        }

        if (line_number == 0)
        {
            break;
        }

        if (line_count == 0)
        {
            fprintf(stderr, "Файл не содержит строк.\n");
            continue;
        }

        if (line_number < 1 || (size_t)line_number > line_count)
        {
            fprintf(stderr,
                "Строки с номером %ld нет. Допустимый диапазон: 1–%zu.\n",
                line_number,
                line_count);
            continue;
        }

        if (print_requested_line(fd, &table[line_number - 1]) == -1)
        {
            status = EXIT_FAILURE;
            break;
        }
    }

    restore_terminal();

    free(table);

    if (close(fd) == -1)
    {
        perror("close");
        status = EXIT_FAILURE;
    }

    return status;
}
