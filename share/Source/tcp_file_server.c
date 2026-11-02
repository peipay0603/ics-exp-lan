/*  -*- coding: utf-8-unix; -*-                                     */
/*  FILENAME     :  tcp_file_server.c                                 */
/*  DESCRIPTION  :  Simple TCP file Server                          */
/*  USAGE:          tcp_file_server.out filename                    */
/*  VERSION      :                                                  */
/*  DATE         :  Sep 01, 2020                                    */
/*  UPDATE       :                                                  */
/*                                                                  */

#include "icslab2_net.h"

int
main(int argc, char** argv)
{
    int     sock0;                  /* 待ち受け用ソケットディスクリプタ */
    int     sock;                   /* ソケットディスクリプタ */
    struct sockaddr_in  serverAddr; /* サーバ＝自分用アドレス構造体 */
    struct sockaddr_in  clientAddr; /* クライアント＝相手用アドレス構造体 */
    int     addrLen;                /* clientAddrのサイズ */

    char    buf[BUF_LEN];           /* 受信バッファ */
    int     n;                      /* 受信バイト数 */
    int     isEnd = 0;              /* 終了フラグ，0でなければ終了 */

    char *filename;                 /* 返送するファイルの名前 */
    int fd;                         /* ファイルデスクリプタ */

    int     yes = 1;                /* setsockopt()用 */

    /* コマンドライン引数の処理 */
    if(argc != 2) {
        printf("Usage: %s filename\n", argv[0]);
        printf("ex. %s http_get_req.txt\n", argv[0]);
        return 0;
    }

    /* 返送するファイル名 */
    filename = argv[1];
    printf("filename: %s\n", filename);
    
    /* STEP 1: TCPソケットをオープンする */
    if((sock0 = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socke");
        return  1;
    }

    /* sock0のコネクションがTIME_WAIT状態でもbind()できるように設定 */
    setsockopt(sock0, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&yes, sizeof(yes));

    /* STEP 2: クライアントからの要求を受け付けるIPアドレスとポートを設定する */
    memset(&serverAddr, 0, sizeof(serverAddr));     /* ゼロクリア */
    serverAddr.sin_family = AF_INET;                /* Internetプロトコル */
    serverAddr.sin_port = htons(TCP_SERVER_PORT);   /* 待ち受けるポート */
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY); /* どのIPアドレス宛でも */

    /* STEP 3: ソケットとアドレスをbindする */
    if(bind(sock0, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("bind");
        return  1;
    }

    /* STEP 4: コネクションの最大同時受け入れ数を指定する */
    if(listen(sock0, 5) != 0) {
        perror("listen");
        return  1;
    }

    while(!isEnd) {     /* 終了フラグが0の間は繰り返す */

        /* STEP 5: クライアントからの接続要求を受け付ける */        
        printf("waiting connection...\n");
        addrLen = sizeof(clientAddr);
        sock = accept(sock0, (struct sockaddr *)&clientAddr, (socklen_t *)&addrLen);
        if(sock < 0) {
            perror("accept");
            return  1;
        }

        /* STEP 6: クライアントからのファイル要求の受信 */
        if((n = read(sock, buf, BUF_LEN)) < 0) {
            close(sock);
            break;
        }

        /* 今回は表示するだけで中身は無視 */
        buf[n] = '\0';
        printf("recv req: %s\n", buf);

        /* STEP 7: 送信するファイルをオープン */
        fd = open(filename, O_RDONLY);
        if(fd < 0) {
            perror("open");
            return 1;
        }

        /* STEP 8: ファイル読み込むたびに送信 */
        while((n = read(fd, buf, BUF_LEN)) > 0) {
            write(sock, buf, n);
        }

        /* write(sock, NULL, 0); */

        /* STEP 9: 通信用のソケットのクローズ */        
        close(sock);
        printf("closed\n");
    }

    /* STEP 10: 待ち受け用ソケットのクローズ */
    close(sock0);

    return  0;
}

/* Local Variables: */
/* compile-command: "gcc tcp_file_server.c -o tcp_file_server.out" */
/* End: */
