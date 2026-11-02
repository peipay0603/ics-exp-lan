/* -*- coding:utf-8; ##: nil; -*-                                   */
/*                                                                  */
/*  FILENAME     :  udp_echo_client_s.c                              */
/*  DESCRIPTION  :  簡略版UDP echo client                             */
/*                  localhost port 10000 にUDPでメッセージを送受信     */
/*                                                                  */

#include "icslab2_net.h"

int main(int argc, char **argv)
{
    char *server_ipaddr_str = "127.0.0.1";      /* サーバIPアドレス（文字列） */
    unsigned int port = 10000;                  /* ポート番号 */
    char *mesg = "Hello\n";

    int sock;                       /* ソケットディスクリプタ */
    struct sockaddr_in serverAddr;  /* サーバ＝相手用のアドレス構造体 */
    struct sockaddr_in clientAddr;  /* クライアント＝自分用のアドレス構造体 */
    int addrLen;                    /* serverAddrのサイズ */
    char buf[BUF_LEN];              /* 受信バッファ */
    int n;                          /* 読み込み／受信バイト数 */

    struct in_addr addr;            /* アドレス表示用 */


    /* 宛先サーバのIPアドレスとポートを指定する */
    memset(&serverAddr, 0, sizeof(serverAddr)); /* 0クリア */
    serverAddr.sin_family = AF_INET;            /* Internetプロトコル */
    serverAddr.sin_port = htons(port);          /* サーバの待受ポート */
    /* IPアドレス（文字列）から変換 */
    inet_pton(AF_INET, server_ipaddr_str, &serverAddr.sin_addr.s_addr);
    
    /* UDPソケットをオープンする */
    sock = socket(AF_INET, SOCK_DGRAM, 0);

    /* ここで、ローカルでsocketをbind()してもよいが省略する */

    n = strlen(mesg);
    strncpy(buf, mesg, n);

    n = sendto(sock, buf, n, 0,
               (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    printf("Sent %d bytes data: %s", n, buf);

    addrLen = sizeof(clientAddr);
    n = recvfrom(sock, buf, BUF_LEN, 0,
                 (struct sockaddr *)&clientAddr, (socklen_t *)&addrLen);

    /* 受信したデータとそのバイト数の表示 */
    buf[n] = '\0';
    printf("Recv %d bytes data: %s", n, buf);

    close(sock);

    return 0;

}

/* Local Variables: */
/* compile-command: "gcc -g udp_echo_client_s.c -o udp_echo_client_s.out" */
/* End: */
