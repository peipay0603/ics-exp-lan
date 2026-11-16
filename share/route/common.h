/*
 * common.h -- 最終課題（高速ファイル転送）共通ヘッダ
 */
#ifndef FINAL_COMMON_H
#define FINAL_COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <sys/time.h>

#define SERVER_PORT     10000       /* ファイルサーバの待ち受けポート */
#define RELAY_PORT_BASE 20000       /* 経路 k の中継は 20000+k で待ち受ける */
#define REQ_MAX         128         /* 要求1行の最大長 */

/* 経過時間 [sec] */
static inline double elapsed_sec(const struct timespec *s, const struct timespec *e)
{
    return (double)(e->tv_sec - s->tv_sec) + (double)(e->tv_nsec - s->tv_nsec) * 1e-9;
}

/* n バイトを最後まで書く．成功 0，失敗 -1 */
static inline int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t m = write(fd, p, n);
        if (m < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (m == 0) { errno = EIO; return -1; }
        p += m;
        n -= (size_t)m;
    }
    return 0;
}

/* ちょうど n バイト読む．成功 0，EOF/失敗 -1 */
static inline int read_full(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n > 0) {
        ssize_t m = read(fd, p, n);
        if (m < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (m == 0) return -1;
        p += m;
        n -= (size_t)m;
    }
    return 0;
}

/* '\n' まで 1 行読む（'\r','\n' は取り除く）．成功 0，EOF/失敗 -1 */
static inline int read_line(int fd, char *line, size_t max)
{
    size_t i = 0;
    while (i + 1 < max) {
        char c;
        ssize_t m = read(fd, &c, 1);
        if (m < 0 && errno == EINTR) continue;
        if (m <= 0) return -1;
        if (c == '\n') { line[i] = '\0'; return 0; }
        if (c != '\r') line[i++] = c;
    }
    line[i] = '\0';
    errno = EMSGSIZE;
    return -1;
}

/* connect の待ち時間も有限にする（SO_RCVTIMEO だけでは connect を制限できない） */
static inline int connect_timeout(int fd, const struct sockaddr_in *addr, int seconds)
{
    int flags = fcntl(fd, F_GETFL, 0), error = 0;
    socklen_t length = sizeof(error);
    struct pollfd pfd = { fd, POLLOUT, 0 };
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) < 0) {
        if (errno != EINPROGRESS) return -1;
        int n;
        do { n = poll(&pfd, 1, seconds * 1000); } while (n < 0 && errno == EINTR);
        if (n <= 0) { if (n == 0) errno = ETIMEDOUT; return -1; }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) return -1;
        if (error) { errno = error; return -1; }
    }
    return fcntl(fd, F_SETFL, flags);
}

static inline int valid_port(const char *text)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    return errno || end == text || *end || value < 1 || value > 65535 ? -1 : (int)value;
}

#endif
