/* -*- mode: c; coding:utf-8; ##: nil; -*-                          */
/*                                                                  */
/*  FILENAME     :  udp_echo_client.c                              */
/*  DESCRIPTION  :  UDP echo client                                 */
/*                  ホスト名の解決を含まない。IPアドレスで指定する。     */
/*  USAGE:   $ udp_echo_client.out [dest host] [port]            */
/*           引数がなければ、デフォルトのlocalhost, port# 10000 に送信 */
/*           dest host が指定されていれば、dest host, port# 10000に送信 */
/*           dest hostとportが指定されていれば、それらを使用            */
/*                                                                  */
/*  VERSION      :                                                  */
/*  DATE         :  Sep. 01, 2020                                   */
/*  UPDATE       :                                                  */
/*                                                                  */

#include "icslab2_net.h"

int main(int argc, char **argv)
{
    char *server_ipaddr_str = "127.0.0.1";      /* サーバIPアドレス（文字列） */
    unsigned int port = UDP_SERVER_PORT;        /* ポート番号 */

    int sock;                       /* ソケットディスクリプタ */
    struct sockaddr_in serverAddr;  /* サーバ＝相手用のアドレス構造体 */
    struct sockaddr_in clientAddr;  /* クライアント＝自分用のアドレス構造体 */
    int addrLen;                    /* serverAddrのサイズ */
    char buf[BUF_LEN];              /* 受信バッファ */
    int n;                          /* 読み込み／受信バイト数 */

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

    /* STEP 1: 宛先サーバのIPアドレスとポートを指定する */
    memset(&serverAddr, 0, sizeof(serverAddr)); /* 0クリア */
    serverAddr.sin_family = AF_INET;            /* Internetプロトコル */
    serverAddr.sin_port = htons(port);          /* サーバの待受ポート */
    /* IPアドレス（文字列）から変換 */
    inet_pton(AF_INET, server_ipaddr_str, &serverAddr.sin_addr.s_addr);
    
    /* 確認用：IPアドレスを文字列に変換して表示 */
    addr.s_addr = serverAddr.sin_addr.s_addr;
    printf("ip address: %s\n", inet_ntoa(addr));
    printf("port#: %d\n", ntohs(serverAddr.sin_port));

    /* STEP 2: UDPソケットをオープンする */
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

        /* STEP 3: サーバにデータグラムを送信する */
        if(n != sendto(sock, buf, n, 0,
                      (struct sockaddr *)&serverAddr, sizeof(serverAddr))) {
            perror("sendto");
            return  -1;
        }

        /* STEP 4: サーバからデータグラムを受けとる */
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

/* Local Variables: */
/* compile-command: "gcc -g udp_echo_client.c -o udp_echo_client.out" */
/* End: */
