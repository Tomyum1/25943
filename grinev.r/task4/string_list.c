#define _XOPEN_SOURCE 600

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define MAX_INPUT_BYTES 80
#define INPUT_BUFFER_SIZE (MAX_INPUT_BYTES + 2)

typedef struct Node
{
    char *text;
    struct Node *next;
} Node;

enum ReadResult
{
    READ_ERROR = -1,
    READ_EOF = 0,
    READ_OK = 1,
    READ_STOP = 2,
    READ_TOO_LONG = 3,
    READ_CONTROL_ONLY = 4
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
        fprintf(stderr, "Cannot register terminal restoration.\n");
        return -1;
    }

    return 0;
}

static void free_list(Node *head)
{
    Node *next;

    while (head != NULL)
    {
        next = head->next;
        free(head->text);
        free(head);
        head = next;
    }
}

static int append_node(Node **head, Node **tail, const char *text)
{
    Node *node;
    size_t length;

    node = malloc(sizeof(*node));

    if (node == NULL)
    {
        perror("malloc");
        return -1;
    }

    length = strlen(text);
    node->text = malloc(length + 1);

    if (node->text == NULL)
    {
        perror("malloc");
        free(node);
        return -1;
    }

    memcpy(node->text, text, length + 1);
    node->next = NULL;

    if (*head == NULL)
    {
        *head = node;
        *tail = node;
    }
    else
    {
        (*tail)->next = node;
        *tail = node;
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

static size_t remove_last_utf8_character(char *text, size_t length)
{
    if (length == 0)
    {
        return 0;
    }

    length--;

    while (length > 0 &&
           ((unsigned char)text[length] & 0xC0U) == 0x80U)
    {
        length--;
    }

    return length;
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
            unsigned char character = input[i++];

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

static size_t sanitize_line(const char *input, char *output,
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
        unsigned char character = bytes[input_position];

        if (character == 0x1BU)
        {
            input_position = skip_escape_sequence(bytes, input_position);
            continue;
        }

        if (character == '\b' || character == 0x7FU)
        {
            output_length = remove_last_utf8_character(
                output, output_length);
            input_position++;
            continue;
        }

        if (character == '\t' || character == '\r')
        {
            input_position++;
            continue;
        }

        if (character < 0x20U)
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
    return output_length;
}

static int read_safe_line(char *output, size_t output_size)
{
    char input[INPUT_BUFFER_SIZE];
    size_t input_length;
    size_t output_length;
    int has_newline;

    if (fgets(input, sizeof(input), stdin) == NULL)
    {
        if (ferror(stdin))
        {
            perror("fgets");
            return READ_ERROR;
        }

        return READ_EOF;
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
        return READ_TOO_LONG;
    }

    if (input_length > 0 && input[input_length - 1] == '\r')
    {
        input[--input_length] = '\0';
    }

    if (input[0] == '.')
    {
        return READ_STOP;
    }

    output_length = sanitize_line(input, output, output_size);

    if (input_length > 0 && output_length == 0)
    {
        return READ_CONTROL_ONLY;
    }

    return READ_OK;
}

int main(void)
{
    Node *head;
    Node *tail;
    Node *current;
    char line[MAX_INPUT_BYTES + 1];
    int read_result;
    int status;

    head = NULL;
    tail = NULL;
    status = EXIT_SUCCESS;

    if (restrict_terminal_commands() == -1)
    {
        return EXIT_FAILURE;
    }

    printf("Enter strings, maximum %d bytes each.\n", MAX_INPUT_BYTES);
    printf("A dot at the beginning finishes input.\n");

    for (;;)
    {
        printf("> ");
        fflush(stdout);

        read_result = read_safe_line(line, sizeof(line));

        if (read_result == READ_STOP || read_result == READ_EOF)
        {
            break;
        }

        if (read_result == READ_ERROR)
        {
            status = EXIT_FAILURE;
            break;
        }

        if (read_result == READ_TOO_LONG)
        {
            fprintf(stderr,
                "Input is too long. Maximum length is %d bytes.\n",
                MAX_INPUT_BYTES);
            continue;
        }

        if (read_result == READ_CONTROL_ONLY)
        {
            fprintf(stderr, "Control input was ignored.\n");
            continue;
        }

        if (append_node(&head, &tail, line) == -1)
        {
            status = EXIT_FAILURE;
            break;
        }
    }

    if (status == EXIT_SUCCESS)
    {
        restore_terminal();
        printf("Stored strings:\n");

        for (current = head; current != NULL; current = current->next)
        {
            puts(current->text);
        }
    }

    restore_terminal();

    free_list(head);
    return status;
}
