#ifndef SOCKET_IO_H
#define SOCKET_IO_H

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_SOCKET "./task31.sock"

/* Сервер и клиент одинаково заполняют адрес локального сокета. */
static inline int make_address(struct sockaddr_un *address, const char *path)
{
    size_t length = strlen(path);

    /* В sun_path должно хватить места для пути и завершающего нуля. */
    if (length == 0 || length >= sizeof(address->sun_path)) {
        errno = length == 0 ? EINVAL : ENAMETOOLONG;
        return -1;
    }
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path, path, length + 1);
    return 0;
}

static inline int write_all(int fd, const unsigned char *data, size_t length)
{
    /* write может записать только часть буфера; отправляем остаток в цикле. */
    while (length > 0) {
        ssize_t written = write(fd, data, length);

        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        /* Сдвигаем указатель на первый байт, который ещё не записан. */
        data += written;
        length -= (size_t)written;
    }
    return 0;
}

#endif
