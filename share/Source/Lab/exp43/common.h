#ifndef EXP43_COMMON_H
#define EXP43_COMMON_H

#include "../icslab2_net.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_CHUNK (1024U * 1024U)

static int parse_number(const char *text, uint64_t min, uint64_t max,
                        uint64_t *value)
{
    char *end;
    unsigned long long number;
    if(text[0] < '0' || text[0] > '9')
        return -1;
    errno = 0;
    number = strtoull(text, &end, 10);
    if(errno || *end != '\0' || number < min || number > max)
        return -1;
    *value = (uint64_t)number;
    return 0;
}

static ssize_t read_retry(int fd, void *buffer, size_t length)
{
    ssize_t count;
    do {
        count = read(fd, buffer, length);
    } while(count < 0 && errno == EINTR);
    return count;
}

/* 部分書き込みを処理する。callsは成功したwrite()の呼び出し回数。 */
static int write_all(int fd, const void *buffer, size_t length, uint64_t *calls)
{
    const char *next = buffer;
    while(length > 0) {
        ssize_t count = write(fd, next, length);
        if(count < 0) {
            if(errno == EINTR)
                continue;
            return -1;
        }
        if(count == 0) {
            errno = EIO;
            return -1;
        }
        if(calls != NULL)
            ++*calls;
        next += count;
        length -= (size_t)count;
    }
    return 0;
}

/* 制御メッセージだけを改行まで読む。ファイル本体の回数には含めない。 */
static int read_line(int fd, char *line, size_t capacity)
{
    size_t length = 0;
    while(length + 1 < capacity) {
        char ch;
        ssize_t count = read_retry(fd, &ch, 1);
        if(count <= 0) {
            if(count == 0)
                errno = ECONNRESET;
            return -1;
        }
        if(ch == '\n') {
            line[length] = '\0';
            return 0;
        }
        line[length++] = ch;
    }
    errno = EMSGSIZE;
    return -1;
}

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec)
         + (double)(end.tv_nsec - start.tv_nsec) * 1e-9;
}

#endif
