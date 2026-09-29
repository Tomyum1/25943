#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_DATA_FILE "private_data.txt"

static void print_user_ids(const char *stage)
{
    printf("%s\n", stage);
    printf("  real UID:      %ld\n", (long)getuid());
    printf("  effective UID: %ld\n", (long)geteuid());
}

static void try_to_open(const char *path)
{
    FILE *file;

    printf("  fopen(\"%s\", \"r+\"): ", path);
    fflush(stdout);

    file = fopen(path, "r+");

    if (file == NULL) {
        perror("error");
        return;
    }

    puts("success");

    if (fclose(file) == EOF) {
        perror("fclose error");
    }
}

int main(int argc, char *argv[])
{
    const char *path = DEFAULT_DATA_FILE;
    uid_t real_uid;

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [data-file]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 2) {
        path = argv[1];
    }

    print_user_ids("Before setuid:");
    try_to_open(path);

    real_uid = getuid();

    if (setuid(real_uid) == -1) {
        perror("setuid");
        return EXIT_FAILURE;
    }

    putchar('\n');

    print_user_ids("After setuid(getuid()):");
    try_to_open(path);

    return EXIT_SUCCESS;
}