/* -*- mode: c; coding:utf-8; ##: nil; -*-                          */
/*                                                                  */
/*  FILENAME     :  udp_client2.c                                   */
/*  DESCRIPTION  :  UDP echo client                                 */
/*                  getaddrinfo()によるアドレス解決を含む              */
/*  DATE         :  Sep. 01, 2020                                   */
/*  UPDATE       :                                                  */
/*                                                                  */

/*-------------------------- <include>  ----------------------------*/
#include "icslab2_net.h"

/*-------------------------- <functions> ---------------------------*/

int main(int argc, char **argv)
{
    char *server_name_str = SERVER_HOSTNAME; /* サーバ名（文字列） */
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
        printf("Usage: %s [dst_host] [port]\n", argv[0]);
        return 1;
    }
    if(argc == 2) {
        server_name_str = argv[1];
    } else if(argc == 3) {
        server_name_str = argv[1];
        port_num_str = argv[2];
    }

    /* ホスト名からIPアドレスを検索する */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          /* IPv4で通信 */
    hints.ai_socktype = SOCK_DGRAM;     /* UDP */

    err = getaddrinfo(server_name_str, port_num_str, &hints, &res);
    if(err != 0) {
        perror("getaddrinfo");
        return 1;
    }

    /* 確認用：解決したIPアドレスを文字列に変換して表示 */
    addr.s_addr = ((struct sockaddr_in *)(res->ai_addr))->sin_addr.s_addr;
    printf("ip address: %s\n", inet_ntoa(addr));
    printf("port#: %d\n", ntohs(((struct sockaddr_in*)(res->ai_addr))->sin_port));


    /* UDPソケットをオープンする */
    /* 参考： sock = socket(AF_INET, SOCK_DGRAM, 0); */
    sock = socket(res->ai_family, res->ai_socktype, 0);
    if(sock < 0) {
        perror("socket");
        return  1;
    }

    /* ここで、ローカルでsocketをbind()してもよいが省略する */
    
    while((n = read(0, buf, BUF_LEN)) > 0) {  /* 標準入力から読み込み */

        /* 送信するデータ（読み込んだデータ）とそのバイト数の表示 */
        buf[n] = '\0';
        printf("Send %d bytes data: %s", n, buf);

        /* サーバにデータグラムを送信する */
        /* 参考:
           sendto(sock, buf, n, 0, 
           (struct sockaddr *)&serverAddr, sizeof(serverAddr)) */
        if((n = sendto(sock, buf, n, 0,
                       res->ai_addr, res->ai_addrlen)) != n) {
            perror("sendto");
            return  -11;
        }

        /* サーバからデータグラムを受けとる */
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
        
        if(0 == strncmp(buf, "quit", 4)) {     /* "quit"なら停止 */
            break;
        }
    }

    close(sock);
    freeaddrinfo(res);

    return 0;

}

/* Local Variables: */
/* compile-command: "gcc -g udp_echo_client2.c -o udp_echo_client2.out" */
/* End: */
