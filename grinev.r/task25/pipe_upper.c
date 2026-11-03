#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define INPUT_LIMIT 80

static struct termios saved_terminal;
static int terminal_changed;
static volatile sig_atomic_t stopped;

static void restore_terminal(void)
{
    if (terminal_changed) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_terminal);
        terminal_changed = 0;
    }
}

static void stop_handler(int number)
{
    stopped = number;
}

static int read_key(unsigned char *key, int timeout)
{
    struct pollfd descriptor;
    int result;
    ssize_t count;

    descriptor.fd = STDIN_FILENO;
    descriptor.events = POLLIN;
    while (!stopped) {
        descriptor.revents = 0;
        result = poll(&descriptor, 1, timeout < 0 ? 100 : timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) {
            if (timeout >= 0) return 0;
            continue;
        }
        count = read(STDIN_FILENO, key, 1);
        if (count >= 0) return (int)count;
        if (errno != EINTR) return -1;
    }
    return -1;
}

static void skip_escape(void)
{
    unsigned char key;
    int previous_escape = 0;

    if (read_key(&key, 100) != 1) return;
    if (key == '[' || key == 'O') {
        while (read_key(&key, 100) == 1) {
            if (key >= 0x40 && key <= 0x7e) return;
        }
    } else if (key == ']' || key == 'P' || key == '^' ||
               key == '_' || key == 'X') {
        while (read_key(&key, 100) == 1) {
            if (key == 7 || (previous_escape && key == '\\')) return;
            previous_escape = key == 27;
        }
    }
}

static void redraw(const char *line, size_t length)
{
    fprintf(stderr, "\r\033[2K%.*s", (int)length, line);
    fflush(stderr);
}

static int write_all(int fd, const char *buffer, size_t length)
{
    size_t offset = 0;
    ssize_t count;

    while (offset < length) {
        count = write(fd, buffer + offset, length - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) { errno = EIO; return -1; }
        offset += (size_t)count;
    }
    return 0;
}

static int terminal_input(int destination)
{
    struct termios settings;
    struct sigaction action;
    char line[INPUT_LIMIT + 1];
    size_t length = 0;
    unsigned char key;
    int result;
    int status = EXIT_SUCCESS;

    if (tcgetattr(STDIN_FILENO, &saved_terminal) == -1) {
        perror("tcgetattr");
        return EXIT_FAILURE;
    }
    if (atexit(restore_terminal) != 0) return EXIT_FAILURE;
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGQUIT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1 ||
        sigaction(SIGHUP, &action, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    settings = saved_terminal;
    settings.c_lflag &= (tcflag_t)~(ICANON | ECHO | ECHONL | ISIG | IEXTEN);
    settings.c_iflag &= (tcflag_t)~(IXON | IXOFF | ISTRIP | INLCR | IGNCR);
    settings.c_iflag |= ICRNL;
    settings.c_cc[VMIN] = 1;
    settings.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &settings) == -1) {
        perror("tcsetattr");
        return EXIT_FAILURE;
    }
    terminal_changed = 1;
    fprintf(stderr, "Введите английский текст (до 80 байт). Ctrl-D — выход.\n");
    for (;;) {
        result = read_key(&key, -1);
        if (result <= 0) {
            if (result < 0 && !stopped) { perror("read/poll"); status = EXIT_FAILURE; }
            break;
        }
        if (key == 3) { stopped = SIGINT; break; }
        if (key == 4) {
            if (length == 0) break;
            fputc('\a', stderr);
        } else if (key == 27) {
            skip_escape();
        } else if (key == 8 || key == 127 ||
                   (saved_terminal.c_cc[VERASE] != _POSIX_VDISABLE &&
                    key == saved_terminal.c_cc[VERASE])) {
            if (length > 0) length--;
            redraw(line, length);
        } else if (key == 21 ||
                   (saved_terminal.c_cc[VKILL] != _POSIX_VDISABLE &&
                    key == saved_terminal.c_cc[VKILL])) {
            length = 0;
            redraw(line, length);
        } else if (key == '\n' || key == '\r') {
            fputc('\n', stderr);
            line[length++] = '\n';
            if (write_all(destination, line, length) == -1) {
                perror("write pipe");
                status = EXIT_FAILURE;
                break;
            }
            length = 0;
        } else if (key >= 32 && key <= 126) {
            if (length < INPUT_LIMIT) {
                line[length++] = (char)key;
                redraw(line, length);
            } else fputc('\a', stderr);
        }
        fflush(stderr);
    }
    fputc('\n', stderr);
    restore_terminal();
    return stopped ? 128 + (int)stopped : status;
}

static int transfer(int source, int destination, int uppercase)
{
    char buffer[4096];
    ssize_t count;
    ssize_t i;

    for (;;) {
        count = read(source, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) continue;
            perror("read");
            return EXIT_FAILURE;
        }
        if (count == 0) return EXIT_SUCCESS;
        if (uppercase) {
            for (i = 0; i < count; i++) {
                buffer[i] = (char)toupper((unsigned char)buffer[i]);
            }
        }
        if (write_all(destination, buffer, (size_t)count) == -1) {
            perror("write");
            return EXIT_FAILURE;
        }
    }
}

int main(void)
{
    int channel[2];
    pid_t child;
    pid_t result;
    int child_status;
    int status;
    struct sigaction action;

    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGPIPE, &action, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    if (pipe(channel) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }
    child = fork();
    if (child == -1) {
        perror("fork");
        close(channel[0]);
        close(channel[1]);
        return EXIT_FAILURE;
    }
    if (child == 0) {
        close(channel[1]);
        status = transfer(channel[0], STDOUT_FILENO, 1);
        close(channel[0]);
        _exit(status);
    }

    close(channel[0]);
    status = isatty(STDIN_FILENO)
                 ? terminal_input(channel[1])
                 : transfer(STDIN_FILENO, channel[1], 0);
    /* Closing the last writer lets the child receive EOF from read(). */
    close(channel[1]);

    do {
        result = waitpid(child, &child_status, 0);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        perror("waitpid");
        return EXIT_FAILURE;
    }
    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        return EXIT_FAILURE;
    }
    return status;
}
