#ifndef __EXTENSIONS__
#define __EXTENSIONS__ 1
#endif

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define MAX_INPUT_BYTES 80
#define ESCAPE_WAIT_MS 100

typedef struct Node
{
    char *text;
    struct Node *next;
} Node;

enum ReadResult
{
    READ_SIGNAL = -2,
    READ_ERROR = -1,
    READ_EOF = 0,
    READ_OK = 1,
    READ_STOP = 2,
    READ_TOO_LONG = 3,
    READ_CONTROL_ONLY = 4,
    READ_INVALID_UTF8 = 5
};

enum ByteResult
{
    BYTE_SIGNAL = -3,
    BYTE_TIMEOUT = -2,
    BYTE_ERROR = -1,
    BYTE_EOF = 0,
    BYTE_OK = 1
};

static struct termios saved_terminal;
static int terminal_was_changed = 0;
static int pending_byte = -1;
static int skip_line_feed = 0;
static volatile sig_atomic_t stopped_signal = 0;

static void signal_handler(int signal_number)
{
    stopped_signal = signal_number;
}

static void restore_terminal(void)
{
    if (terminal_was_changed)
    {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_terminal);
        terminal_was_changed = 0;
    }
}

static int install_signal_handler(int signal_number)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);

    return sigaction(signal_number, &action, NULL);
}

static int prepare_terminal(void)
{
    struct termios raw_terminal;

    if (install_signal_handler(SIGINT) == -1 ||
        install_signal_handler(SIGTERM) == -1 ||
        install_signal_handler(SIGHUP) == -1 ||
        install_signal_handler(SIGQUIT) == -1)
    {
        perror("sigaction");
        return -1;
    }

    if (!isatty(STDIN_FILENO))
    {
        return 0;
    }

    if (tcgetattr(STDIN_FILENO, &saved_terminal) == -1)
    {
        perror("tcgetattr");
        return -1;
    }

    raw_terminal = saved_terminal;
    raw_terminal.c_lflag &=
        (tcflag_t)~(ICANON | ECHO | ECHONL | ISIG | IEXTEN);
    raw_terminal.c_iflag &=
        (tcflag_t)~(IXON | IXOFF | ICRNL | INLCR | IGNCR | ISTRIP);
    raw_terminal.c_cc[VMIN] = 1;
    raw_terminal.c_cc[VTIME] = 0;

#ifdef VREPRINT
    raw_terminal.c_cc[VREPRINT] = _POSIX_VDISABLE;
#endif
#ifdef VDISCARD
    raw_terminal.c_cc[VDISCARD] = _POSIX_VDISABLE;
#endif
#ifdef VLNEXT
    raw_terminal.c_cc[VLNEXT] = _POSIX_VDISABLE;
#endif

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_terminal) == -1)
    {
        perror("tcsetattr");
        return -1;
    }

    terminal_was_changed = 1;
    return 0;
}

static int read_byte(unsigned char *character, int timeout_ms)
{
    struct pollfd descriptor;
    ssize_t count;
    int poll_result;
    int wait_ms;

    if (pending_byte >= 0)
    {
        *character = (unsigned char)pending_byte;
        pending_byte = -1;
        return BYTE_OK;
    }

    descriptor.fd = STDIN_FILENO;
    descriptor.events = POLLIN;
    descriptor.revents = 0;

    for (;;)
    {
        if (stopped_signal != 0)
        {
            return BYTE_SIGNAL;
        }

        wait_ms = timeout_ms >= 0 ? timeout_ms : 100;
        poll_result = poll(&descriptor, 1, wait_ms);

        if (poll_result == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("poll");
            return BYTE_ERROR;
        }

        if (poll_result == 0)
        {
            if (timeout_ms >= 0)
            {
                return BYTE_TIMEOUT;
            }

            continue;
        }

        if ((descriptor.revents & POLLIN) != 0)
        {
            count = read(STDIN_FILENO, character, 1);

            if (count == 1)
            {
                return BYTE_OK;
            }

            if (count == 0)
            {
                return BYTE_EOF;
            }

            if (errno == EINTR)
            {
                continue;
            }

            perror("read");
            return BYTE_ERROR;
        }

        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            count = read(STDIN_FILENO, character, 1);

            if (count == 1)
            {
                return BYTE_OK;
            }

            if (count == 0)
            {
                return BYTE_EOF;
            }

            if (errno == EINTR)
            {
                continue;
            }

            perror("read");
            return BYTE_ERROR;
        }
    }
}

static int skip_escape_sequence(void)
{
    unsigned char character;
    int result;
    int string_sequence;

    result = read_byte(&character, ESCAPE_WAIT_MS);

    if (result == BYTE_TIMEOUT || result == BYTE_EOF)
    {
        return BYTE_OK;
    }

    if (result != BYTE_OK)
    {
        return result;
    }

    if (character == '[' || character == 'O')
    {
        for (;;)
        {
            result = read_byte(&character, ESCAPE_WAIT_MS);

            if (result == BYTE_TIMEOUT || result == BYTE_EOF)
            {
                return BYTE_OK;
            }

            if (result != BYTE_OK)
            {
                return result;
            }

            if (character >= 0x40U && character <= 0x7EU)
            {
                return BYTE_OK;
            }
        }
    }

    string_sequence = character == ']' || character == 'P' ||
                      character == 'X' || character == '^' ||
                      character == '_';

    if (string_sequence)
    {
        int previous_was_escape = 0;

        for (;;)
        {
            result = read_byte(&character, ESCAPE_WAIT_MS);

            if (result == BYTE_TIMEOUT || result == BYTE_EOF)
            {
                return BYTE_OK;
            }

            if (result != BYTE_OK)
            {
                return result;
            }

            if (character == 0x07U ||
                (previous_was_escape && character == '\\'))
            {
                return BYTE_OK;
            }

            previous_was_escape = character == 0x1BU;
        }
    }

    pending_byte = character;
    return BYTE_OK;
}

static void show_input(const char *text, size_t length)
{
    struct winsize window_size;
    size_t width;
    size_t start;

    if (!isatty(STDIN_FILENO))
    {
        return;
    }

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window_size) == 0 &&
        window_size.ws_col > 4)
    {
        width = (size_t)window_size.ws_col - 4;
    }
    else
    {
        width = length;
    }

    start = length > width ? length - width : 0;

    while (start < length &&
           ((unsigned char)text[start] & 0xC0U) == 0x80U)
    {
        start++;
    }

    printf("\r\033[2K> %.*s", (int)(length - start), text + start);
    fflush(stdout);
}

static void finish_input_display(void)
{
    if (isatty(STDIN_FILENO))
    {
        putchar('\n');
        fflush(stdout);
    }
}

static size_t remove_last_utf8_character(const char *text, size_t length)
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

static size_t utf8_sequence_length(unsigned char first_byte)
{
    if (first_byte >= 0xC2U && first_byte <= 0xDFU)
    {
        return 2;
    }

    if (first_byte >= 0xE0U && first_byte <= 0xEFU)
    {
        return 3;
    }

    if (first_byte >= 0xF0U && first_byte <= 0xF4U)
    {
        return 4;
    }

    return 0;
}

static int read_utf8_character(unsigned char first_byte,
                               unsigned char sequence[4],
                               size_t *sequence_length)
{
    size_t expected_length;
    size_t i;
    int result;

    expected_length = utf8_sequence_length(first_byte);

    if (expected_length == 0)
    {
        return 0;
    }

    sequence[0] = first_byte;

    for (i = 1; i < expected_length; i++)
    {
        result = read_byte(&sequence[i], -1);

        if (result != BYTE_OK)
        {
            return result;
        }

        if ((sequence[i] & 0xC0U) != 0x80U)
        {
            pending_byte = sequence[i];
            return 0;
        }
    }

    if ((first_byte == 0xE0U && sequence[1] < 0xA0U) ||
        (first_byte == 0xEDU && sequence[1] > 0x9FU) ||
        (first_byte == 0xF0U && sequence[1] < 0x90U) ||
        (first_byte == 0xF4U && sequence[1] > 0x8FU))
    {
        return 0;
    }

    *sequence_length = expected_length;
    return BYTE_OK;
}

static int read_safe_line(char output[MAX_INPUT_BYTES + 1])
{
    unsigned char character;
    unsigned char sequence[4];
    size_t length;
    size_t sequence_length;
    int result;
    int control_seen;
    int invalid_utf8;
    int too_long;

    length = 0;
    control_seen = 0;
    invalid_utf8 = 0;
    too_long = 0;
    output[0] = '\0';

    if (isatty(STDIN_FILENO))
    {
        show_input(output, length);
    }
    else
    {
        printf("> ");
        fflush(stdout);
    }

    for (;;)
    {
        result = read_byte(&character, -1);

        if (result == BYTE_SIGNAL)
        {
            finish_input_display();
            return READ_SIGNAL;
        }

        if (result == BYTE_ERROR)
        {
            finish_input_display();
            return READ_ERROR;
        }

        if (result == BYTE_EOF)
        {
            finish_input_display();

            if (length == 0 && !control_seen && !too_long && !invalid_utf8)
            {
                return READ_EOF;
            }

            break;
        }

        if (skip_line_feed)
        {
            skip_line_feed = 0;

            if (character == '\n')
            {
                continue;
            }
        }

        if (character == '\r' || character == '\n')
        {
            if (character == '\r')
            {
                skip_line_feed = 1;
            }

            finish_input_display();
            break;
        }

        if (character == 0x04U)
        {
            finish_input_display();

            if (length == 0 && !control_seen && !too_long && !invalid_utf8)
            {
                return READ_EOF;
            }

            break;
        }

        if (character == 0x03U)
        {
            stopped_signal = SIGINT;
            finish_input_display();
            return READ_SIGNAL;
        }

        if (character == 0x1BU)
        {
            control_seen = 1;
            result = skip_escape_sequence();

            if (result == BYTE_SIGNAL)
            {
                finish_input_display();
                return READ_SIGNAL;
            }

            if (result == BYTE_ERROR)
            {
                finish_input_display();
                return READ_ERROR;
            }

            continue;
        }

        if (character == '\b' || character == 0x7FU)
        {
            length = remove_last_utf8_character(output, length);
            output[length] = '\0';
            show_input(output, length);
            continue;
        }

        if (character == '\t' || character < 0x20U)
        {
            control_seen = 1;
            continue;
        }

        if (character < 0x80U)
        {
            if (length < MAX_INPUT_BYTES)
            {
                output[length++] = (char)character;
                output[length] = '\0';
                show_input(output, length);
            }
            else
            {
                too_long = 1;
            }

            continue;
        }

        result = read_utf8_character(character, sequence, &sequence_length);

        if (result == BYTE_SIGNAL)
        {
            finish_input_display();
            return READ_SIGNAL;
        }

        if (result == BYTE_ERROR)
        {
            finish_input_display();
            return READ_ERROR;
        }

        if (result != BYTE_OK)
        {
            invalid_utf8 = 1;
            continue;
        }

        if (length + sequence_length <= MAX_INPUT_BYTES)
        {
            memcpy(output + length, sequence, sequence_length);
            length += sequence_length;
            output[length] = '\0';
            show_input(output, length);
        }
        else
        {
            too_long = 1;
        }
    }

    output[length] = '\0';

    if (output[0] == '.')
    {
        return READ_STOP;
    }

    if (too_long)
    {
        return READ_TOO_LONG;
    }

    if (invalid_utf8)
    {
        return READ_INVALID_UTF8;
    }

    if (length == 0 && control_seen)
    {
        return READ_CONTROL_ONLY;
    }

    return READ_OK;
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

    if (atexit(restore_terminal) != 0)
    {
        fprintf(stderr, "Cannot register terminal restoration.\n");
        return EXIT_FAILURE;
    }

    if (prepare_terminal() == -1)
    {
        return EXIT_FAILURE;
    }

    printf("Enter strings, maximum %d bytes each.\n", MAX_INPUT_BYTES);
    printf("A dot at the beginning finishes input.\n");

    for (;;)
    {
        read_result = read_safe_line(line);

        if (read_result == READ_STOP || read_result == READ_EOF)
        {
            break;
        }

        if (read_result == READ_SIGNAL)
        {
            status = 128 + (int)stopped_signal;
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

        if (read_result == READ_INVALID_UTF8)
        {
            fprintf(stderr, "Invalid UTF-8 input was ignored.\n");
            continue;
        }

        if (append_node(&head, &tail, line) == -1)
        {
            status = EXIT_FAILURE;
            break;
        }
    }

    restore_terminal();

    if (status == EXIT_SUCCESS)
    {
        printf("Stored strings:\n");

        for (current = head; current != NULL; current = current->next)
        {
            puts(current->text);
        }
    }

    free_list(head);
    return status;
}
