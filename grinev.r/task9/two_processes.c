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

    if (argc != 2) {
        fprintf(stderr, "Использование: %s файл\n", argv[0]);
        return EXIT_FAILURE;
    }

    fflush(NULL);
    child_pid = fork();

    if (child_pid == -1) {
        perror("fork");
        return EXIT_FAILURE;
    }

    if (child_pid == 0) {
        execlp("cat", "cat", argv[1], (char *)NULL);
        perror("execlp cat");
        _exit(127);
    }

    printf("Родитель: создан дочерний процесс PID=%ld.\n",
           (long)child_pid);
    printf("Родитель: ожидаю завершения команды cat.\n");
    fflush(stdout);

    if (wait_for_child(child_pid, &status) == -1) {
        return EXIT_FAILURE;
    }

    if (WIFEXITED(status)) {
        printf("Родитель: cat завершился с кодом %d.\n",
               WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        printf("Родитель: cat завершён сигналом %d.\n",
               WTERMSIG(status));
    } else {
        printf("Родитель: состояние дочернего процесса изменилось.\n");
    }

    printf("Родитель: эта строка напечатана после завершения потомка.\n");

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
