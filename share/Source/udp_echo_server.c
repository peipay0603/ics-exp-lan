/* -*- mode: c; coding:utf-8; ##: nil; -*-                          */
/*                                                                  */
/*  FILENAME     :  udp_echo_server.c                               */
/*  DESCRIPTION  :  UDP Echo Server                                 */
/*                                                                  */
/*  DATE         :  Sep. 01, 2020                                   */
/*                                                                  */

#include "icslab2_net.h"

int
main(int argc, char** argv)
{
    int     sock;                  /* ソケットディスクリプタ */
    struct sockaddr_in  serverAddr; /* サーバ＝自分用アドレス構造体 */
    struct sockaddr_in  clientAddr; /* クライアント＝相手用アドレス構造体 */
    int     addrLen;                /* clientAddrのサイズ */
    char    buf[BUF_LEN];          /* 受信バッファ */
    int     n;                      /* 受信バイト数 */

    struct in_addr addr;            /* アドレス表示用 */

    /* コマンドライン引数の処理 */
    if(argc == 2 && strncmp(argv[1], "-h", 2) == 0) {
        printf("Usage: %s\n", argv[0]);
        printf("       default port # %d\n", UDP_SERVER_PORT);
        return 0;
    }

    /* STEP 1: UDPソケットをオープンする */
    if((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        return  1;
    }

    /* STEP 2: クライアントからの要求を受け付けるIPアドレスとポートを設定する */
    memset(&serverAddr, 0, sizeof(serverAddr));     /* ゼロクリア */
    serverAddr.sin_family = AF_INET;                /* Internetプロトコル */
    serverAddr.sin_port = htons(UDP_SERVER_PORT);   /* 待ち受けるポート */
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY); /* どのIPアドレス宛でも */

    /* STEP 3:ソケットとアドレスをbindする */
    if(bind(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("bind");
        return  1;
    }

    for( ; ; ) {                            /* 無限ループ */
        /* STEP 4: クライアントからのデータグラムを受けとる */
        addrLen = sizeof(clientAddr);
        n = recvfrom(sock, buf, BUF_LEN, 0,
                     (struct sockaddr *)&clientAddr, (socklen_t *)&addrLen);
        if(n < 0) {
            perror("recvfrom");
            break;
        }

        /* 受信パケットの送信元IPアドレスとポート番号を表示 */
        addr.s_addr = clientAddr.sin_addr.s_addr;
        printf("received from :  ip address: %s, ", inet_ntoa(addr));
        printf("port#: %d\n", ntohs(clientAddr.sin_port));

        /* STEP 5: 受信データをクライアントに送り返す */
        if(sendto(sock, buf, n, 0, 
                  (struct sockaddr *)&clientAddr, addrLen) != n) {
            perror("sendto");
            break;
        }
        printf("\n");
        
        if(strncmp(buf, "quit", 4) == 0)     /* "quit"なら停止 */
            break;
    }

    close(sock);                               /* ソケットのクローズ */

    return  0;
}
/*--------------------------- <end> --------------------------------*/

/* Local Variables: */
/* compile-command: "gcc -g udp_echo_server.c -o udp_echo_server.out " */
/* End: */
