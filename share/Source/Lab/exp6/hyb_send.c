/*  -*- coding: utf-8-unix; -*-                                     */
/*  FILENAME     :  hyb_send.c                                      */
/*  DESCRIPTION  :  UDP（データ）+ TCP（制御）でファイルを送る      */
/*                  送信側 node3                                    */
/*  USAGE        :  hyb_send.out [-c chunk] [-b BUF_LEN] [-m rounds]  */
/*                      ctrl_host ctrl_port file dest [dest ...]    */
/*                  dest = IP:PORT@Mbps                             */
/*                                                                  */
/*  しくみ                                                          */
/*   1. 制御用TCPで受信側へ接続し、ファイルサイズ等(hello)を送る     */
/*   2. ファイルを chunk バイトずつに分け、番号(seq)を付けてUDPで送る */
/*      TCPのように相手の応答を待たず、指定したレート(Mbps)で送り続ける */
/*   3. 全部送ったら TCP で ROUND_END を送る                         */
/*   4. 受信側は TCP で「未着の番号の範囲(NACK)」か「完了(DONE)」を返す */
/*   5. NACKなら、その番号だけUDPで再送して 3 へ戻る                 */
/*  dest を複数書くと、各宛先のレートに比例してチャンクを振り分ける。   */
/*                                                                  */
/*  例: データは 3→4→5→1（udp_relay経由）、制御は直接回線 3→1      */
/*   ./hyb_send.out 172.21.0.10 20000 test.dat 172.27.0.40:20000@48 */

#include "common.h"
#include <sys/uio.h>
#include <sys/prctl.h>

#define MAX_PATHS   8
#define BURST_NS    2000000ull  /* 遅れを取り戻すための連続送信は2ms分まで */

struct path {
    char spec[64];              /* 表示用 "IP:PORT@Mbps"            */
    int sock;                   /* connect()済みのUDPソケット        */
    double mbps;                /* 回線上の送信レート                */
    uint64_t next_ns;           /* 次に送ってよい時刻（MONOTONIC）   */
    unsigned long long sent;
    unsigned long refused;      /* 宛先ポートが閉じていた回数        */
};

static struct path paths[MAX_PATHS];
static int npaths;

static int file_fd;
static uint64_t file_size;
static uint32_t chunk = HYB_DEFAULT_CHUNK;
static uint32_t total_chunks, session;
static uint32_t chunks_per_block;  /* 1回のpread()で読むチャンク数 = BUF_LEN / chunk */
static char *block_buf;
static long cached_block = -1;
static unsigned long file_read_count;

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-c chunk] [-b BUF_LEN] [-m max_rounds] ctrl_host ctrl_port file dest [dest ...]\n"
        "  dest  UDPデータの宛先と送信レート IP:PORT@Mbps（例 172.27.0.40:20000@48）\n"
        "  -c    1データグラムのデータ長（既定 %d = MTU1500で断片化しない最大）\n"
        "  -b    ファイルを1回に読む大きさ（既定 64K）\n"
        "  -m    再送を含めた最大回数（既定 100）\n",
        prog, HYB_DEFAULT_CHUNK);
}

/* "IP:PORT@Mbps" を解釈して、connect()したUDPソケットを作る */
static int open_path(const char *spec, struct path *p)
{
    char tmp[64], *colon, *at, *end;
    struct sockaddr_in addr;
    int port;

    if(strlen(spec) >= sizeof(tmp))
        return -1;
    strcpy(tmp, spec);
    strcpy(p->spec, spec);
    at = strchr(tmp, '@');
    colon = strchr(tmp, ':');
    if(at == NULL || colon == NULL || colon > at)
        return -1;
    *at = '\0';
    *colon = '\0';
    port = parse_port(colon + 1);
    p->mbps = strtod(at + 1, &end);
    if(port < 0 || *end != '\0' || p->mbps < 0.1 || p->mbps > 10000)
        return -1;
    if(make_addr(tmp, port, &addr) < 0)
        return -1;
    p->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(p->sock < 0 || connect(p->sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("udp socket");
        return -1;
    }
    return 0;
}

/* offset から len バイトをファイルから読む */
static int pread_all(int fd, char *buf, size_t len, off_t offset)
{
    while(len > 0) {
        ssize_t n = pread(fd, buf, len, offset);
        if(n < 0 && errno == EINTR)
            continue;
        if(n <= 0)
            return -1;
        buf += n;
        len -= (size_t)n;
        offset += n;
    }
    return 0;
}

/* seq番目のチャンクの先頭を返す。BUF_LEN分をまとめて読み、使い回す */
static const char *chunk_data(uint32_t seq, uint32_t *len)
{
    long block = (long)(seq / chunks_per_block);
    uint64_t block_off = (uint64_t)block * chunks_per_block * chunk;
    uint64_t off = (uint64_t)seq * chunk;

    if(block != cached_block) {
        uint64_t want = (uint64_t)chunks_per_block * chunk;
        if(block_off + want > file_size)
            want = file_size - block_off;
        if(pread_all(file_fd, block_buf, (size_t)want, (off_t)block_off) < 0) {
            perror("read file");
            return NULL;
        }
        cached_block = block;
        file_read_count++;
    }
    *len = (off + chunk > file_size) ? (uint32_t)(file_size - off) : chunk;
    return block_buf + (off - block_off);
}

/* 次に送る経路を選び、送ってよい時刻まで待つ。
   「次に送ってよい時刻」が一番早い経路を選ぶので、レートに比例して振り分けられる */
static struct path *wait_next_path(void)
{
    struct path *p = &paths[0];
    uint64_t now;
    int i;

    for(i = 1; i < npaths; i++)
        if(paths[i].next_ns < p->next_ns)
            p = &paths[i];
    now = mono_ns();
    if(p->next_ns > now) {
        struct timespec ts;
        ts.tv_sec = (time_t)(p->next_ns / 1000000000ull);
        ts.tv_nsec = (long)(p->next_ns % 1000000000ull);
        while(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
            ;
    } else if(now - p->next_ns > BURST_NS) {
        /* 大きく遅れたとき、取り戻そうとして一度に大量に送らない */
        p->next_ns = now - BURST_NS;
    }
    return p;
}

/* seq番目のチャンクを1データグラムとして送る */
static int send_chunk(uint32_t seq)
{
    struct path *p = wait_next_path();
    uint32_t len, hdr[2];
    const char *data = chunk_data(seq, &len);
    struct iovec iov[2];

    if(data == NULL)
        return -1;
    hdr[0] = htonl(session);
    hdr[1] = htonl(seq);
    iov[0].iov_base = hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = (void *)data;
    iov[1].iov_len = len;
    for(;;) {
        /* ヘッダとデータを1つのデータグラムとして送る（コピー不要） */
        if(writev(p->sock, iov, 2) >= 0)
            break;
        if(errno == EINTR)
            continue;
        if(errno == ECONNREFUSED) {
            /* 前に送ったデータグラムに対しICMP port unreachableが返ってきた。
               このデータは失われるが、後でNACKにより再送される */
            if(p->refused++ == 0)
                fprintf(stderr, "warning: %s のUDPポートが閉じています（中継・受信側は起動していますか）\n",
                        p->spec);
            break;
        }
        if(errno == ENOBUFS || errno == EAGAIN) {
            usleep(100);
            continue;
        }
        perror("send udp");
        return -1;
    }
    p->sent++;
    /* 送ったビット数 / レート だけ次の送信を遅らせる（ns = bit * 1000 / Mbps） */
    p->next_ns += (uint64_t)((double)(len + HYB_WIRE_OVERHEAD) * 8.0 * 1000.0 / p->mbps);
    return 0;
}

int main(int argc, char **argv)
{
    long buf_len = DEFAULT_BUF_LEN, v;
    unsigned long max_rounds = 100;
    int opt, ctrl_port, ctrl, i;
    char *ctrl_host, *filename;
    struct sockaddr_in ctrlAddr;
    struct stat st;
    struct hyb_hello hello;
    struct hyb_msg m;
    struct hyb_range *ranges;
    uint32_t nranges, round, k;
    uint64_t chunks64;
    struct timespec start_time, end_time;
    unsigned long long total_sent = 0, retransmitted = 0;
    int yes = 1;
    double elapsed;

    while((opt = getopt(argc, argv, "c:b:m:")) != -1) {
        switch(opt) {
        case 'c':
            v = parse_size(optarg);
            if(v < 1 || v > HYB_MAX_CHUNK) {
                fprintf(stderr, "invalid chunk: %s\n", optarg);
                return 1;
            }
            chunk = (uint32_t)v;
            break;
        case 'b':
            buf_len = parse_size(optarg);
            if(buf_len < 1 || buf_len > MAX_BUF_LEN) {
                fprintf(stderr, "invalid BUF_LEN: %s\n", optarg);
                return 1;
            }
            break;
        case 'm':
            max_rounds = strtoul(optarg, NULL, 10);
            if(max_rounds < 1) {
                fprintf(stderr, "invalid max_rounds\n");
                return 1;
            }
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if(argc - optind < 4 || argc - optind - 3 > MAX_PATHS) {
        usage(argv[0]);
        return 1;
    }
    ctrl_host = argv[optind];
    ctrl_port = parse_port(argv[optind + 1]);
    filename = argv[optind + 2];
    if(ctrl_port < 0) {
        fprintf(stderr, "invalid ctrl_port\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    /* usleep等の起床の遅れを小さくし、送信間隔を正確にする */
    prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);

    /* STEP 1: ファイルを開き、チャンク数を求める */
    file_fd = open(filename, O_RDONLY);
    if(file_fd < 0 || fstat(file_fd, &st) < 0) {
        perror(filename);
        return 1;
    }
    file_size = (uint64_t)st.st_size;
    chunks64 = (file_size + chunk - 1) / chunk;
    if(chunks64 > UINT32_MAX) {
        fprintf(stderr, "file too large for chunk size\n");
        return 1;
    }
    total_chunks = (uint32_t)chunks64;
    chunks_per_block = (uint32_t)(buf_len / chunk);
    if(chunks_per_block == 0)
        chunks_per_block = 1;
    block_buf = malloc((size_t)chunks_per_block * chunk);
    ranges = malloc(sizeof(struct hyb_range) * HYB_MAX_RANGES);
    if(block_buf == NULL || ranges == NULL) {
        perror("malloc");
        return 1;
    }

    /* STEP 2: UDPの宛先（経路）を用意する */
    for(i = optind + 3; i < argc; i++) {
        if(open_path(argv[i], &paths[npaths]) < 0) {
            fprintf(stderr, "invalid dest: %s（IP:PORT@Mbps の形で指定）\n", argv[i]);
            return 1;
        }
        npaths++;
    }
    if(make_addr(ctrl_host, ctrl_port, &ctrlAddr) < 0)
        return 1;

    /* STEP 3: 計測開始。制御用TCPで接続し、helloを送る */
    clock_gettime(CLOCK_REALTIME, &start_time);
    session = (uint32_t)getpid() ^ (uint32_t)start_time.tv_nsec ^ (uint32_t)start_time.tv_sec;
    ctrl = socket(AF_INET, SOCK_STREAM, 0);
    if(ctrl < 0 || connect(ctrl, (struct sockaddr *)&ctrlAddr, sizeof(ctrlAddr)) < 0) {
        perror("connect ctrl");
        return 1;
    }
    /* 制御メッセージは小さいので、まとめずにすぐ送る */
    setsockopt(ctrl, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    hello.magic = htonl(HYB_MAGIC);
    hello.chunk = htonl(chunk);
    hello.file_size = htobe64(file_size);
    hello.total_chunks = htonl(total_chunks);
    hello.session = htonl(session);
    hello.start_sec = (int64_t)htobe64((uint64_t)start_time.tv_sec);
    hello.start_nsec = (int64_t)htobe64((uint64_t)start_time.tv_nsec);
    if(write_all(ctrl, &hello, sizeof(hello)) < 0) {
        perror("write hello");
        return 1;
    }
    if(recv_msg(ctrl, &m) != 0 || m.type != MSG_READY) {
        fprintf(stderr, "receiver did not send READY（受信側は hyb_recv.out ですか）\n");
        return 1;
    }

    /* STEP 4: 1回目は全チャンク、2回目以降はNACKで言われたチャンクだけ送る */
    ranges[0].start = 0;
    ranges[0].len = total_chunks;
    nranges = total_chunks > 0 ? 1 : 0;
    for(round = 1; ; round++) {
        uint32_t count = 0, r;
        uint64_t now;

        if(round > max_rounds) {
            fprintf(stderr, "too many rounds (%lu)。UDPの宛先・中継・レートを確認してください\n",
                    max_rounds);
            return 1;
        }
        now = mono_ns();
        for(i = 0; i < npaths; i++)
            paths[i].next_ns = now;
        for(r = 0; r < nranges; r++) {
            for(k = 0; k < ranges[r].len; k++) {
                if(send_chunk(ranges[r].start + k) < 0)
                    return 1;
            }
            count += ranges[r].len;
        }
        total_sent += count;
        if(round > 1)
            retransmitted += count;

        /* STEP 5: 送り終えたことを伝え、受信側の返事を待つ */
        if(send_msg(ctrl, MSG_ROUND_END, round, count) < 0) {
            perror("write ROUND_END");
            return 1;
        }
        if(recv_msg(ctrl, &m) != 0) {
            fprintf(stderr, "control connection closed\n");
            return 1;
        }
        if(m.type == MSG_DONE)
            break;
        if(m.type != MSG_NACK || m.count == 0 || m.count > HYB_MAX_RANGES) {
            fprintf(stderr, "unexpected control message type=%u\n", m.type);
            return 1;
        }
        nranges = m.count;
        if(read_all(ctrl, ranges, sizeof(struct hyb_range) * nranges) != 0) {
            fprintf(stderr, "failed to read NACK ranges\n");
            return 1;
        }
        {
            unsigned long long missing = 0;
            for(r = 0; r < nranges; r++) {
                ranges[r].start = ntohl(ranges[r].start);
                ranges[r].len = ntohl(ranges[r].len);
                if((uint64_t)ranges[r].start + ranges[r].len > total_chunks) {
                    fprintf(stderr, "bad NACK range\n");
                    return 1;
                }
                missing += ranges[r].len;
            }
            printf("round %u: sent %u, missing %llu (%.3f%%) -> resend\n",
                   round, count, missing, count ? 100.0 * (double)missing / count : 0.0);
        }
    }

    /* STEP 6: 完了（DONE受信）までの時間 */
    clock_gettime(CLOCK_REALTIME, &end_time);
    close(ctrl);
    elapsed = ts_diff(&start_time, &end_time);

    printf("File: %llu bytes, chunk %u bytes x %u, BUF_LEN(file read) %u bytes\n",
           (unsigned long long)file_size, chunk, total_chunks, chunks_per_block * chunk);
    for(i = 0; i < npaths; i++)
        printf("Path %d: %s sent=%llu%s\n", i, paths[i].spec, paths[i].sent,
               paths[i].refused ? " (port unreachable あり)" : "");
    printf("Rounds: %u, datagrams sent: %llu (retransmitted %llu)\n",
           round, total_sent, retransmitted);
    printf("File read count: %lu\n", file_read_count);
    printf("Time (送信開始→DONE受信): %.6f sec\n", elapsed);
    printf("Goodput: %.3f Mbps\n", (double)file_size * 8.0 / elapsed / 1e6);
    printf("CPU time (user+sys): %.6f sec\n", cpu_seconds());
    printf("RESULT prog=hyb_send chunk=%u bytes=%llu rounds=%u sent=%llu retrans=%llu"
           " time=%.6f goodput_mbps=%.3f cpu=%.6f\n",
           chunk, (unsigned long long)file_size, round, total_sent, retransmitted,
           elapsed, (double)file_size * 8.0 / elapsed / 1e6, cpu_seconds());
    return 0;
}
