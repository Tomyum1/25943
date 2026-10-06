#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define INPUT_SIZE 128

typedef struct LineInfo
{
    off_t offset;
    size_t length;
} LineInfo;

static int add_line(LineInfo **table,
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
            new_capacity = 16;
        }
        else
        {
            new_capacity = *capacity * 2;
        }

        new_table = realloc(
            *table,
            new_capacity * sizeof(**table)
        );

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

static int read_exactly(int fd, char *buffer, size_t length)
{
    size_t total;
    ssize_t result;

    total = 0;

    while (total < length)
    {
        result = read(fd, buffer + total, length - total);

        if (result == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("read");
            return -1;
        }

        if (result == 0)
        {
            fprintf(stderr, "Unexpected end of file\n");
            return -1;
        }

        total += (size_t)result;
    }

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

int main(int argc, char *argv[])
{
    int fd;
    LineInfo *table;
    size_t line_count;
    size_t capacity;

    char character;
    ssize_t result;

    off_t line_start;
    off_t current_position;
    off_t line_length;

    char input[INPUT_SIZE];
    long line_number;
    LineInfo selected_line;
    char *line_buffer;

    size_t i;
    int status;

    if (argc != 2)
    {
        fprintf(
            stderr,
            "Usage: %s text-file\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    fd = open(argv[1], O_RDONLY);

    if (fd == -1)
    {
        perror("open");
        return EXIT_FAILURE;
    }

    table = NULL;
    line_count = 0;
    capacity = 0;
    line_start = 0;
    status = EXIT_SUCCESS;

    if (lseek(fd, 0L, SEEK_SET) == (off_t)-1)
    {
        perror("lseek");
        close(fd);
        return EXIT_FAILURE;
    }

    for (;;)
    {
        result = read(fd, &character, 1);

        if (result == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("read");
            free(table);
            close(fd);
            return EXIT_FAILURE;
        }

        if (result == 0)
        {
            break;
        }

        current_position = lseek(fd, 0L, SEEK_CUR);

        if (current_position == (off_t)-1)
        {
            perror("lseek");
            free(table);
            close(fd);
            return EXIT_FAILURE;
        }

        if (character == '\n')
        {
            line_length =
                (current_position - 1) - line_start;

            if (line_length < 0)
            {
                fprintf(stderr, "Invalid file position\n");
                free(table);
                close(fd);
                return EXIT_FAILURE;
            }

            if (add_line(
                    &table,
                    &line_count,
                    &capacity,
                    line_start,
                    (size_t)line_length) == -1)
            {
                free(table);
                close(fd);
                return EXIT_FAILURE;
            }

            line_start = current_position;
        }
    }

    current_position = lseek(fd, 0L, SEEK_CUR);

    if (current_position == (off_t)-1)
    {
        perror("lseek");
        free(table);
        close(fd);
        return EXIT_FAILURE;
    }

    if (current_position > line_start)
    {
        line_length = current_position - line_start;

        if (add_line(
                &table,
                &line_count,
                &capacity,
                line_start,
                (size_t)line_length) == -1)
        {
            free(table);
            close(fd);
            return EXIT_FAILURE;
        }
    }

    printf("Line table:\n");
    printf("Number\tOffset\tLength\n");

    for (i = 0; i < line_count; i++)
    {
        printf(
            "%zu\t%lld\t%zu\n",
            i + 1,
            (long long)table[i].offset,
            table[i].length
        );
    }

    for (;;)
    {
        printf("Enter line number (0 to exit): ");
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL)
        {
            if (ferror(stdin))
            {
                perror("fgets");
                status = EXIT_FAILURE;
            }

            break;
        }

        if (parse_line_number(input, &line_number) == -1)
        {
            fprintf(stderr, "Invalid line number\n");
            continue;
        }

        if (line_number == 0)
        {
            break;
        }

        if (line_number < 1 ||
            (size_t)line_number > line_count)
        {
            fprintf(
                stderr,
                "Line %ld does not exist\n",
                line_number
            );

            continue;
        }

        selected_line = table[line_number - 1];

        line_buffer = malloc(selected_line.length + 1);

        if (line_buffer == NULL)
        {
            perror("malloc");
            status = EXIT_FAILURE;
            break;
        }

        if (lseek(
                fd,
                selected_line.offset,
                SEEK_SET) == (off_t)-1)
        {
            perror("lseek");
            free(line_buffer);
            status = EXIT_FAILURE;
            break;
        }

        if (read_exactly(
                fd,
                line_buffer,
                selected_line.length) == -1)
        {
            free(line_buffer);
            status = EXIT_FAILURE;
            break;
        }

        line_buffer[selected_line.length] = '\0';

        printf("%s\n", line_buffer);

        free(line_buffer);
    }

    free(table);

    if (close(fd) == -1)
    {
        perror("close");
        status = EXIT_FAILURE;
    }

    return status;
}