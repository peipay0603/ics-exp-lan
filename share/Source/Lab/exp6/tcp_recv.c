/*  -*- coding: utf-8-unix; -*-                                     */
/*  FILENAME     :  tcp_recv.c                                      */
/*  DESCRIPTION  :  TCPでファイルを受け取る（受信側 node1）         */
/*  USAGE        :  tcp_recv.out [-b BUF_LEN] port output_file      */
/*                                                                  */
/*  1回の転送を受け取ったら終了する。                                */
/*  時間は「送信側の開始時刻（ヘッダ）→ 受信側で保存し終えた時刻」。   */
/*  同じPC上のコンテナはカーネルの時計を共有しているので、             */
/*  node3の時刻とnode1の時刻をそのまま引き算できる。                  */

#include "common.h"
#include "output_file.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-b BUF_LEN] port output_file\n"
        "  -b  1回のread()/write()の大きさ。例: 1000, 64K, 1M（既定 64K）\n",
        prog);
}

int main(int argc, char **argv)
{
    long buf_len = DEFAULT_BUF_LEN;
    int opt, port, sock0, sock, fd, r;
    int yes = 1, confirmed;
    char *buf, *filename;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t addrLen;
    struct tcp_hdr hdr;
    struct timespec start_time, accept_time, end_time;
    unsigned long long file_size, total_bytes = 0;
    unsigned long receive_count = 0;
    double e2e, local;

    while((opt = getopt(argc, argv, "b:")) != -1) {
        if(opt == 'b') {
            buf_len = parse_size(optarg);
            if(buf_len < 1 || buf_len > MAX_BUF_LEN) {
                fprintf(stderr, "invalid BUF_LEN: %s\n", optarg);
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
    buf = malloc((size_t)buf_len);
    if(buf == NULL) {
        perror("malloc");
        return 1;
    }

    /* STEP 1: 待ち受けソケットを作る */
    sock0 = socket(AF_INET, SOCK_STREAM, 0);
    if(sock0 < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(sock0, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons((unsigned short)port);
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(sock0, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("bind");
        return 1;
    }
    if(listen(sock0, 1) < 0) {
        perror("listen");
        return 1;
    }
    printf("waiting connection... port=%d BUF_LEN=%ld\n", port, buf_len);
    fflush(stdout);

    /* STEP 2: 接続を受け付ける（1回だけ） */
    addrLen = sizeof(clientAddr);
    sock = accept(sock0, (struct sockaddr *)&clientAddr, &addrLen);
    if(sock < 0) {
        perror("accept");
        return 1;
    }
    clock_gettime(CLOCK_REALTIME, &accept_time);
    close(sock0);

    /* STEP 3: ヘッダを読む */
    r = read_all(sock, &hdr, sizeof(hdr));
    if(r != 0 || (ntohl(hdr.magic) != TCP_MAGIC && ntohl(hdr.magic) != TCP_CONFIRMED_MAGIC)) {
        fprintf(stderr, "bad header（送信側は tcp_send.out ですか）\n");
        return 1;
    }
    confirmed = ntohl(hdr.magic) == TCP_CONFIRMED_MAGIC;
    file_size = be64toh(hdr.file_size);
    start_time.tv_sec = (time_t)(int64_t)be64toh((uint64_t)hdr.start_sec);
    start_time.tv_nsec = (long)(int64_t)be64toh((uint64_t)hdr.start_nsec);

    /* STEP 4: 保存先を開く */
    fd = output_open(filename);
    if(fd < 0) {
        perror(filename);
        return 1;
    }

    /* STEP 5: ファイルサイズ分だけ受け取って保存する */
    while(total_bytes < file_size) {
        unsigned long long rest = file_size - total_bytes;
        size_t want = rest < (unsigned long long)buf_len ? (size_t)rest : (size_t)buf_len;
        ssize_t n = read(sock, buf, want);
        if(n < 0) {
            if(errno == EINTR)
                continue;
            perror("read socket");
            break;
        }
        if(n == 0)
            break;      /* 予定より早く切断された */
        receive_count++;
        if(write_all(fd, buf, (size_t)n) < 0) {
            perror("write file");
            return 1;
        }
        total_bytes += (unsigned long long)n;
    }

    /* STEP 6: 保存を終え、時間とグッドプットを計算する */
    if(total_bytes == file_size && output_commit() < 0) {
        perror("save file");
        if(confirmed)
            send_receipt(sock, 1, total_bytes);
        return 1;
    }
    clock_gettime(CLOCK_REALTIME, &end_time);
    if(confirmed && send_receipt(sock, total_bytes == file_size ? 0 : 1, total_bytes) < 0) {
        perror("send receipt");
        return 1;
    }
    close(sock);
    e2e = ts_diff(&start_time, &end_time);
    local = ts_diff(&accept_time, &end_time);

    printf("From: %s:%d\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
    printf("Receive BUF_LEN: %ld (sender BUF_LEN: %u)\n", buf_len, ntohl(hdr.buf_len));
    printf("Receive bytes: %llu / %llu %s\n", total_bytes, file_size,
           total_bytes == file_size ? "OK" : "INCOMPLETE");
    printf("Receive count: %lu\n", receive_count);
    printf("Time (送信開始→保存完了): %.6f sec\n", e2e);
    if(e2e > 0)
        printf("Goodput: %.3f Mbps\n", (double)total_bytes * 8.0 / e2e / 1e6);
    printf("Time (accept→保存完了, 参考): %.6f sec\n", local);
    printf("CPU time (user+sys): %.6f sec\n", cpu_seconds());
    if(e2e <= 0 || e2e + 1.0 < local)
        fprintf(stderr, "warning: 送信側と時計が共有されていない可能性があります\n");
    printf("RESULT prog=tcp_recv buf_len=%ld bytes=%llu expected=%llu reads=%lu"
           " time=%.6f goodput_mbps=%.3f local_time=%.6f cpu=%.6f confirmed=%d\n",
           buf_len, total_bytes, file_size, receive_count, e2e,
           e2e > 0 ? (double)total_bytes * 8.0 / e2e / 1e6 : 0.0,
           local, cpu_seconds(), confirmed);
    free(buf);
    return total_bytes == file_size ? 0 : 2;
}
