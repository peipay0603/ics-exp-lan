/* 実験4-3: ファイル受信時間・グッドプット・受信回数の計測。 */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

static int connect_server(const char *host, const char *port)
{
    struct addrinfo hints, *res, *entry;
    int sock = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int error = getaddrinfo(host, port, &hints, &res);
    if(error != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
        return -1;
    }
    for(entry = res; entry != NULL; entry = entry->ai_next) {
        sock = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if(sock < 0)
            continue;
        if(connect(sock, entry->ai_addr, entry->ai_addrlen) == 0)
            break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if(sock < 0)
        perror("connect");
    return sock;
}

static int wait_before_receive(uint64_t microseconds)
{
    struct timespec wait = {
        (time_t)(microseconds / 1000000),
        (long)((microseconds % 1000000) * 1000)
    };
    if(microseconds == 0)
        return 0;
    while(nanosleep(&wait, &wait) < 0) {
        if(errno != EINTR)
            return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t port, wait_us = 0, receive_buffer = BUF_LEN;
    uint64_t expected, bytes = 0, receive_calls = 0;
    struct timespec start, end;
    char header[128];
    char *buf;
    int sock = -1, fd = -1, result = 1;

    if(argc < 4 || argc > 6) {
        fprintf(stderr, "Usage: %s host port output_file [wait_us=0] [receive_buffer=%d]\n",
                argv[0], BUF_LEN);
        return argc == 2 && strcmp(argv[1], "-h") == 0 ? 0 : 1;
    }
    if(parse_number(argv[2], 1, 65535, &port) < 0
       || (argc > 4 && parse_number(argv[4], 0, 60000000, &wait_us) < 0)
       || (argc > 5 && parse_number(argv[5], 1, MAX_CHUNK, &receive_buffer) < 0)) {
        fprintf(stderr, "invalid port, wait_us, or receive_buffer\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    buf = malloc((size_t)receive_buffer);
    if(buf == NULL) {
        perror("malloc");
        return 1;
    }
    sock = connect_server(argv[1], argv[2]);
    if(sock < 0)
        goto done;

    /* A: 接続確立後、要求送信直前から全データの保存・close完了まで。 */
    if(clock_gettime(CLOCK_REALTIME, &start) < 0) {
        perror("clock_gettime");
        goto done;
    }
    if(write_all(sock, "GET\n", 4, NULL) < 0
       || read_line(sock, header, sizeof(header)) < 0) {
        perror("request/header");
        goto done;
    }
    if(strncmp(header, "OK ", 3) != 0
       || parse_number(header + 3, 0, UINT64_MAX, &expected) < 0) {
        fprintf(stderr, "server rejected the request or returned an invalid header: %s\n", header);
        goto done;
    }
    fd = open(argv[3], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if(fd < 0) {
        perror("open output");
        goto done;
    }

    /* D: 各read()の前に待つことで、ソケットからの読み出しを遅くする。 */
    while(bytes < expected) {
        uint64_t remaining = expected - bytes;
        size_t length = remaining < receive_buffer ? (size_t)remaining : (size_t)receive_buffer;
        if(wait_before_receive(wait_us) < 0) {
            perror("nanosleep");
            goto done;
        }
        ssize_t n = read_retry(sock, buf, length);
        if(n <= 0) {
            if(n < 0)
                perror("read socket");
            else
                fprintf(stderr, "incomplete transfer: received %" PRIu64 " of %" PRIu64 " bytes\n",
                        bytes, expected);
            goto done;
        }
        ++receive_calls;
        if(write_all(fd, buf, (size_t)n, NULL) < 0) {
            perror("write output");
            goto done;
        }
        bytes += (uint64_t)n;
    }
    int closed = close(fd);
    fd = -1;
    if(closed < 0) {
        perror("close output");
        goto done;
    }
    if(clock_gettime(CLOCK_REALTIME, &end) < 0) {
        perror("clock_gettime");
        goto done;
    }
    double seconds = elapsed_seconds(start, end);
    if(seconds <= 0) {
        fprintf(stderr, "elapsed time must be positive\n");
        goto done;
    }
    /* B: ファイル本体の総バイト数だけをbitへ換算する。 */
    double goodput = (double)bytes * 8.0 / seconds;
    printf("RESULT status=ok payload_bytes=%" PRIu64 " receive_calls=%" PRIu64
           " wait_us=%" PRIu64 " receive_buffer=%" PRIu64
           " seconds=%.9f goodput_bps=%.3f goodput_mbps=%.6f\n",
           bytes, receive_calls, wait_us, receive_buffer, seconds, goodput, goodput / 1e6);
    result = 0;
done:
    if(fd >= 0)
        close(fd);
    if(sock >= 0)
        close(sock);
    free(buf);
    return result;
}
