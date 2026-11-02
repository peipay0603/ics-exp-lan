/*  -*- coding: utf-8-unix; -*-                                     */
/*  FILENAME     :  tcp_echo_client.c                               */
/*  DESCRIPTION  :  TCP Echo Client                                 */
/*  USAGE        :  tcp_echo_client.out [dst_ip_addr] [port]        */
/*  DATE         :  Sep 01, 2020                                    */
/*                                                                  */

#include "icslab2_net.h"

int
main(int argc, char** argv)
{
    char *server_ipaddr_str = "127.0.0.1";      /* サーバIPアドレス（文字列） */
    unsigned int port = TCP_SERVER_PORT;        /* ポート番号 */
    
    int     sock;                   /* ソケットディスクリプタ */
    struct sockaddr_in serverAddr;  /* サーバ＝相手用のアドレス構造体 */
    char    buf[BUF_LEN];           /* 受信バッファ */
    int     n;                      /* 読み込み／受信バイト数 */

    struct in_addr addr;            /* アドレス表示用 */
    struct addrinfo hints, *res;
    int err;

    char *filename;
    int fd;

    /* コマンドライン引数の処理 */
    if(argc != 4) {
        fprintf(stderr, "Usage: %s host port output_file\n", argv[0]);
        return 1;
    }

    server_ipaddr_str = argv[1];
    port = (unsigned int)atoi(argv[2]);
    filename = argv[3];
    
    /* STEP 1: 宛先サーバのIPアドレスとポートを指定する */
    memset(&serverAddr, 0, sizeof(serverAddr));     /* 0クリア */
    serverAddr.sin_family = AF_INET;                /* Internetプロトコル */
    serverAddr.sin_port = htons(port);              /* サーバの待受ポート */
    /* IPアドレス（文字列）から変換 */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    err = getaddrinfo(server_ipaddr_str, NULL, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return 1;
    }

    serverAddr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    /* 確認用：IPアドレスを文字列に変換して表示 */
    addr.s_addr = serverAddr.sin_addr.s_addr;
    printf("ip address: %s\n", inet_ntoa(addr));
    printf("port#: %d\n", ntohs(serverAddr.sin_port));

    /* STEP 2x: TCPソケットをオープンする */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) {
        perror("socket");
        return  1;
    }

    /* ここで、ローカルでsocketをbind()してもよいが省略する */
    
    /* STEP 3x: サーバに接続する（bind相当も実行） */
    if(connect(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("connect");
        return  1;
    }

    /* 保存先ファイルを開く */
    fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if(fd < 0) {
        perror("open");
        close(sock);
        return 1;
    }

    /* ファイル要求を送信する */
    char request[] = "GET\r\n";
    int requestLen = (int)strlen(request);
    int sent = 0;

    while(sent < requestLen) {
        ssize_t count = write(sock, request + sent, requestLen - sent);
        if(count <= 0) {
            if(count < 0)
                perror("write request");
            else
                fprintf(stderr, "write request returned 0\n");

            close(fd);
            close(sock);
            return 1;
        }

        sent += (int)count;
    }

    /* サーバーから受信するたびにファイルへ保存する */
    while((n = read(sock, buf, BUF_LEN)) > 0) {
        int saved = 0;

        while(saved < n) {
            ssize_t count = write(fd, buf + saved, n - saved);
            if(count <= 0) {
                if(count < 0)
                    perror("write file");
                else
                    fprintf(stderr, "write file returned 0\n");

                close(fd);
                close(sock);
                return 1;
            }

            saved += (int)count;
        }
    }

    if(n < 0) {
        perror("read socket");
        close(fd);
        close(sock);
        return 1;
    }

    close(fd);

    /* STEP 6: ソケットのクローズ */
    close(sock);
    printf("closed\n");

    return 0;
}


/* Local Variables: */
/* compile-command: "gcc tcp_echo_client.c -o tcp_echo_client.out " */
/* End: */
