#define _XOPEN_SOURCE 600

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ulimit.h>
#include <sys/types.h>
#include <sys/resource.h>

#define MAX_OPTIONS 100

extern char **environ;

static void print_help(FILE *stream, const char *program)
{
    fprintf(stream,
        "Использование: %s [опции]\n"
        "Опции выполняются справа налево.\n\n"
        "  -h              показать эту справку\n"
        "  -i              показать реальные и эффективные UID/GID\n"
        "  -s              сделать процесс лидером группы и показать до/после\n"
        "  -p              показать PID, PPID и PGRP\n"
        "  -u              показать текущий ulimit\n"
        "  -Uчисло         изменить ulimit текущего процесса\n"
        "  -c              показать допустимый размер core-файла\n"
        "  -Cчисло         изменить допустимый размер core-файла\n"
        "  -d              показать текущую рабочую директорию\n"
        "  -v              показать переменные окружения\n"
        "  -Vname=value    добавить или изменить переменную окружения\n\n"
        "Примеры:\n"
        "  %s -i -p -d\n"
        "  %s -u -U100\n"
        "  %s -v -VTEST=hello\n",
        program, program, program, program);
}

static int parse_nonnegative_long(const char *text, long *result)
{
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);

    if (errno == ERANGE || end == text || *end != '\0' || value < 0) {
        return -1;
    }

    *result = value;
    return 0;
}

static void print_process_ids(void)
{
    pid_t pid = getpid();
    pid_t pgrp = getpgrp();

    printf("  PID:  %ld\n", (long)pid);
    printf("  PPID: %ld\n", (long)getppid());
    printf("  PGRP: %ld\n", (long)pgrp);
    printf("  Лидер группы: %s\n", pid == pgrp ? "да" : "нет");
}

int main(int argc, char *argv[])
{
    const char valid_options[] = ":hispuU:cC:dvV:";
    int saved_options[MAX_OPTIONS];
    char *saved_arguments[MAX_OPTIONS];
    int option;
    int count = 0;
    int i;
    int j;
    int status = EXIT_SUCCESS;
    long value;
    long old_value;
    long new_value;
    char directory[1024];
    struct rlimit limit;
    struct rlimit file_limit_before;
    struct rlimit file_limit_after;

    opterr = 0;

    if (argc == 1) {
        print_help(stdout, argv[0]);
        return EXIT_SUCCESS;
    }

    /* Сначала только разбираем и сохраняем опции. */
    while ((option = getopt(argc, argv, valid_options)) != -1) {
        if (option == 'h') {
            print_help(stdout, argv[0]);
            return EXIT_SUCCESS;
        }

        if (option == '?') {
            fprintf(stderr, "Недопустимая опция: -%c\n\n", optopt);
            print_help(stderr, argv[0]);
            return EXIT_FAILURE;
        }

        if (option == ':') {
            fprintf(stderr, "Для опции -%c требуется значение.\n\n", optopt);
            print_help(stderr, argv[0]);
            return EXIT_FAILURE;
        }

        if (count == MAX_OPTIONS) {
            fprintf(stderr, "Слишком много опций.\n\n");
            print_help(stderr, argv[0]);
            return EXIT_FAILURE;
        }

        saved_options[count] = option;
        saved_arguments[count] = optarg;
        count++;
    }

    if (optind < argc) {
        fprintf(stderr, "Неожиданный аргумент: %s\n\n", argv[optind]);
        print_help(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    /* Выполняем сохранённые опции справа налево. */
    for (i = count - 1; i >= 0; i--) {
        switch (saved_options[i]) {
        case 'i':
            printf("Real UID: %ld\n", (long)getuid());
            printf("Effective UID: %ld\n", (long)geteuid());
            printf("Real GID: %ld\n", (long)getgid());
            printf("Effective GID: %ld\n", (long)getegid());
            break;

        case 's':
            printf("До setpgid():\n");
            print_process_ids();

            if (getpid() == getpgrp()) {
                printf("Процесс уже является лидером группы.\n");
            } else {
                if (setpgid(0, 0) == -1) {
                    perror("setpgid");
                    status = EXIT_FAILURE;
                    break;
                }
            }

            printf("После setpgid(0, 0):\n");
            print_process_ids();
            break;

        case 'p':
            print_process_ids();
            break;

        case 'u':
            if (getrlimit(RLIMIT_FSIZE, &file_limit_after) == -1) {
                perror("getrlimit(RLIMIT_FSIZE)");
                status = EXIT_FAILURE;
            } else if (file_limit_after.rlim_cur == RLIM_INFINITY) {
                printf("ulimit: unlimited\n");
            } else {
                errno = 0;
                value = ulimit(UL_GETFSIZE);
                if (value == -1 && errno != 0) {
                    perror("ulimit");
                    status = EXIT_FAILURE;
                } else {
                    printf("ulimit: %ld блоков по 512 байт\n", value);
                }
            }
            break;

        case 'U':
            if (parse_nonnegative_long(saved_arguments[i], &value) == -1) {
                fprintf(stderr, "Неудачное значение для -U: %s\n",
                    saved_arguments[i]);
                status = EXIT_FAILURE;
                break;
            }

            if (getrlimit(RLIMIT_FSIZE, &file_limit_before) == -1) {
                perror("getrlimit(RLIMIT_FSIZE)");
                status = EXIT_FAILURE;
                break;
            }

            errno = 0;
            old_value = ulimit(UL_GETFSIZE);
            if (old_value == -1 && errno != 0) {
                perror("ulimit(UL_GETFSIZE)");
                status = EXIT_FAILURE;
                break;
            }

            if (ulimit(UL_SETFSIZE, value) == -1) {
                perror("ulimit(UL_SETFSIZE)");
                status = EXIT_FAILURE;
                break;
            }

            if (getrlimit(RLIMIT_FSIZE, &file_limit_after) == -1) {
                perror("getrlimit(RLIMIT_FSIZE)");
                status = EXIT_FAILURE;
                break;
            }

            errno = 0;
            new_value = ulimit(UL_GETFSIZE);
            if (new_value == -1 && errno != 0) {
                perror("ulimit(UL_GETFSIZE)");
                status = EXIT_FAILURE;
                break;
            }

            printf("ulimit процесса %ld изменён: ", (long)getpid());
            if (file_limit_before.rlim_cur == RLIM_INFINITY) {
                printf("unlimited");
            } else {
                printf("%ld", old_value);
            }
            printf(" -> ");
            if (file_limit_after.rlim_cur == RLIM_INFINITY) {
                printf("unlimited");
            } else {
                printf("%ld", new_value);
            }
            printf(" блоков по 512 байт\n");
            break;

        case 'c':
            if (getrlimit(RLIMIT_CORE, &limit) == -1) {
                perror("getrlimit");
                status = EXIT_FAILURE;
            } else if (limit.rlim_cur == RLIM_INFINITY) {
                printf("Core size: unlimited\n");
            } else {
                printf("Core size: %lu bytes\n",
                    (unsigned long)limit.rlim_cur);
            }
            break;

        case 'C':
            if (parse_nonnegative_long(saved_arguments[i], &value) == -1) {
                fprintf(stderr, "Неудачное значение для -C: %s\n",
                    saved_arguments[i]);
                status = EXIT_FAILURE;
                break;
            }

            if (getrlimit(RLIMIT_CORE, &limit) == -1) {
                perror("getrlimit");
                status = EXIT_FAILURE;
                break;
            }

            limit.rlim_cur = (rlim_t)value;
            if (setrlimit(RLIMIT_CORE, &limit) == -1) {
                perror("setrlimit");
                status = EXIT_FAILURE;
            }
            break;

        case 'd':
            if (getcwd(directory, sizeof(directory)) == NULL) {
                perror("getcwd");
                status = EXIT_FAILURE;
            } else {
                printf("%s\n", directory);
            }
            break;

        case 'v':
            for (j = 0; environ[j] != NULL; j++) {
                printf("%s\n", environ[j]);
            }
            break;

        case 'V':
            if (saved_arguments[i][0] == '=' ||
                strchr(saved_arguments[i], '=') == NULL) {
                fprintf(stderr, "После -V требуется name=value\n");
                status = EXIT_FAILURE;
            } else if (putenv(saved_arguments[i]) != 0) {
                perror("putenv");
                status = EXIT_FAILURE;
            }
            break;
        }
    }

    if (status != EXIT_SUCCESS) {
        fputc('\n', stderr);
        print_help(stderr, argv[0]);
    }

    return status;
}
