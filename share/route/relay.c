/*
 * relay.c -- 最終課題 中継プログラム（スプリットTCP）
 *
 *   ./relay.out listen_port next_ip next_port
 *
 * 例（経路 3→4→5→1 の node5 側）: ./relay.out 20001 172.29.0.40 20001
 *
 * 受け付けた接続ごとに fork し，次のノードへ新しい TCP 接続を張って
 * 両方向のデータをそのまま転送する．
 *   上り（クライアント→サーバ）：要求メッセージ
 *   下り（サーバ→クライアント）：ファイルのデータ
 * 受け取った分から次々に送るので，ファイル全体を待たずに中継できる（逐次中継）．
 * TCP 接続を区間ごとに分けるため，各区間の RTT は短く，再送も区間内で済む．
 */
#include "common.h"
#include <pthread.h>

#define RELAY_BUF (256 * 1024)

struct pipe_arg {
    int from, to;
    long long bytes;
};

/* from から読んだデータを to に書き続ける．EOF なら to の送信側を閉じる */
static void *pump(void *p)
{
    struct pipe_arg *a = p;
    char *buf = malloc(RELAY_BUF);
    ssize_t n;

    while ((n = read(a->from, buf, RELAY_BUF)) > 0) {
        if (write_all(a->to, buf, (size_t)n) < 0) break;
        a->bytes += n;
    }
    shutdown(a->to, SHUT_WR);               /* 相手に EOF を伝える */
    shutdown(a->from, SHUT_RD);
    free(buf);
    return NULL;
}

static void handle(int csock, struct sockaddr_in *next)
{
    int nsock = socket(AF_INET, SOCK_STREAM, 0);
    pthread_t th;
    struct pipe_arg up, down;
    struct timespec t0, t1;
    int one = 1;

    /* 要求（小さいデータ）を Nagle で待たせない */
    setsockopt(csock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(nsock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(nsock, (struct sockaddr *)next, sizeof(*next)) < 0) {
        perror("connect next hop");
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);

    up.from = csock;   up.to = nsock;   up.bytes = 0;    /* 要求：前段 → 次段 */
    down.from = nsock; down.to = csock; down.bytes = 0;  /* データ：次段 → 前段 */
    pthread_create(&th, NULL, pump, &up);
    pump(&down);
    pthread_join(th, NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("[relay %d] down %lld bytes, up %lld bytes, %.3f sec\n",
           getpid(), down.bytes, up.bytes, elapsed_sec(&t0, &t1));
    fflush(stdout);
    close(nsock);
}

int main(int argc, char **argv)
{
    int sock0, sock, yes = 1;
    struct sockaddr_in addr, next, peer;
    socklen_t plen;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s listen_port next_ip next_port\n", argv[0]);
        return 1;
    }
    memset(&next, 0, sizeof(next));
    next.sin_family = AF_INET;
    next.sin_port = htons(atoi(argv[3]));
    if (inet_pton(AF_INET, argv[2], &next.sin_addr) != 1) {
        fprintf(stderr, "bad next_ip: %s\n", argv[2]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    sock0 = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(sock0, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(argv[1]));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(sock0, 64) < 0) {
        perror("listen");
        return 1;
    }
    printf("relay: listen %s -> %s:%s\n", argv[1], argv[2], argv[3]);
    fflush(stdout);

    for (;;) {
        plen = sizeof(peer);
        sock = accept(sock0, (struct sockaddr *)&peer, &plen);
        if (sock < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        if (fork() == 0) {
            close(sock0);
            handle(sock, &next);
            close(sock);
            _exit(0);
        }
        close(sock);
    }
    return 0;
}
