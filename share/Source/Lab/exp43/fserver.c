#include "icslab2_net.h"

int main(int argc, char **argv)
{
    int sock0, sock, fd, n;
    int yes = 1;
    int port = TCP_SERVER_PORT;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t addrLen;
    char buf[BUF_LEN];
    char *filename;

    if(argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s filename [port]\n", argv[0]);
        return 1;
    }
    filename = argv[1];
    if(argc > 2)
        port = atoi(argv[2]);
    if(port < 1 || port > 65535) {
        fprintf(stderr, "invalid port\n");
        return 1;
    }

    /* STEP 1: ソケットを作る */
    sock0 = socket(AF_INET, SOCK_STREAM, 0);
    if(sock0 < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(sock0, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* STEP 2: アドレスとポートを設定する */
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* STEP 3: bindする */
    if(bind(sock0, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("bind");
        close(sock0);
        return 1;
    }

    /* STEP 4: 接続を待つ */
    if(listen(sock0, 5) < 0) {
        perror("listen");
        close(sock0);
        return 1;
    }

    while(1) {
        unsigned long long total_bytes = 0;
        unsigned long send_count = 0;
        unsigned long file_read_count = 0;

        /* STEP 5: 接続を受け付ける */
        printf("waiting connection... port=%d BUF_LEN=%d\n", port, BUF_LEN);
        fflush(stdout);
        addrLen = sizeof(clientAddr);
        sock = accept(sock0, (struct sockaddr *)&clientAddr, &addrLen);
        if(sock < 0) {
            perror("accept");
            close(sock0);
            return 1;
        }

        /* STEP 6: ファイル要求を受信する */
        n = read(sock, buf, BUF_LEN);
        if(n <= 0) {
            if(n < 0)
                perror("read request");
            close(sock);
            continue;
        }

        /* STEP 7: 送信するファイルを開く */
        fd = open(filename, O_RDONLY);
        if(fd < 0) {
            perror("open");
            close(sock);
            continue;
        }

        /* STEP 8: ファイルを読み、ソケットへ送る */
        while((n = read(fd, buf, BUF_LEN)) > 0) {
            int sent = 0;
            file_read_count++;
            while(sent < n) {
                ssize_t count = write(sock, buf + sent, n - sent);
                if(count <= 0) {
                    if(count < 0)
                        perror("write socket");
                    close(fd);
                    close(sock);
                    close(sock0);
                    return 1;
                }
                sent += (int)count;
                send_count++;
            }
            total_bytes += (unsigned long long)n;
        }
        if(n < 0) {
            perror("read file");
            close(fd);
            close(sock);
            close(sock0);
            return 1;
        }

        /* STEP 9: ファイルと通信用ソケットを閉じる */
        close(fd);
        close(sock);
        printf("Send bytes: %llu\n", total_bytes);
        printf("File read count: %lu\n", file_read_count);
        printf("Send count: %lu\n", send_count);
        fflush(stdout);
    }
}
