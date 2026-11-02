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

    /* コマンドライン引数の処理 */
    if(argc == 2 && strncmp(argv[1], "-h", 2) == 0) {
        printf("Usage: %s [dst_ip_addr] [port]\n", argv[0]);
        return 0;
    }
    if(argc > 1)    /* 宛先を指定のIPアドレスにする。 portはデフォルト */
        server_ipaddr_str = argv[1];
    if(argc > 2)    /* 宛先を指定のIPアドレス、portにする */
        port = (unsigned int)atoi(argv[2]);

    /* STEP 1: 宛先サーバのIPアドレスとポートを指定 */
    memset(&serverAddr, 0, sizeof(serverAddr));     /* 0クリア */
    serverAddr.sin_family = AF_INET;                /* Internetプロトコル */
    serverAddr.sin_port = htons(port);              /* サーバの待受ポート */
    /* IPアドレス（文字列）から変換 */
    inet_pton(AF_INET, server_ipaddr_str, &serverAddr.sin_addr.s_addr);

    /* 確認用：IPアドレスを文字列に変換して表示 */
    addr.s_addr = serverAddr.sin_addr.s_addr;
    printf("ip address: %s\n", inet_ntoa(addr));
    printf("port#: %d\n", ntohs(serverAddr.sin_port));

    /* STEP 2: TCPソケットをオープン */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) {
        perror("socket");
        return  1;
    }

    /* ここで、ローカルでsocketをbind()してもよいが省略 */
    
    /* STEP 3: サーバに接続（bind相当も実行） */
    if(connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("connetc");
        return  1;
    }

    while((n = read(0, buf, BUF_LEN)) > 0) {    /* 標準入力から読み込み */

        /* STEP 4: サーバに送信 */
        write(sock, buf, n);                    /* 読み込んだ内容を送信 */

        /* STEP 5: サーバからエコーを受信 */
        if((n = read(sock, buf, BUF_LEN))> 0) {
            /* 受信成功なら標準出力に出力（＝表示） */
            write(1, buf, n);
        } else {
            break;                              /* 受信失敗なら停止 */
        }
        
        if(strncmp(buf, "quit", 4) == 0) {      /* "quit"なら停止 */
            break;
        }
    }

    /* STEP 6: ソケットのクローズ */
    close(sock);
    printf("closed\n");

    return 0;
}


/* Local Variables: */
/* compile-command: "gcc tcp_echo_client.c -o tcp_echo_client.out " */
/* End: */
