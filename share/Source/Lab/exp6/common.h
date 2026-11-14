/*  -*- coding: utf-8-unix; -*-                                     */
/*  FILENAME     :  common.h                                        */
/*  DESCRIPTION  :  実験6（ファイル転送の高速化）の共通定義         */
/*                                                                  */
/*  tcp_send / tcp_recv           : TCPだけで送る版（BUF_LENを実行時に指定）   */
/*  hyb_send / hyb_recv / udp_relay : データはUDP、制御はTCPで送る併用版      */

#ifndef EXP6_COMMON_H
#define EXP6_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <endian.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

/*-------------------------- BUF_LEN -------------------------------*/
/* 1回のread()/write()で扱う最大バイト数。
   実験4-3までは icslab2_net.h の #define BUF_LEN 1000 で固定だった。
   ここでは再コンパイルせずに比べられるよう、-b オプションで実行時に変える。 */
#define DEFAULT_BUF_LEN     (64 * 1024)
#define MAX_BUF_LEN         (64 * 1024 * 1024)

/*-------------------------- TCP版のヘッダ -------------------------*/
/* ファイル本体の前に1回だけ送る。値はすべてネットワークバイトオーダ。
   中継（relay）はバイト列をそのまま流すだけなので、このヘッダも素通りする。 */
#define TCP_MAGIC           0x45365443u     /* "E6TC" */

struct tcp_hdr {
    uint32_t magic;
    uint32_t buf_len;       /* 送信側のBUF_LEN（記録用）                 */
    uint64_t file_size;     /* 受信側が「全部届いたか」を確認するため   */
    int64_t  start_sec;     /* 送信側が計測を始めた時刻 CLOCK_REALTIME  */
    int64_t  start_nsec;
};
_Static_assert(sizeof(struct tcp_hdr) == 32, "tcp_hdr must be 32 bytes");

/*-------------------------- UDP+TCP併用版 -------------------------*/
/* データグラム = [session 4B][seq 4B][ファイルのseq番目のチャンク]
   1500(MTU) - 20(IP) - 8(UDP) - 8(独自ヘッダ) = 1464 なら IP断片化しない */
#define HYB_MAGIC           0x45364842u     /* "E6HB" */
#define HYB_DGRAM_HDR       8
#define HYB_DEFAULT_CHUNK   1464
#define HYB_MAX_CHUNK       (65507 - HYB_DGRAM_HDR)
#define HYB_MAX_RANGES      65536           /* 1回のNACKで送る未着範囲の上限 */
/* 回線上の1データグラムの大きさ = データ + 独自8 + UDP8 + IP20 + Ethernet14
   tcのtbfはEthernetヘッダ込みの長さで帯域を数えるので、送信レートもこれで計算する */
#define HYB_WIRE_OVERHEAD   (HYB_DGRAM_HDR + 8 + 20 + 14)

/* 送信側 → 受信側（TCP）: 最初に1回送る */
struct hyb_hello {
    uint32_t magic;
    uint32_t chunk;         /* 1データグラムに入れるファイルデータの長さ */
    uint64_t file_size;
    uint32_t total_chunks;  /* チャンク数 = ceil(file_size / chunk)     */
    uint32_t session;       /* 前回の転送の残りのデータグラムを捨てる用 */
    int64_t  start_sec;
    int64_t  start_nsec;
};
_Static_assert(sizeof(struct hyb_hello) == 40, "hyb_hello must be 40 bytes");

/* 制御メッセージ（TCP） */
enum {
    MSG_READY     = 1,      /* 受信側→送信側: UDPを受け取る準備ができた        */
    MSG_ROUND_END = 2,      /* 送信側→受信側: この回の送信を終えた count=送信数 */
    MSG_NACK      = 3,      /* 受信側→送信側: 未着範囲 count個（後ろに続く）  */
    MSG_DONE      = 4       /* 受信側→送信側: 全チャンクを受け取り保存した    */
};

struct hyb_msg {
    uint32_t type;
    uint32_t round;
    uint32_t count;
    uint32_t reserved;
};
_Static_assert(sizeof(struct hyb_msg) == 16, "hyb_msg must be 16 bytes");

struct hyb_range {          /* seq が start 〜 start+len-1 のチャンクが未着 */
    uint32_t start;
    uint32_t len;
};

/*-------------------------- 小さな関数 ----------------------------*/

/* b - a を秒で返す */
static inline double ts_diff(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec)
         + (double)(b->tv_nsec - a->tv_nsec) * 1e-9;
}

/* 時刻合わせの影響を受けない時計（送信間隔の制御に使う） */
static inline uint64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* このプロセスが使ったCPU時間（user + sys）[秒] */
static inline double cpu_seconds(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec * 1e-6
         + (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec * 1e-6;
}

/* len バイト全部を書き終えるまで write() を繰り返す。成功 0、失敗 -1 */
static inline int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while(len > 0) {
        ssize_t n = write(fd, p, len);
        if(n < 0) {
            if(errno == EINTR)
                continue;
            return -1;
        }
        if(n == 0) {
            errno = EIO;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ちょうど len バイト読む。成功 0、途中で相手が閉じた 1、エラー -1 */
static inline int read_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while(len > 0) {
        ssize_t n = read(fd, p, len);
        if(n < 0) {
            if(errno == EINTR)
                continue;
            return -1;
        }
        if(n == 0)
            return 1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* "1000" "64K" "1M" をバイト数にする。不正なら -1 */
static inline long parse_size(const char *s)
{
    char *end;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if(errno != 0 || end == s || v <= 0)
        return -1;
    if(*end == 'k' || *end == 'K') {
        v *= 1024;
        end++;
    } else if(*end == 'm' || *end == 'M') {
        v *= 1024 * 1024;
        end++;
    }
    if(*end != '\0')
        return -1;
    return v;
}

/* ポート番号。不正なら -1 */
static inline int parse_port(const char *s)
{
    char *end;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if(errno != 0 || end == s || *end != '\0' || v < 1 || v > 65535)
        return -1;
    return (int)v;
}

/* ホスト名またはIPアドレスとポートから宛先アドレスを作る。成功 0 */
static inline int make_addr(const char *host, int port, struct sockaddr_in *addr)
{
    struct addrinfo hints, *res;
    int err;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    err = getaddrinfo(host, NULL, &hints, &res);
    if(err != 0) {
        fprintf(stderr, "getaddrinfo(%s): %s\n", host, gai_strerror(err));
        return -1;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((unsigned short)port);
    addr->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

/* UDPの受信バッファをできるだけ大きくし、実際の大きさを返す。
   一般ユーザーでは net.core.rmem_max（既定 約208KB）の2倍までしか増えない。
   rootで実行した場合は SO_RCVBUFFORCE で上限を超えられる。 */
static inline int grow_rcvbuf(int sock, int want)
{
    int got = 0;
    socklen_t len = sizeof(got);
    if(setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, &want, sizeof(want)) < 0)
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
    getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &got, &len);
    return got;
}

/* 制御メッセージを送る / 受け取る（TCP） */
static inline int send_msg(int sock, uint32_t type, uint32_t round, uint32_t count)
{
    struct hyb_msg m;
    m.type = htonl(type);
    m.round = htonl(round);
    m.count = htonl(count);
    m.reserved = 0;
    return write_all(sock, &m, sizeof(m));
}

/* 成功 0、相手が閉じた 1、エラー -1 */
static inline int recv_msg(int sock, struct hyb_msg *m)
{
    int r = read_all(sock, m, sizeof(*m));
    if(r != 0)
        return r;
    m->type = ntohl(m->type);
    m->round = ntohl(m->round);
    m->count = ntohl(m->count);
    return 0;
}

#endif
