#include "icslab2_net.h"
#include <time.h>

int main(int argc, char **argv)
{
    int sock, fd, n, err;
    int port, wait_us = 0;
    struct sockaddr_in serverAddr;
    struct addrinfo hints, *res;
    struct timespec start_time, end_time;
    char buf[BUF_LEN];
    char request[] = "GET\r\n";
    char *server_name, *filename;
    unsigned long long total_bytes = 0;
    unsigned long receive_count = 0;
    double elapsed, goodput;

    if(argc < 4 || argc > 5) {
        fprintf(stderr, "Usage: %s host port output_file [wait_us]\n", argv[0]);
        return 1;
    }
    server_name = argv[1];
    port = atoi(argv[2]);
    filename = argv[3];
    if(argc > 4)
        wait_us = atoi(argv[4]);
    if(port < 1 || port > 65535 || wait_us < 0 || wait_us > 999999) {
        fprintf(stderr, "invalid port or wait_us\n");
        return 1;
    }

    /* STEP 1: 宛先を設定する */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    err = getaddrinfo(server_name, NULL, &hints, &res);
    if(err != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return 1;
    }
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    /* STEP 2: ソケットを作る */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) {
        perror("socket");
        return 1;
    }

    /* STEP 3: サーバーに接続する */
    if(connect(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }
    fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if(fd < 0) {
        perror("open");
        close(sock);
        return 1;
    }

    /* STEP 4: 計測を開始し、ファイルを要求する */
    clock_gettime(CLOCK_REALTIME, &start_time);
    if(write(sock, request, strlen(request)) != (ssize_t)strlen(request)) {
        perror("write request");
        close(fd);
        close(sock);
        return 1;
    }

    /* STEP 5: 受信したデータをファイルに保存する */
    while((n = read(sock, buf, BUF_LEN)) > 0) {
        int saved = 0;
        receive_count++;
        if(wait_us > 0)
            usleep(wait_us);
        while(saved < n) {
            ssize_t count = write(fd, buf + saved, n - saved);
            if(count <= 0) {
                if(count < 0)
                    perror("write file");
                close(fd);
                close(sock);
                return 1;
            }
            saved += (int)count;
        }
        total_bytes += (unsigned long long)n;
    }
    if(n < 0) {
        perror("read socket");
        close(fd);
        close(sock);
        return 1;
    }

    /* STEP 6: 保存を終え、時間とグッドプットを計算する */
    if(close(fd) < 0) {
        perror("close file");
        close(sock);
        return 1;
    }
    clock_gettime(CLOCK_REALTIME, &end_time);
    close(sock);
    elapsed = (double)(end_time.tv_sec - start_time.tv_sec)
            + (double)(end_time.tv_nsec - start_time.tv_nsec) * 1e-9;
    if(elapsed <= 0) {
        fprintf(stderr, "elapsed time must be positive\n");
        return 1;
    }
    goodput = (double)total_bytes * 8.0 / elapsed;
    printf("Receive BUF_LEN: %d\n", BUF_LEN);
    printf("Receive bytes: %llu\n", total_bytes);
    printf("Receive count: %lu\n", receive_count);
    printf("Time: %.9f sec\n", elapsed);
    printf("Goodput: %.3f bps (%.6f Mbps)\n", goodput, goodput / 1e6);
    return 0;
}
