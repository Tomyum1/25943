#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE_LENGTH 1024

typedef struct Node
{
    char *text;
    struct Node *next;
} Node;

static int append_line(Node **head, Node **tail, const char *text)
{
    Node *new_node;
    size_t length;

    new_node = malloc(sizeof(*new_node));

    if (new_node == NULL)
    {
        perror("malloc node");
        return -1;
    }

    length = strlen(text);

    new_node->text = malloc(length + 1);

    if (new_node->text == NULL)
    {
        perror("malloc text");
        free(new_node);
        return -1;
    }

    memcpy(new_node->text, text, length + 1);
    new_node->next = NULL;

    if (*tail == NULL)
    {
        *head = new_node;
        *tail = new_node;
    }
    else
    {
        (*tail)->next = new_node;
        *tail = new_node;
    }

    return 0;
}

static void print_list(const Node *head)
{
    const Node *current;

    current = head;

    while (current != NULL)
    {
        puts(current->text);
        current = current->next;
    }
}

static void free_list(Node *head)
{
    Node *current;
    Node *next;

    current = head;

    while (current != NULL)
    {
        next = current->next;

        free(current->text);
        free(current);

        current = next;
    }
}

int main(void)
{
    char buffer[MAX_LINE_LENGTH];
    Node *head;
    Node *tail;
    size_t length;

    head = NULL;
    tail = NULL;

    while (fgets(buffer, sizeof(buffer), stdin) != NULL)
    {
        if (buffer[0] == '.')
        {
            break;
        }

        length = strlen(buffer);

        if (length > 0 && buffer[length - 1] == '\n')
        {
            buffer[length - 1] = '\0';
        }

        if (append_line(&head, &tail, buffer) == -1)
        {
            free_list(head);
            return EXIT_FAILURE;
        }
    }

    if (ferror(stdin))
    {
        perror("fgets");
        free_list(head);
        return EXIT_FAILURE;
    }

    print_list(head);
    free_list(head);

    return EXIT_SUCCESS;
}