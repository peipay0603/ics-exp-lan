/*  -*- coding: utf-8-unix; -*-                                     */
/*  FILENAME     :  hyb_recv.c                                      */
/*  DESCRIPTION  :  UDP（データ）+ TCP（制御）でファイルを受け取る  */
/*                  受信側 node1                                    */
/*  USAGE        :  hyb_recv.out [-w quiet_ms] port output_file     */
/*                                                                  */
/*  port番号で、制御用TCPとデータ用UDPの両方を待ち受ける。           */
/*  データグラムは順番どおりに届かなくてもよい。                      */
/*  seq番目のチャンクをファイルの seq*chunk バイト目へ pwrite() する。 */
/*  どの番号が届いたかはビットマップ（1チャンク1ビット）で覚える。     */
/*                                                                  */
/*  送信側から ROUND_END が来たら、まだ経路上にあるデータグラムを     */
/*  待つため、UDPが quiet_ms ミリ秒来なくなってから未着範囲を NACK する。 */
/*  全チャンクがそろったら、その時点で DONE を返す。                  */

#include "common.h"
#include <poll.h>

#define RECV_BATCH  256     /* 1回の poll() の後にまとめて受け取る最大数 */

static uint8_t *bitmap;
static uint32_t total_chunks;

static int has_chunk(uint32_t seq)
{
    return bitmap[seq >> 3] & (1u << (seq & 7));
}

/* 未着のチャンクを範囲 [start, start+len) のリストにする */
static uint32_t build_nack(struct hyb_range *ranges)
{
    uint32_t n = 0, seq = 0;
    while(seq < total_chunks && n < HYB_MAX_RANGES) {
        uint32_t start;
        if(has_chunk(seq)) {
            seq++;
            continue;
        }
        start = seq;
        while(seq < total_chunks && !has_chunk(seq))
            seq++;
        ranges[n].start = htonl(start);
        ranges[n].len = htonl(seq - start);
        n++;
    }
    return n;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-w quiet_ms] port output_file\n"
        "  -w  ROUND_END後、UDPがこの時間来なければ未着とみなす（既定 100ms）\n",
        prog);
}

int main(int argc, char **argv)
{
    long quiet_ms = 100;
    int opt, port, usock, lsock, ctrl, fd, rcvbuf;
    int yes = 1;
    char *filename, *buf;
    struct sockaddr_in addr, ctrlAddr;
    socklen_t addrLen;
    struct hyb_hello hello;
    struct hyb_msg m;
    struct hyb_range *ranges;
    uint32_t chunk, session, last_len, received = 0;
    uint32_t sender_round = 0, nack_rounds = 0, received_at_round_end = 0;
    uint64_t file_size;
    unsigned long long dup = 0, stale = 0, bad = 0, nacked = 0;
    uint64_t last_udp_ns = 0, round_end_ns = 0;
    int round_end_pending = 0, have_first = 0;
    struct timespec start_time, first_time, end_time;
    double e2e, local;

    while((opt = getopt(argc, argv, "w:")) != -1) {
        if(opt == 'w') {
            quiet_ms = strtol(optarg, NULL, 10);
            if(quiet_ms < 1 || quiet_ms > 60000) {
                fprintf(stderr, "invalid quiet_ms\n");
                return 1;
            }
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if(argc - optind != 2) {
        usage(argv[0]);
        return 1;
    }
    port = parse_port(argv[optind]);
    filename = argv[optind + 1];
    if(port < 0) {
        fprintf(stderr, "invalid port\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    /* STEP 1: UDP（データ）を先にbindし、TCP（制御）で接続を待つ */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    usock = socket(AF_INET, SOCK_DGRAM, 0);
    if(usock < 0 || bind(usock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("udp bind");
        return 1;
    }
    rcvbuf = grow_rcvbuf(usock, 32 * 1024 * 1024);
    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if(lsock < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if(bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(lsock, 1) < 0) {
        perror("tcp bind/listen");
        return 1;
    }
    printf("waiting connection... port=%d (TCP control + UDP data), UDP rcvbuf=%d bytes\n",
           port, rcvbuf);
    fflush(stdout);

    /* STEP 2: 制御接続を受け付け、helloを読む */
    addrLen = sizeof(ctrlAddr);
    ctrl = accept(lsock, (struct sockaddr *)&ctrlAddr, &addrLen);
    if(ctrl < 0) {
        perror("accept");
        return 1;
    }
    close(lsock);
    setsockopt(ctrl, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    if(read_all(ctrl, &hello, sizeof(hello)) != 0 || ntohl(hello.magic) != HYB_MAGIC) {
        fprintf(stderr, "bad hello（送信側は hyb_send.out ですか）\n");
        return 1;
    }
    chunk = ntohl(hello.chunk);
    file_size = be64toh(hello.file_size);
    total_chunks = ntohl(hello.total_chunks);
    session = ntohl(hello.session);
    start_time.tv_sec = (time_t)(int64_t)be64toh((uint64_t)hello.start_sec);
    start_time.tv_nsec = (long)(int64_t)be64toh((uint64_t)hello.start_nsec);
    if(chunk < 1 || chunk > HYB_MAX_CHUNK
       || (file_size + chunk - 1) / chunk != total_chunks) {
        fprintf(stderr, "bad hello parameters\n");
        return 1;
    }
    last_len = total_chunks ? (uint32_t)(file_size - (uint64_t)(total_chunks - 1) * chunk) : 0;

    /* STEP 3: 保存先を用意する（順不同で書くので先に大きさを決めておく） */
    fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if(fd < 0 || ftruncate(fd, (off_t)file_size) < 0) {
        perror(filename);
        return 1;
    }
    bitmap = calloc((size_t)total_chunks / 8 + 1, 1);
    buf = malloc(HYB_DGRAM_HDR + chunk);
    ranges = malloc(sizeof(struct hyb_range) * HYB_MAX_RANGES);
    if(bitmap == NULL || buf == NULL || ranges == NULL) {
        perror("malloc");
        return 1;
    }
    if(send_msg(ctrl, MSG_READY, 0, 0) < 0) {
        perror("write READY");
        return 1;
    }

    /* STEP 4: UDPのデータと、TCPの制御メッセージを同時に待つ */
    while(received < total_chunks) {
        struct pollfd pfd[2];
        int timeout = -1, k, r;

        if(round_end_pending) {
            uint64_t base = last_udp_ns > round_end_ns ? last_udp_ns : round_end_ns;
            uint64_t deadline = base + (uint64_t)quiet_ms * 1000000ull;
            uint64_t now = mono_ns();
            if(now >= deadline) {
                /* STEP 5: しばらくUDPが来ない → 未着範囲を NACK で知らせる */
                uint32_t n = build_nack(ranges);
                if(received == received_at_round_end)
                    fprintf(stderr, "warning: round %u で新しいデータが1つも届いていません"
                            "（UDPの宛先・中継を確認してください）\n", sender_round);
                nacked += total_chunks - received;
                printf("round %u: missing %u -> NACK (%u ranges)\n",
                       sender_round, total_chunks - received, n);
                if(send_msg(ctrl, MSG_NACK, sender_round, n) < 0
                   || write_all(ctrl, ranges, sizeof(struct hyb_range) * n) < 0) {
                    perror("write NACK");
                    return 1;
                }
                nack_rounds++;
                round_end_pending = 0;
                received_at_round_end = received;
                continue;
            }
            timeout = (int)((deadline - now + 999999) / 1000000);
        }

        pfd[0].fd = usock;
        pfd[0].events = POLLIN;
        pfd[1].fd = ctrl;
        pfd[1].events = POLLIN;
        r = poll(pfd, 2, timeout);
        if(r < 0) {
            if(errno == EINTR)
                continue;
            perror("poll");
            return 1;
        }

        if(pfd[0].revents & POLLIN) {
            for(k = 0; k < RECV_BATCH && received < total_chunks; k++) {
                uint32_t seq, len, expect;
                ssize_t n = recv(usock, buf, HYB_DGRAM_HDR + chunk, MSG_DONTWAIT | MSG_TRUNC);
                if(n < 0) {
                    if(errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    if(errno == EINTR)
                        continue;
                    perror("recv udp");
                    return 1;
                }
                if(n < HYB_DGRAM_HDR || n > (ssize_t)(HYB_DGRAM_HDR + chunk)) {
                    bad++;
                    continue;
                }
                if(ntohl(((uint32_t *)buf)[0]) != session) {
                    stale++;    /* 以前の転送の残り */
                    continue;
                }
                seq = ntohl(((uint32_t *)buf)[1]);
                len = (uint32_t)n - HYB_DGRAM_HDR;
                expect = (seq == total_chunks - 1) ? last_len : chunk;
                if(seq >= total_chunks || len != expect) {
                    bad++;
                    continue;
                }
                if(has_chunk(seq)) {
                    dup++;
                    continue;
                }
                if(!have_first) {
                    clock_gettime(CLOCK_REALTIME, &first_time);
                    have_first = 1;
                }
                if(pwrite(fd, buf + HYB_DGRAM_HDR, len, (off_t)seq * chunk) != (ssize_t)len) {
                    perror("pwrite");
                    return 1;
                }
                bitmap[seq >> 3] |= (uint8_t)(1u << (seq & 7));
                received++;
            }
            last_udp_ns = mono_ns();
        }

        if(pfd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if(recv_msg(ctrl, &m) != 0) {
                fprintf(stderr, "control connection closed (received %u / %u chunks)\n",
                        received, total_chunks);
                return 1;
            }
            if(m.type != MSG_ROUND_END) {
                fprintf(stderr, "unexpected control message type=%u\n", m.type);
                return 1;
            }
            sender_round = m.round;
            round_end_ns = mono_ns();
            round_end_pending = 1;
        }
    }

    /* STEP 6: 全部そろった。保存を終えて DONE を返す */
    if(close(fd) < 0) {
        perror("close file");
        return 1;
    }
    clock_gettime(CLOCK_REALTIME, &end_time);
    if(!have_first)
        first_time = end_time;
    if(send_msg(ctrl, MSG_DONE, sender_round, 0) < 0)
        perror("write DONE");
    /* 送信側が先に閉じるのを待つ（未読データを残してcloseするとRSTになり、
       送信側がDONEを読めないことがあるため） */
    shutdown(ctrl, SHUT_WR);
    {
        struct pollfd pfd;
        char tmp[256];
        pfd.fd = ctrl;
        pfd.events = POLLIN;
        while(poll(&pfd, 1, 2000) > 0 && read(ctrl, tmp, sizeof(tmp)) > 0)
            ;
    }
    close(ctrl);

    e2e = ts_diff(&start_time, &end_time);
    local = ts_diff(&first_time, &end_time);
    printf("From: %s (control)\n", inet_ntoa(ctrlAddr.sin_addr));
    printf("File: %llu bytes, chunk %u bytes x %u\n",
           (unsigned long long)file_size, chunk, total_chunks);
    printf("Receive bytes: %llu / %llu OK\n",
           (unsigned long long)file_size, (unsigned long long)file_size);
    printf("Datagrams: dup=%llu stale=%llu bad=%llu\n", dup, stale, bad);
    printf("NACK rounds: %u (NACKした延べチャンク数 %llu)\n", nack_rounds, nacked);
    printf("Time (送信開始→保存完了): %.6f sec\n", e2e);
    if(e2e > 0)
        printf("Goodput: %.3f Mbps\n", (double)file_size * 8.0 / e2e / 1e6);
    printf("Time (最初のデータ→保存完了, 参考): %.6f sec\n", local);
    printf("CPU time (user+sys): %.6f sec\n", cpu_seconds());
    if(e2e <= 0 || e2e + 1.0 < local)
        fprintf(stderr, "warning: 送信側と時計が共有されていない可能性があります\n");
    printf("RESULT prog=hyb_recv chunk=%u bytes=%llu nack_rounds=%u nacked=%llu dup=%llu"
           " time=%.6f goodput_mbps=%.3f local_time=%.6f cpu=%.6f rcvbuf=%d\n",
           chunk, (unsigned long long)file_size, nack_rounds, nacked, dup, e2e,
           e2e > 0 ? (double)file_size * 8.0 / e2e / 1e6 : 0.0, local, cpu_seconds(), rcvbuf);
    return 0;
}
