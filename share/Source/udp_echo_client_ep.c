/* -*- mode: c; coding:utf-8; ##: nil; -*-                          */
/*                                                                  */
/*  FILENAME     :  udp_echo_client_ep.c                             */
/*  DESCRIPTION  :  UDP echo client                                 */
/*                  ホスト名の解決を含まない。IPアドレスで指定する。     */
/*                  epollを使った多重化                            */
/*  DATE         :  Sep. 01, 2020                                   */
/*  UPDATE       :                                                  */
/*                                                                  */

/*-------------------------- <include>  ----------------------------*/
#include "icslab2_net.h"

#define NEVENTS 16

/*-------------------------- <functions> ---------------------------*/

int main(int argc, char **argv)
{
    char *server_ipaddr_str = "127.0.0.1";      /* サーバIPアドレス（文字列） */
    unsigned int port = UDP_SERVER_PORT;        /* ポート番号（文字列） */

    int fd = 0;                     /* 標準入力 */
    int sock;                       /* ソケットディスクリプタ */
    struct sockaddr_in serverAddr;  /* サーバ＝相手用のアドレス構造体 */
    struct sockaddr_in clientAddr;  /* クライアント＝自分用のアドレス構造体 */
    int addrLen;                    /* serverAddrのサイズ */
    char buf[BUF_LEN];              /* 受信バッファ */
    int n;                          /* 読み込み／受信バイト数 */

    struct in_addr addr;            /* アドレス表示用 */

    int epfd;                       /* epollインスタンスのファイルデスクリプタ */
    struct epoll_event ev, events[NEVENTS]; /* イベントとイベントの配列 */
    int nfds;                       /* 要求するI/Oが可能なｆｄの数 */
    int i;

    int cnt = 0;                    /* timeoutのカウンタ用 */


    /* コマンドライン引数の処理 */
    if(argc == 2 && strncmp(argv[1], "-h", 2) == 0) {
        printf("Usage: %s [dst_host_ip_addr] [port]\n", argv[0]);
        return 1;
    }
    if(argc > 1)    /* 宛先を指定のIPアドレスにする。 portはデフォルト */
        server_ipaddr_str = argv[1];
    if(argc > 2)    /* 宛先を指定のIPアドレス、portにする */
        port = (unsigned int)atoi(argv[2]);

    /* STEP 1: 宛先サーバのIPアドレスとポートを指定する */
    memset(&serverAddr, 0, sizeof(serverAddr));  /* 0クリア */
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

    /* 多重化の準備 */
    /* STEP 3: epollインスタンスの作成 */
    epfd = epoll_create(NEVENTS);
    if(epfd < 0) {
        perror("epoll_create");
        return 1;
    }

    /* STEP 4: 監視するfdとイベントを登録 */
    /* fdを登録 */
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if(epoll_ctl(epfd, EPOLL_CTL_ADD, 0, &ev) != 0) {
        perror("epoll_ctl 1");
        return 1;
    }

    /* STEP 4': 監視するfdとイベントを登録 */
    /* sockを登録 */
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = sock;
    if(epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev) != 0) {
        perror("epoll_ctl 2");
        return 1;
    }

    for( ; ; ) {

        printf("before epoll_wait\n");

        /* STEP 5: イベント発生をを待つ */
        /* 最後の引数はtimeout (msec単位), -1はtimeoutなし = ブロック */
        /* nfdsには処理が可能になったイベント数が返される             */
        /* events[]には処理可能なイベントとデスクリプタが登録されている */
        nfds = epoll_wait(epfd, events, NEVENTS, 5000);
        if(nfds < 0) {
            perror("epoll_wait");
            return 1;
        }
        
        printf("after epoll_wait\n");

        /* timeout */
        if(nfds == 0) {
            printf("timeout %d\n", cnt++);
            continue;
        }

        /* STEP 6: events[]を順次確認して必要な処理を行う */
        for(i = 0; i < nfds; i++) {
            if(events[i].data.fd == fd) {
                /* STEP 7: fdに関するイベントなら */
                
                n = read(fd, buf, BUF_LEN); /* 標準入力から読み込み */

                /* 送信するデータ（=読み込んだデータ）とそのバイト数の表示 */
                buf[n] = '\0';              /* 文字列と表示するために */
                printf("Send %d bytes data: %s", n, buf);

                /* サーバにデータグラムを送信する */
                if(n != sendto(sock, buf, n, 0,
                               (struct sockaddr *)&serverAddr, sizeof(serverAddr))) {
                    perror("sendto");
                    return  -1;
                }
                
            } else if(events[i].data.fd == sock) {
                /* STEP 8: sockに関するイベントなら */
                
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
            }
        }
        
        /* printf("\n"); */

        if(strncmp(buf, "quit", 4) == 0) {     /* "quit"なら停止 */
            break;
        }
    }

    /* STEP 5: ソケットのクローズ */    
    close(sock);

    return 0;

}

/* Local Variables: */
/* compile-command: "gcc -g udp_echo_client_ep.c -o udp_echo_client_ep.out" */
/* End: */
