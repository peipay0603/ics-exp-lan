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
        if (c == '\n') break;
        if (c != '\r') line[i++] = c;
    }
    line[i] = '\0';
    return 0;
}

#endif
