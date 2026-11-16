#include "icslab2_net.h"

int main(int argc, char **argv)
{
    int sock0, client, server, n, yes = 1;
    struct sockaddr_in localAddr, serverAddr;
    char buf[BUF_LEN];
    if(argc != 4) {
        fprintf(stderr, "Usage: %s listen_port server_ip server_port\n", argv[0]);
        return 1;
    }
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(atoi(argv[3]));
    if(inet_pton(AF_INET, argv[2], &serverAddr.sin_addr) != 1)
        return 1;

    /* STEP 1: クライアントからの接続を待つ */
    sock0 = socket(AF_INET, SOCK_STREAM, 0);
    if(sock0 < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(sock0, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(atoi(argv[1]));
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(sock0, (struct sockaddr *)&localAddr, sizeof(localAddr)) < 0 || listen(sock0, 5) < 0) {
        perror("bind/listen");
        close(sock0);
        return 1;
    }

    while(1) {
        client = accept(sock0, NULL, NULL);
        if(client < 0) {
            perror("accept");
            break;
        }
        /* STEP 2: ファイル要求を上流のサーバーへ送る */
        n = read(client, buf, BUF_LEN);
        if(n <= 0) {
            close(client);
            continue;
        }
        server = socket(AF_INET, SOCK_STREAM, 0);
        if(server < 0) {
            close(client);
            continue;
        }
        if(connect(server, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0
           || write(server, buf, n) != n) {
            perror("connect/write request");
            close(server);
            close(client);
            continue;
        }

        /* STEP 3: 受信したファイルをクライアントへ転送する */
        while((n = read(server, buf, BUF_LEN)) > 0) {
            int sent = 0;
            while(sent < n) {
                ssize_t count = write(client, buf + sent, n - sent);
                if(count <= 0) {
                    perror("write client");
                    close(server);
                    close(client);
                    close(sock0);
                    return 1;
                }
                sent += (int)count;
            }
        }
        if(n < 0)
            perror("read server");
        close(server);
        close(client);
    }
    close(sock0);
    return 1;
}
