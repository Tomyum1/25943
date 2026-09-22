#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ulimit.h>
#include <sys/types.h>
#include <sys/resource.h>

extern char **environ;

int main(int argc, char *argv[])
{
    char valid_options[] = "ispuU:cC:dvV:";
    int saved_options[100];
    char *saved_arguments[100];

    int c;
    int count = 0;
    int i;
    int j;
    int status = 0;

    long value;
    char *end;
    char directory[1024];
    struct rlimit limit;

    opterr = 0;

    
     //Сначала только запоминаем опции.
     
    while ((c = getopt(argc, argv, valid_options)) != -1) {
        if (c == '?') {
            printf("Недопустимая опция: -%c\n", optopt);
            return 1;
        }

        if (count == 100) {
            printf("Слишком много опций\n");
            return 1;
        }

        saved_options[count] = c;
        saved_arguments[count] = optarg;
        count++;
    }

    
     // Выполняем опции справа налево.
     
    for (i = count - 1; i >= 0; i--) {
        switch (saved_options[i]) {
        case 'i':
            printf("Real UID: %ld\n", (long)getuid());
            printf("Effective UID: %ld\n", (long)geteuid());
            printf("Real GID: %ld\n", (long)getgid());
            printf("Effective GID: %ld\n", (long)getegid());
            break;

        case 's':
            if (setpgid(0, 0) == -1) {
                perror("setpgid");
                status = 1;
            }
            break;

        case 'p':
            printf("PID: %ld\n", (long)getpid());
            printf("PPID: %ld\n", (long)getppid());
            printf("PGRP: %ld\n", (long)getpgrp());
            break;

        case 'u':
            printf("ulimit: %ld\n", ulimit(UL_GETFSIZE));
            break;

        case 'U':
            value = strtol(saved_arguments[i], &end, 10);

            if (saved_arguments[i][0] == '\0' ||
                *end != '\0' ||
                value < 0) {
                printf("Неудачное значение для -U: %s\n",
                    saved_arguments[i]);
                status = 1;
            } else if (ulimit(UL_SETFSIZE, value) == -1) {
                perror("ulimit");
                status = 1;
            }
            break;

        case 'c':
            if (getrlimit(RLIMIT_CORE, &limit) == -1) {
                perror("getrlimit");
                status = 1;
            } else if (limit.rlim_cur == RLIM_INFINITY) {
                printf("Core size: unlimited\n");
            } else {
                printf("Core size: %lu bytes\n",
                    (unsigned long)limit.rlim_cur);
            }
            break;

        case 'C':
            value = strtol(saved_arguments[i], &end, 10);

            if (saved_arguments[i][0] == '\0' ||
                *end != '\0' ||
                value < 0) {
                printf("Неудачное значение для -C: %s\n",
                    saved_arguments[i]);
                status = 1;
            } else if (getrlimit(RLIMIT_CORE, &limit) == -1) {
                perror("getrlimit");
                status = 1;
            } else {
                limit.rlim_cur = (rlim_t)value;

                if (setrlimit(RLIMIT_CORE, &limit) == -1) {
                    perror("setrlimit");
                    status = 1;
                }
            }
            break;

        case 'd':
            if (getcwd(directory, sizeof(directory)) == NULL) {
                perror("getcwd");
                status = 1;
            } else {
                printf("%s\n", directory);
            }
            break;

        case 'v':
            for (j = 0; environ[j] != NULL; j++)
                printf("%s\n", environ[j]);
            break;

        case 'V':
            if (strchr(saved_arguments[i], '=') == NULL) {
                printf("После -V требуется name=value\n");
                status = 1;
            } else if (putenv(saved_arguments[i]) != 0) {
                perror("putenv");
                status = 1;
            }
            break;
        }
    }

    return status;
}