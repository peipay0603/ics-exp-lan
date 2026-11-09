/* 実験4-3: TCPファイル送信。4-1のopen/read/writeの流れに計測を追加。 */
#define _POSIX_C_SOURCE 200809L
#include "common.h"

static int transfer_file(int sock, const char *filename, char *buf, size_t chunk)
{
    char request[128], header[128];
    struct stat info;
    struct timespec start, end;
    uint64_t bytes = 0, file_reads = 0, send_calls = 0;
    int fd = -1;
    int result = -1;

    /* 要求内容は解析せず、起動時に指定されたファイルを送る。 */
    if(read_line(sock, request, sizeof(request)) < 0) {
        perror("read request");
        return -1;
    }
    fd = open(filename, O_RDONLY);
    if(fd < 0) {
        perror("open input");
        write_all(sock, "ERR\n", 4, NULL);
        return -1;
    }
    if(fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        fprintf(stderr, "input must be a regular file\n");
        write_all(sock, "ERR\n", 4, NULL);
        goto done;
    }
    if(clock_gettime(CLOCK_REALTIME, &start) < 0) {
        perror("clock_gettime");
        goto done;
    }
    /* サイズ通知はファイル本体とは分け、途中切断を検出できるようにする。 */
    snprintf(header, sizeof(header), "OK %" PRIu64 "\n", (uint64_t)info.st_size);
    if(write_all(sock, header, strlen(header), NULL) < 0) {
        perror("write header");
        goto done;
    }
    while(bytes < (uint64_t)info.st_size) {
        uint64_t remaining = (uint64_t)info.st_size - bytes;
        size_t length = remaining < chunk ? (size_t)remaining : chunk;
        ssize_t n = read_retry(fd, buf, length);
        if(n <= 0) {
            if(n < 0)
                perror("read input");
            else
                fprintf(stderr, "input ended before its announced size\n");
            goto done;
        }
        ++file_reads;
        if(write_all(sock, buf, (size_t)n, &send_calls) < 0) {
            perror("write socket");
            goto done;
        }
        bytes += (uint64_t)n;
    }
    if(clock_gettime(CLOCK_REALTIME, &end) < 0) {
        perror("clock_gettime");
        goto done;
    }
    printf("RESULT status=ok payload_bytes=%" PRIu64
           " chunk_bytes=%zu file_reads=%" PRIu64 " send_calls=%" PRIu64
           " server_seconds=%.9f\n",
           bytes, chunk, file_reads, send_calls, elapsed_seconds(start, end));
    fflush(stdout);
    result = 0;
done:
    close(fd);
    return result;
}

int main(int argc, char **argv)
{
    uint64_t port = TCP_SERVER_PORT, chunk = BUF_LEN, max_clients = 0;
    uint64_t handled = 0;
    struct sockaddr_in serverAddr;
    socklen_t address_length = sizeof(serverAddr);
    int sock0, yes = 1, failed = 0;
    char *buf;

    if(argc < 2 || argc > 5 || strcmp(argv[1], "-h") == 0) {
        fprintf(stderr, "Usage: %s input_file [port=10000] [chunk_bytes=%d] [max_clients=0]\n",
                argv[0], BUF_LEN);
        return argc == 2 && strcmp(argv[1], "-h") == 0 ? 0 : 1;
    }
    if((argc > 2 && parse_number(argv[2], 0, 65535, &port) < 0)
       || (argc > 3 && parse_number(argv[3], 1, MAX_CHUNK, &chunk) < 0)
       || (argc > 4 && parse_number(argv[4], 0, UINT64_MAX, &max_clients) < 0)) {
        fprintf(stderr, "invalid port, chunk_bytes, or max_clients\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    buf = malloc((size_t)chunk);
    if(buf == NULL) {
        perror("malloc");
        return 1;
    }
    sock0 = socket(AF_INET, SOCK_STREAM, 0);
    if(sock0 < 0) {
        perror("socket");
        free(buf);
        return 1;
    }
    setsockopt(sock0, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons((unsigned short)port);
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(sock0, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0
       || listen(sock0, 5) < 0
       || getsockname(sock0, (struct sockaddr *)&serverAddr, &address_length) < 0) {
        perror("bind/listen/getsockname");
        close(sock0);
        free(buf);
        return 1;
    }
    printf("LISTEN port=%u chunk_bytes=%" PRIu64 "\n", ntohs(serverAddr.sin_port), chunk);
    fflush(stdout);

    while(max_clients == 0 || handled < max_clients) {
        int sock = accept(sock0, NULL, NULL);
        if(sock < 0) {
            if(errno == EINTR)
                continue;
            perror("accept");
            failed = 1;
            break;
        }
        if(transfer_file(sock, argv[1], buf, (size_t)chunk) < 0)
            failed = 1;
        close(sock);
        ++handled;
    }
    close(sock0);
    free(buf);
    return failed;
}
