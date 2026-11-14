/*  -*- coding: utf-8-unix; -*-                                     */
/*  FILENAME     :  udp_relay.c                                     */
/*  DESCRIPTION  :  UDPデータグラムを次のノードへそのまま転送する中継 */
/*  USAGE        :  udp_relay.out listen_port next_ip next_port     */
/*                                                                  */
/*  受け取った1データグラムを、中身を見ずに1データグラムとして送る。   */
/*  TCPの中継と違い、受信確認や再送はしない（それは端の hyb_send/recv が行う）。 */
/*  1スレッドで順番に送るので、届いた順番は変わらない。               */
/*  2秒間何も来なかったとき、それまでの転送数を表示する。Ctrl+Cで終了。 */
/*                                                                  */
/*  例（3→4→5→1）:                                                 */
/*   node5$ ./udp_relay.out 20000 172.23.0.10 20000                 */
/*   node4$ ./udp_relay.out 20000 172.29.0.50 20000                 */

#include "common.h"
#include <poll.h>

#define RELAY_BATCH 256

int main(int argc, char **argv)
{
    int listen_port, next_port, in, out, rcvbuf;
    struct sockaddr_in addr, nextAddr;
    static char buf[65536];
    unsigned long long count = 0, bytes = 0, refused = 0, reported = 0;

    if(argc != 4) {
        fprintf(stderr, "Usage: %s listen_port next_ip next_port\n", argv[0]);
        return 1;
    }
    listen_port = parse_port(argv[1]);
    next_port = parse_port(argv[3]);
    if(listen_port < 0 || next_port < 0) {
        fprintf(stderr, "invalid port\n");
        return 1;
    }
    if(make_addr(argv[2], next_port, &nextAddr) < 0)
        return 1;

    /* STEP 1: 受信用ソケットを listen_port に bind する */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)listen_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    in = socket(AF_INET, SOCK_DGRAM, 0);
    if(in < 0 || bind(in, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    rcvbuf = grow_rcvbuf(in, 32 * 1024 * 1024);

    /* STEP 2: 送信用ソケットを次のノードへ connect する（宛先を固定するだけ） */
    out = socket(AF_INET, SOCK_DGRAM, 0);
    if(out < 0 || connect(out, (struct sockaddr *)&nextAddr, sizeof(nextAddr)) < 0) {
        perror("connect");
        return 1;
    }
    printf("udp relay: port %d -> %s:%d, rcvbuf=%d bytes\n",
           listen_port, argv[2], next_port, rcvbuf);
    fflush(stdout);

    /* STEP 3: 受け取ったら、そのまま次へ送る */
    for(;;) {
        struct pollfd pfd;
        int k, r;

        pfd.fd = in;
        pfd.events = POLLIN;
        r = poll(&pfd, 1, 2000);
        if(r < 0) {
            if(errno == EINTR)
                continue;
            perror("poll");
            return 1;
        }
        if(r == 0) {
            if(count != reported) {
                printf("forwarded %llu datagrams (%llu bytes), send refused %llu\n",
                       count, bytes, refused);
                fflush(stdout);
                reported = count;
            }
            continue;
        }
        for(k = 0; k < RELAY_BATCH; k++) {
            ssize_t n = recv(in, buf, sizeof(buf), MSG_DONTWAIT);
            if(n < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                if(errno == EINTR)
                    continue;
                perror("recv");
                return 1;
            }
            for(;;) {
                if(send(out, buf, (size_t)n, 0) >= 0)
                    break;
                if(errno == EINTR)
                    continue;
                if(errno == ECONNREFUSED) {
                    refused++;  /* 次のノードが待ち受けていなかった。捨てる */
                    break;
                }
                if(errno == ENOBUFS || errno == EAGAIN) {
                    usleep(100);
                    continue;
                }
                perror("send");
                return 1;
            }
            count++;
            bytes += (unsigned long long)n;
        }
    }
}
