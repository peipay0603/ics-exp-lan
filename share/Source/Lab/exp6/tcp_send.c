/*  -*- coding: utf-8-unix; -*-                                     */
/*  FILENAME     :  tcp_send.c                                      */
/*  DESCRIPTION  :  TCPでファイルを送る（送信側 node3）             */
/*  USAGE        :  tcp_send.out [-b BUF_LEN] host port file        */
/*                                                                  */
/*  host:port には受信側(tcp_recv.out)か、最初の中継ノードを指定する。 */
/*  実験4-3との違い:                                                  */
/*   - BUF_LENを -b で実行時に変えられる（再コンパイル不要）          */
/*   - 送信側から接続して送る（中継 relay <待受> <次IP> <次port> と同じ向き） */
/*   - 先頭にヘッダ（ファイルサイズ・開始時刻）を付け、受信側で完了を確認する */
/*   - 転送中は何も表示しない                                          */

#include "common.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-b BUF_LEN] host port file\n"
        "  -b  1回のread()/write()の大きさ。例: 1000, 64K, 1M（既定 64K）\n",
        prog);
}

int main(int argc, char **argv)
{
    long buf_len = DEFAULT_BUF_LEN;
    int opt, port, sock, fd;
    char *buf, *host, *filename;
    struct sockaddr_in serverAddr;
    struct stat st;
    struct tcp_hdr hdr;
    struct timespec start_time, end_time;
    unsigned long long total_bytes = 0;
    unsigned long file_read_count = 0, send_count = 0;
    ssize_t n;
    double elapsed;

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
    if(argc - optind != 3) {
        usage(argv[0]);
        return 1;
    }
    host = argv[optind];
    port = parse_port(argv[optind + 1]);
    filename = argv[optind + 2];
    if(port < 0) {
        fprintf(stderr, "invalid port\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);   /* 相手が先に閉じてもwrite()のエラーで止まるように */

    /* STEP 1: 送るファイルを開き、大きさを調べる */
    fd = open(filename, O_RDONLY);
    if(fd < 0 || fstat(fd, &st) < 0) {
        perror(filename);
        return 1;
    }
    buf = malloc((size_t)buf_len);
    if(buf == NULL) {
        perror("malloc");
        return 1;
    }
    if(make_addr(host, port, &serverAddr) < 0)
        return 1;

    /* STEP 2: 計測開始。接続確立（中継を含む）から測る */
    clock_gettime(CLOCK_REALTIME, &start_time);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0) {
        perror("socket");
        return 1;
    }
    if(connect(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("connect");
        return 1;
    }

    /* STEP 3: ヘッダを送る（ファイルサイズと開始時刻） */
    hdr.magic = htonl(TCP_MAGIC);
    hdr.buf_len = htonl((uint32_t)buf_len);
    hdr.file_size = htobe64((uint64_t)st.st_size);
    hdr.start_sec = (int64_t)htobe64((uint64_t)start_time.tv_sec);
    hdr.start_nsec = (int64_t)htobe64((uint64_t)start_time.tv_nsec);
    if(write_all(sock, &hdr, sizeof(hdr)) < 0) {
        perror("write header");
        return 1;
    }

    /* STEP 4: BUF_LENずつファイルを読み、ソケットへ書く */
    while((n = read(fd, buf, (size_t)buf_len)) > 0) {
        ssize_t sent = 0;
        file_read_count++;
        while(sent < n) {
            ssize_t count = write(sock, buf + sent, (size_t)(n - sent));
            if(count < 0) {
                if(errno == EINTR)
                    continue;
                perror("write socket");
                return 1;
            }
            sent += count;
            send_count++;
        }
        total_bytes += (unsigned long long)n;
    }
    if(n < 0) {
        perror("read file");
        return 1;
    }

    /* STEP 5: 閉じる（未送信分はOSがこの後も送り続ける） */
    close(fd);
    if(close(sock) < 0)
        perror("close socket");
    clock_gettime(CLOCK_REALTIME, &end_time);
    elapsed = ts_diff(&start_time, &end_time);

    if(total_bytes != (unsigned long long)st.st_size)
        fprintf(stderr, "warning: file size changed while sending\n");
    printf("Send BUF_LEN: %ld\n", buf_len);
    printf("Send bytes: %llu\n", total_bytes);
    printf("File read count: %lu\n", file_read_count);
    printf("Send count: %lu\n", send_count);
    printf("Time (送信側でwriteし終えるまで・参考値): %.6f sec\n", elapsed);
    printf("CPU time (user+sys): %.6f sec\n", cpu_seconds());
    printf("RESULT prog=tcp_send buf_len=%ld bytes=%llu reads=%lu writes=%lu"
           " time=%.6f cpu=%.6f\n",
           buf_len, total_bytes, file_read_count, send_count,
           elapsed, cpu_seconds());
    free(buf);
    return 0;
}
