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
/*                                                                  */
/*  送信側が -a（経路の状況に合わせる）のときは、経路ごとに            */
/*  届いた数・抜けた番号・片道遅延の増え方を集計し、0.1秒ごとに         */
/*  MSG_STATS で送信側へ返す。                                       */

#include "common.h"
#include "output_file.h"
#include <poll.h>

#define RECV_BATCH  256     /* 1回の poll() の後にまとめて受け取る最大数 */

static uint8_t *bitmap;
static uint32_t total_chunks;

/* -a のとき、経路ごとの受信状況 */
struct rpath {
    int seen;
    uint32_t received;      /* 受け取ったデータグラム数（累計）                 */
    uint32_t next;          /* 経路内の通し番号の最大+1（32ビットに広げた値）   */
    uint32_t first_raw;     /* 最初の「受信時刻−送信時刻」。これとの差を返す    */
    int32_t ivl_min;        /* この0.1秒間の最小の遅延                           */
    uint32_t ivl_n, ivl_first_us, ivl_last_us;  /* この0.1秒間の個数と最初・最後の到着時刻 */
};
static struct rpath rpaths[HYB_MAX_PATHS];

/* 1データグラムを経路ごとの集計に加える。
   遅延は送信側と受信側の時計の差を含むが、送信側は同じ経路の値どうしの差だけを見るので打ち消し合う。 */
static void note_path(const uint32_t *w, const struct timespec *rx)
{
    uint32_t info = ntohl(w[2]), count = info & 0xFFFFFF;
    uint32_t rx_us = (uint32_t)((uint64_t)rx->tv_sec * 1000000u + (uint64_t)rx->tv_nsec / 1000);
    uint32_t raw = rx_us - ntohl(w[3]);
    struct rpath *rp = &rpaths[info >> 24];
    int32_t rel;

    rp->received++;
    if(!rp->seen) {
        rp->seen = 1;
        rp->next = count + 1;
        rp->first_raw = raw;
        rp->ivl_min = INT32_MAX;
    } else {
        uint32_t ahead = (count - rp->next) & 0xFFFFFF;
        if(ahead < 0x800000)
            rp->next += ahead + 1;      /* 抜けた番号（落ちたもの）も含めて先へ進む */
    }
    rel = (int32_t)(raw - rp->first_raw);
    if(rel < rp->ivl_min)
        rp->ivl_min = rel;
    if(rp->ivl_n++ == 0)
        rp->ivl_first_us = rx_us;
    rp->ivl_last_us = rx_us;
}

/* 経路ごとの受信状況を送信側へ返す */
static int send_stats(int ctrl, uint64_t interval_ns)
{
    struct {
        struct hyb_msg m;
        struct hyb_pstat s[HYB_MAX_PATHS];
    } out;
    uint32_t n = 0, i;

    for(i = 0; i < HYB_MAX_PATHS; i++) {
        struct rpath *rp = &rpaths[i];
        if(!rp->seen)
            continue;
        out.s[n].path = htonl(i);
        out.s[n].received = htonl(rp->received);
        out.s[n].passed = htonl(rp->next);
        out.s[n].owd_us = (int32_t)htonl((uint32_t)(rp->ivl_n ? rp->ivl_min : HYB_NO_SAMPLE));
        out.s[n].span_us = htonl(rp->ivl_n >= 2 ? rp->ivl_last_us - rp->ivl_first_us : 0);
        out.s[n].reserved = 0;
        rp->ivl_min = INT32_MAX;
        rp->ivl_n = 0;
        n++;
    }
    out.m.type = htonl(MSG_STATS);
    out.m.round = 0;
    out.m.count = htonl(n);
    out.m.reserved = htonl((uint32_t)(interval_ns / 1000));
    return write_all(ctrl, &out, sizeof(out.m) + n * sizeof(out.s[0]));
}

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
    uint64_t last_report_ns = 0, next_report_ns = 0;
    int round_end_pending = 0, have_first = 0, adaptive;
    uint32_t hdr_len;
    struct timespec start_time, first_time, end_time;
    double e2e, local;
    struct iovec iov;
    struct msghdr msg;
    char cbuf[CMSG_SPACE(sizeof(struct timespec))];

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
    if(read_all(ctrl, &hello, sizeof(hello)) != 0
       || (ntohl(hello.magic) != HYB_MAGIC && ntohl(hello.magic) != HYB_MAGIC_ADAPTIVE)) {
        fprintf(stderr, "bad hello（送信側は hyb_send.out ですか）\n");
        return 1;
    }
    adaptive = ntohl(hello.magic) == HYB_MAGIC_ADAPTIVE;
    hdr_len = adaptive ? HYB_ADAPT_HDR : HYB_DGRAM_HDR;
    if(adaptive) {
        /* カーネルが受け取った時刻を使う。このプログラムの処理待ちを遅延に含めないため */
        setsockopt(usock, SOL_SOCKET, SO_TIMESTAMPNS, &yes, sizeof(yes));
    }
    chunk = ntohl(hello.chunk);
    file_size = be64toh(hello.file_size);
    total_chunks = ntohl(hello.total_chunks);
    session = ntohl(hello.session);
    start_time.tv_sec = (time_t)(int64_t)be64toh((uint64_t)hello.start_sec);
    start_time.tv_nsec = (long)(int64_t)be64toh((uint64_t)hello.start_nsec);
    if(chunk < 1 || chunk > 65507 - hdr_len
       || file_size / chunk + (file_size % chunk != 0) != total_chunks) {
        fprintf(stderr, "bad hello parameters\n");
        return 1;
    }
    last_len = total_chunks ? (uint32_t)(file_size - (uint64_t)(total_chunks - 1) * chunk) : 0;

    /* STEP 3: 保存先を用意する（順不同で書くので先に大きさを決めておく） */
    fd = output_open(filename);
    if(fd < 0 || ftruncate(fd, (off_t)file_size) < 0) {
        perror(filename);
        return 1;
    }
    bitmap = calloc((size_t)total_chunks / 8 + 1, 1);
    buf = malloc(hdr_len + chunk);
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
        int timeout = -1, k, r, got_new = 0;
        uint64_t now = mono_ns();

        if(adaptive && have_first) {
            /* -a: 0.1秒ごとに経路ごとの受信状況を返す */
            if(now >= next_report_ns) {
                if(send_stats(ctrl, now - last_report_ns) < 0) {
                    perror("write STATS");
                    return 1;
                }
                last_report_ns = now;
                next_report_ns = now + HYB_REPORT_MS * 1000000ull;
            }
            timeout = (int)((next_report_ns - now + 999999) / 1000000);
        }

        if(round_end_pending) {
            uint64_t base = last_udp_ns > round_end_ns ? last_udp_ns : round_end_ns;
            uint64_t deadline = base + (uint64_t)quiet_ms * 1000000ull;
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
            k = (int)((deadline - now + 999999) / 1000000);
            if(timeout < 0 || k < timeout)
                timeout = k;
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
                ssize_t n;
                iov.iov_base = buf;
                iov.iov_len = hdr_len + chunk;
                memset(&msg, 0, sizeof(msg));
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                msg.msg_control = cbuf;
                msg.msg_controllen = sizeof(cbuf);
                n = recvmsg(usock, &msg, MSG_DONTWAIT | MSG_TRUNC);
                if(n < 0) {
                    if(errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    if(errno == EINTR)
                        continue;
                    perror("recv udp");
                    return 1;
                }
                if(n < (ssize_t)hdr_len || n > (ssize_t)(hdr_len + chunk)) {
                    bad++;
                    continue;
                }
                if(ntohl(((uint32_t *)buf)[0]) != session) {
                    stale++;    /* 以前の転送の残り */
                    continue;
                }
                if(adaptive) {
                    struct timespec rx = {0, 0};
                    struct cmsghdr *cm;
                    if((ntohl(((uint32_t *)buf)[2]) >> 24) >= HYB_MAX_PATHS) {
                        bad++;
                        continue;
                    }
                    for(cm = CMSG_FIRSTHDR(&msg); cm != NULL; cm = CMSG_NXTHDR(&msg, cm))
                        if(cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_TIMESTAMPNS)
                            memcpy(&rx, CMSG_DATA(cm), sizeof(rx));
                    if(rx.tv_sec == 0)
                        clock_gettime(CLOCK_REALTIME, &rx);
                    note_path((uint32_t *)buf, &rx);
                    if(!have_first) {
                        last_report_ns = mono_ns();
                        next_report_ns = last_report_ns + HYB_REPORT_MS * 1000000ull;
                    }
                }
                seq = ntohl(((uint32_t *)buf)[1]);
                len = (uint32_t)n - hdr_len;
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
                if(pwrite_all(fd, buf + hdr_len, len, (off_t)seq * chunk) < 0) {
                    perror("pwrite");
                    return 1;
                }
                bitmap[seq >> 3] |= (uint8_t)(1u << (seq & 7));
                received++;
                got_new = 1;
            }
            /* -a では、遅い経路から重複だけが少しずつ届き続けても NACK を遅らせない */
            if(!adaptive || got_new)
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
    if(output_commit() < 0) {
        perror("save file");
        return 1;
    }
    clock_gettime(CLOCK_REALTIME, &end_time);
    if(!have_first)
        first_time = end_time;
    if(send_msg(ctrl, MSG_DONE, sender_round, 0) < 0) {
        perror("write DONE");
        return 1;
    }
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
    if(adaptive) {
        uint32_t i;
        for(i = 0; i < HYB_MAX_PATHS; i++)
            if(rpaths[i].seen)
                printf("Path %u: received=%u lost=%u\n", i, rpaths[i].received,
                       rpaths[i].received >= rpaths[i].next
                       ? 0 : rpaths[i].next - rpaths[i].received);
    }
    printf("NACK rounds: %u (NACKした延べチャンク数 %llu)\n", nack_rounds, nacked);
    printf("Time (送信開始→保存完了): %.6f sec\n", e2e);
    if(e2e > 0)
        printf("Goodput: %.3f Mbps\n", (double)file_size * 8.0 / e2e / 1e6);
    printf("Time (最初のデータ→保存完了, 参考): %.6f sec\n", local);
    printf("CPU time (user+sys): %.6f sec\n", cpu_seconds());
    if(e2e <= 0 || e2e + 1.0 < local)
        fprintf(stderr, "warning: 送信側と時計が共有されていない可能性があります\n");
    printf("RESULT prog=hyb_recv chunk=%u bytes=%llu nack_rounds=%u nacked=%llu dup=%llu"
           " time=%.6f goodput_mbps=%.3f local_time=%.6f cpu=%.6f rcvbuf=%d adaptive=%d\n",
           chunk, (unsigned long long)file_size, nack_rounds, nacked, dup, e2e,
           e2e > 0 ? (double)file_size * 8.0 / e2e / 1e6 : 0.0, local, cpu_seconds(), rcvbuf,
           adaptive);
    return 0;
}
