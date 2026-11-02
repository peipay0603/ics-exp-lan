/* -*- mode: c; coding:utf-8; ##: nil; -*-                          */
/*                                                                  */
/*  FILENAME     :  lab_udp_ecli.c                                  */
/*  DESCRIPTION  :  UDP echo clientの練習                           */
/*                                                                  */
/*  DATE         :  Sep. 01, 2020                                   */
/*                                                                  */

#include "icslab2_net.h"

int main(int argc, char **argv)
{
    char *server_ipaddr_str = "127.0.0.1";      /* サーバIPアドレス（文字列） */
    unsigned int port = UDP_SERVER_PORT;        /* ポート番号 */
    char *port_num_str = UDP_SERVER_PORT_STR;    /* ポート番号（文字列） */

    int sock;                       /* ソケットディスクリプタ */
    struct sockaddr_in serverAddr;  /* サーバ＝相手用のアドレス構造体 */
    struct sockaddr_in clientAddr;  /* クライアント＝自分用のアドレス構造体 */
    int addrLen;                    /* serverAddrのサイズ */
    char buf[BUF_LEN];              /* 受信バッファ */
    int n;                          /* 読み込み／受信バイト数 */

    struct addrinfo hints, *res;    /* アドレス情報の構造体 */
    struct in_addr addr;            /* 表示作業用 */
    int err;

    /* コマンドライン引数の処理 */
    if(argc == 2 && strncmp(argv[1], "-h", 2) == 0) {
        printf("Usage: %s [dst_ip_addr] [port]\n", argv[0]);
        return 0;
    }
    if(argc > 1)    /* 宛先を指定のIPアドレスにする。 portはデフォルト */
        server_ipaddr_str = argv[1];
    if(argc > 2)    /* 宛先を指定のIPアドレス、portにする */
        server_ipaddr_str = argv[1];
        port_num_str = argv[2];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          /* IPv4で通信 */
    hints.ai_socktype = SOCK_DGRAM;     /* UDP */

    err = getaddrinfo(server_ipaddr_str, port_num_str, &hints, &res);
    if(err != 0) {
        perror("getaddrinfo");
        return 1;
    }

    /* STEP 1: 宛先サーバのIPアドレスとポートを指定する */
    memset(&serverAddr, 0, sizeof(serverAddr)); /* 0クリア */
    serverAddr.sin_family = AF_INET;            /* Internetプロトコル */
    serverAddr.sin_port = htons(port);          /* サーバの待受ポート */
    /* IPアドレス（文字列）から変換 */
    inet_pton(AF_INET, server_ipaddr_str, &serverAddr.sin_addr.s_addr);
    
    /* 確認用：IPアドレスとポートを文字列に変換して表示 */
    addr.s_addr = serverAddr.sin_addr.s_addr;
    printf("ip address: %s\n", inet_ntoa(addr));
    printf("port#: %d\n", ntohs(serverAddr.sin_port));

    /* STEP 2xxx: UDPソケットをオープンする */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock < 0) {
        perror("socket");
        return  1;
    }

    /* ここで、ローカルでsocketをbind()してもよいが省略する */

    /* 標準入力から文字列を読み込むたびにサーバに送信し応答を待つ */
    while((n = read(0, buf, BUF_LEN)) > 0) {  /* 標準入力から読み込み */
 
        /* 送信するデータ（読み込んだデータ）とそのバイト数の表示 */
        buf[n] = '\0';              /* 文字列と表示するために */
        printf("Send %d bytes data: %s", n, buf);

        /* STEP 3xxx: サーバにデータグラムを送信する */
        if(n != sendto(sock, buf, n, 0,
                    (struct sockaddr *)&serverAddr, addrLen)) {
            perror("sendto");
            return  -1;
        }

        /* STEP 4xxx: サーバからデータグラムを受けとる */
        addrLen = sizeof(clientAddr);
        n = recvfrom(sock, buf, BUF_LEN, 0,
                     (struct sockaddr *)&clientAddr, (socklen_t *)&addrLen);
        if(n < 0) {
            perror("recvfrom");
        }

        /* 受信したデータとそのバイト数の表示 */
        buf[n] = '\0';
        printf("Recv %d bytes data: %s", n, buf);
        /* write(1, buf, n); をデータの表示に使用してもよい */

        printf("\n");

        if(strncmp(buf, "quit", 4) == 0) {     /* "quit"なら停止 */
            break;
        }
    }

    /* STEP 5: ソケットのクローズ */
    close(sock);

    return 0;

}

/* Option: ホス名を解決するように修正する */
/* hint:  getaddrinfo()               */

