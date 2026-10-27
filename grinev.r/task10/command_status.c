#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int wait_for_child(pid_t child_pid, int *status)
{
    pid_t result;

    do {
        result = waitpid(child_pid, status, 0);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        perror("waitpid");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    pid_t child_pid;
    int status;

    if (argc < 2) {
        fprintf(stderr,
                "Использование: %s команда [аргументы ...]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    fflush(NULL);
    child_pid = fork();

    if (child_pid == -1) {
        perror("fork");
        return EXIT_FAILURE;
    }

    if (child_pid == 0) {
        execvp(argv[1], &argv[1]);
        perror("execvp");
        _exit(127);
    }

    if (wait_for_child(child_pid, &status) == -1) {
        return EXIT_FAILURE;
    }

    if (WIFEXITED(status)) {
        printf("Код завершения команды: %d\n", WEXITSTATUS(status));
        return EXIT_SUCCESS;
    }

    if (WIFSIGNALED(status)) {
        printf("Команда завершена сигналом: %d\n", WTERMSIG(status));
        return EXIT_FAILURE;
    }

    fprintf(stderr, "Не удалось определить код завершения команды.\n");
    return EXIT_FAILURE;
}
