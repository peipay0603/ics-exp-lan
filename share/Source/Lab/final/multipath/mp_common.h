#ifndef MP_COMMON_H
#define MP_COMMON_H

#include "../icslab2_net.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

#define MP_BUFFER 65536
#define MP_CONNECTIONS 128
#define MP_EVENTS 64
#define MP_LINE 128

static inline double mp_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static inline int mp_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline int mp_event(int epfd, int operation, int fd, uint32_t events,
                           int index)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.u32 = (uint32_t)index;
    return epoll_ctl(epfd, operation, fd, &ev);
}

static inline int mp_listen(int port)
{
    int fd, yes = 1;
    struct sockaddr_in addr;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
       listen(fd, MP_CONNECTIONS) < 0 || mp_nonblock(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static inline int mp_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res, *p;
    int fd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if(getaddrinfo(host, port, &hints, &res) != 0)
        return -1;
    for(p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if(fd < 0)
            continue;
        if(mp_nonblock(fd) == 0 &&
           (connect(fd, p->ai_addr, p->ai_addrlen) == 0 || errno == EINPROGRESS))
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static inline int mp_connected(int fd)
{
    int error = 0;
    socklen_t length = sizeof(error);
    if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
        return -1;
    if(error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static inline int mp_port(const char *text)
{
    char *end;
    long port;
    errno = 0;
    port = strtol(text, &end, 10);
    return errno || end == text || *end || port < 1 || port > 65535 ? -1 : (int)port;
}

#endif
