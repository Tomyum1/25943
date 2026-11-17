#define _POSIX_C_SOURCE 200809L

#include <aio.h>
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "socket_io.h"

#define MAX_CLIENTS 64

struct client {
    struct aiocb request;
    unsigned char buffer[4096];
};

static const char *socket_path;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t client_finished = PTHREAD_COND_INITIALIZER;
static struct client *clients[MAX_CLIENTS];
static sigset_t completions;
static int active;
static int failed;

static void stop_server(int signum)
{
    (void)unlink(socket_path);
    _exit(128 + signum);
}

/* Таблица клиентов изменяется только под lock. */
static void finish_client(int slot, int error)
{
    close(clients[slot]->request.aio_fildes);
    free(clients[slot]);
    clients[slot] = NULL;

    if (error) failed = 1;
    active--;
    pthread_cond_signal(&client_finished);
}

static void reap_read(int slot)
{
    struct client *client = clients[slot];
    int error = aio_error(&client->request);
    ssize_t count;

    if (error == EINPROGRESS) return;
    count = aio_return(&client->request);
    if (error != 0 || count < 0) {
        if (error > 0) errno = error;
        perror("aio read");
        finish_client(slot, 1);
        return;
    }
    if (count == 0) {
        finish_client(slot, 0);
        return;
    }

    for (ssize_t i = 0; i < count; i++) {
        client->buffer[i] = (unsigned char)toupper(client->buffer[i]);
    }
    /* Одна порция выводится целиком; порции разных клиентов чередуются. */
    if (write_all(STDOUT_FILENO, client->buffer, (size_t)count) == -1) {
        perror("write stdout");
        finish_client(slot, 1);
        return;
    }

    /* После aio_return можно снова использовать тот же запрос и буфер. */
    if (aio_read(&client->request) == -1) {
        perror("aio_read");
        finish_client(slot, 1);
    }
}

static void *collect_reads(void *unused)
{
    (void)unused;
    for (;;) {
        int signum;
        int error = sigwait(&completions, &signum);

        if (error != 0) {
            errno = error;
            perror("sigwait");
            stop_server(SIGTERM);
        }
        /* Сигналы могут объединяться: проверяем все активные запросы. */
        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i]) reap_read(i);
        }
        pthread_mutex_unlock(&lock);
    }
}

static int start_client(int fd)
{
    struct client *client = calloc(1, sizeof(*client));
    int slot = 0;

    if (!client) {
        perror("calloc");
        close(fd);
        return -1;
    }
    pthread_mutex_lock(&lock);
    if (active == MAX_CLIENTS) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr, "Слишком много клиентов.\n");
        free(client);
        close(fd);
        return -1;
    }
    while (clients[slot]) slot++;
    clients[slot] = client;
    active++;

    client->request.aio_fildes = fd;
    client->request.aio_buf = client->buffer;
    client->request.aio_nbytes = sizeof(client->buffer);
    /* У каждого клиента свой запрос. Ожидание его данных не блокирует accept. */
    client->request.aio_sigevent.sigev_notify = SIGEV_SIGNAL;
    client->request.aio_sigevent.sigev_signo = SIGUSR1;
    if (aio_read(&client->request) == -1) {
        perror("aio_read");
        finish_client(slot, 1);
        pthread_mutex_unlock(&lock);
        return -1;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

static int serve(int listener, int demo)
{
    int accepted = 0;
    pthread_t collector;
    int error;

    /* Блокируем SIGUSR1 до создания потока: принимать его будет sigwait(). */
    sigemptyset(&completions);
    sigaddset(&completions, SIGUSR1);
    error = pthread_sigmask(SIG_BLOCK, &completions, NULL);
    if (error == 0) error = pthread_create(&collector, NULL, collect_reads, NULL);
    if (error != 0) {
        errno = error;
        perror("start AIO collector");
        return EXIT_FAILURE;
    }
    (void)pthread_detach(collector);

    while (!demo || accepted < 2) {
        int fd = accept(listener, NULL, NULL);

        if (fd == -1) {
            if (errno == EINTR) continue;
            perror("accept");
            return EXIT_FAILURE;
        }
        if (start_client(fd) == -1 && demo) return EXIT_FAILURE;
        if (demo) accepted++;
    }

    /* Демо завершается после EOF от обоих клиентов, без таймера на сервере. */
    pthread_mutex_lock(&lock);
    while (active != 0) pthread_cond_wait(&client_finished, &lock);
    int status = failed ? EXIT_FAILURE : EXIT_SUCCESS;
    pthread_mutex_unlock(&lock);
    return status;
}

int main(int argc, char *argv[])
{
    struct sockaddr_un address;
    struct sigaction action = {0};
    int demo = argc > 1 && strcmp(argv[1], "--demo") == 0;
    int path_arg = demo ? 2 : 1;
    int listener;
    int status = EXIT_FAILURE;

    if (argc > path_arg + 1) {
        fprintf(stderr, "Использование: %s [--demo] [путь-к-сокету]\n", argv[0]);
        return EXIT_FAILURE;
    }
    socket_path = argc > path_arg ? argv[path_arg] : DEFAULT_SOCKET;
    if (make_address(&address, socket_path) == -1) {
        perror("socket path");
        return EXIT_FAILURE;
    }
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind");
        close(listener);
        return EXIT_FAILURE;
    }

    action.sa_handler = stop_server;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1 ||
        sigaction(SIGHUP, &action, NULL) == -1 ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal handler");
    } else if (listen(listener, MAX_CLIENTS) == -1) {
        perror("listen");
    } else {
        fprintf(stderr, "AIO-сервер ждёт клиентов: %s (Ctrl-C — выход).\n", socket_path);
        status = serve(listener, demo);
    }

    close(listener);
    if (unlink(socket_path) == -1) {
        perror("unlink socket");
        return EXIT_FAILURE;
    }
    return status;
}
