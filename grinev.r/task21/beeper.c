#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t signal_count = 0;
static volatile sig_atomic_t quit_requested = 0;

static void signal_handler(int signal_number)
{
    int saved_errno;

    saved_errno = errno;

    if (signal_number == SIGINT) {
        signal_count++;
        (void)write(STDOUT_FILENO, "\a", 1);
    } else if (signal_number == SIGQUIT) {
        quit_requested = 1;
    }

    errno = saved_errno;
}

static int install_handler(int signal_number)
{
    struct sigaction action;

    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;

    return sigaction(signal_number, &action, NULL);
}

int main(void)
{
    sigset_t blocked_signals;
    sigset_t previous_mask;
    sigset_t wait_mask;
    int final_count;

    sigemptyset(&blocked_signals);
    sigaddset(&blocked_signals, SIGINT);
    sigaddset(&blocked_signals, SIGQUIT);

    if (sigprocmask(SIG_BLOCK, &blocked_signals, &previous_mask) == -1) {
        perror("sigprocmask");
        return EXIT_FAILURE;
    }

    if (install_handler(SIGINT) == -1 ||
        install_handler(SIGQUIT) == -1) {
        perror("sigaction");
        (void)sigprocmask(SIG_SETMASK, &previous_mask, NULL);
        return EXIT_FAILURE;
    }

    wait_mask = previous_mask;
    sigdelset(&wait_mask, SIGINT);
    sigdelset(&wait_mask, SIGQUIT);

    printf("PID программы: %ld\n", (long)getpid());
    printf("Нажимайте Ctrl-C для звукового сигнала.\n");
    printf("Для завершения нажмите Ctrl-\\.\n");
    fflush(stdout);

    while (!quit_requested) {
        (void)sigsuspend(&wait_mask);
    }

    final_count = (int)signal_count;
    printf("\nЗвуковой сигнал прозвучал %d раз(а).\n", final_count);

    if (sigprocmask(SIG_SETMASK, &previous_mask, NULL) == -1) {
        perror("sigprocmask");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
