/*  -*- coding: utf-8-unix; -*-                                     */
/*  FILENAME     :  hyb_send.c                                      */
/*  DESCRIPTION  :  UDP（データ）+ TCP（制御）でファイルを送る      */
/*                  送信側 node3                                    */
/*  USAGE        :  hyb_send.out [-a] [-c chunk] [-b BUF_LEN] [-m rounds] */
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
/*  -a（経路の状況に合わせる）                                        */
/*   dest のレートは「最初のレート」になる。受信側が0.1秒ごとに返す   */
/*   経路ごとの受信状況(MSG_STATS)を見て、経路ごとにレートを変える。   */
/*   - 待ち行列が空いている経路は速くする（×1.25）                    */
/*   - 待ち行列がたまり始めた・落ち始めた経路は、届いた速さの0.85倍に下げる */
/*   - 急に遅くなり、送った分が1秒以上たまった経路は一時停止し、       */
/*     たまっていたチャンクを他の経路で送り直す                         */
/*   結果として、そのとき速い経路ほど多く送ることになる。               */
/*                                                                  */
/*  例: データは 3→4→5→1（udp_relay経由）、制御は直接回線 3→1      */
/*   ./hyb_send.out 172.21.0.10 20000 test.dat 172.27.0.40:20000@48 */

#include "common.h"
#include <sys/uio.h>
#include <sys/prctl.h>
#include <math.h>
#include <poll.h>

#define MAX_PATHS   8
#define BURST_NS    2000000ull  /* 遅れを取り戻すための連続送信は2ms分まで */

/* -a で使う値 */
#define ADAPT_MIN_MBPS          0.5     /* 遅い経路にも様子見でこれだけは送る      */
#define ADAPT_MAX_MBPS          1000.0
#define ADAPT_Q_LOW_US          5000    /* 待ち行列がこれ未満なら空いている         */
#define ADAPT_Q_HIGH_US         20000   /* これを超えたら混んでいる                 */
#define ADAPT_PAUSE_BACKLOG_S   1.0     /* 未着分を届けるのにこれ以上かかるなら止める */
#define ADAPT_STALL_NS          600000000ull    /* 未着があるのに何も届かない時間   */
#define ADAPT_PAUSE_MAX_NS      3000000000ull   /* 止めた経路も、この後また試す     */
#define ADAPT_BASE_WINDOW_NS    5000000000ull   /* 遅延の基準は直近5〜10秒の最小値  */
#define RING                    65536   /* 経路内の通し番号 → チャンク番号の記録    */
#define RESCUE_CAP              (RING * MAX_PATHS)

struct path {
    char spec[64];              /* 表示用 "IP:PORT@Mbps"            */
    int sock;                   /* connect()済みのUDPソケット        */
    double mbps;                /* 回線上の送信レート                */
    uint64_t next_ns;           /* 次に送ってよい時刻（MONOTONIC）   */
    unsigned long long sent;
    unsigned long refused;      /* 宛先ポートが閉じていた回数        */
    /* ここから -a 用 */
    uint32_t counter;           /* 次に付ける経路内の通し番号                   */
    uint32_t *ring;             /* 通し番号 → seq（止めたときに送り直すため） */
    uint32_t *tsent;            /* 通し番号 → 送った時刻 [µs]                  */
    uint32_t received, passed;  /* 受信側から最後に届いた値                     */
    uint32_t resolved;          /* これより前の通し番号は、送り直し済み         */
    int paused;
    unsigned clean;             /* 空いている報告が続いた回数（続くほど速く上げる） */
    unsigned high_streak;       /* 遅延が大きいまま変わらない報告が続いた回数   */
    int32_t base_cur, base_prev;/* 遅延の基準（5秒ごとに入れ替える最小値）      */
    int32_t prev_q;             /* 前回の待ち行列の長さ [µs]（-1 は不明）       */
    uint64_t base_at, paused_at, progress_at;
    double ivl_bits;            /* 前回のSTATSのあとに送ったビット数            */
    double delivered;           /* 直近に届いた速さ [Mbps]                      */
    unsigned long long rescued; /* この経路から他の経路へ送り直したチャンク数   */
    unsigned long local_full;   /* 手元の送信待ちがいっぱいで送れなかった回数   */
    unsigned pauses;
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

static int ctrl;
static int adaptive;
static uint32_t hdr_len = HYB_DGRAM_HDR;
static uint64_t start_ns, last_print_ns;

/* 送る順番：送り直しキュー → 今の回の範囲 */
static uint32_t *rescue_q;
static size_t rescue_head, rescue_count;
static unsigned long long rescued_total;
static struct hyb_range *ranges;
static uint32_t nranges, cur_r, cur_k;

/* 回の進み具合 */
static uint32_t round_no = 1, round_count;
static int round_end_sent, done;
static unsigned long max_rounds = 100;
static unsigned long long total_sent, retransmitted;

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-a] [-c chunk] [-b BUF_LEN] [-m max_rounds] ctrl_host ctrl_port file dest [dest ...]\n"
        "  dest  UDPデータの宛先と送信レート IP:PORT@Mbps（例 172.27.0.40:20000@48）\n"
        "  -a    経路ごとの受信状況を見てレートを変える。dest のレートは最初の値になる\n"
        "  -c    1データグラムのデータ長（既定 %d、-a のとき %d = MTU1500で断片化しない最大）\n"
        "  -b    ファイルを1回に読む大きさ（既定 64K）\n"
        "  -m    再送を含めた最大回数（既定 100）\n",
        prog, HYB_DEFAULT_CHUNK, HYB_ADAPT_CHUNK);
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
    if(port < 0 || end == at + 1 || *end != '\0' || !isfinite(p->mbps)
       || p->mbps < 0.1 || p->mbps > 10000)
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

/* 1データグラムの回線上のビット数（データ＋ヘッダ＋UDP・IP・Ethernet） */
static double wire_bits(uint32_t len)
{
    return (double)(len + hdr_len + 8 + 20 + 14) * 8.0;
}

static double elapsed_s(uint64_t now)
{
    return (double)(now - start_ns) / 1e9;
}

/*-------------------------- 送る順番 ------------------------------*/

/* 次に送るチャンク番号を見る（まだ取り出さない）。1: 送り直し、2: 今の回、0: なし */
static int peek_seq(uint32_t *seq)
{
    if(rescue_count > 0) {
        *seq = rescue_q[rescue_head];
        return 1;
    }
    while(cur_r < nranges && cur_k >= ranges[cur_r].len) {
        cur_r++;
        cur_k = 0;
    }
    if(cur_r < nranges) {
        *seq = ranges[cur_r].start + cur_k;
        return 2;
    }
    return 0;
}

static void consume_seq(int source)
{
    if(source == 1) {
        rescue_head = (rescue_head + 1) % RESCUE_CAP;
        rescue_count--;
    } else {
        cur_k++;
    }
}

static void rescue_push(uint32_t seq)
{
    if(rescue_count < RESCUE_CAP) {     /* あふれた分は最後のNACKで送り直される */
        rescue_q[(rescue_head + rescue_count) % RESCUE_CAP] = seq;
        rescue_count++;
    }
}

/*-------------------------- 送信 ----------------------------------*/

/* 次に送る経路を選ぶ。「次に送ってよい時刻」が一番早い経路なので、レートに比例して振り分けられる。
   -a で一時停止中の経路は選ばない */
static struct path *pick_path(void)
{
    struct path *p = NULL;
    uint64_t now;
    int i;

    for(i = 0; i < npaths; i++) {
        if(paths[i].paused)
            continue;
        if(p == NULL || paths[i].next_ns < p->next_ns)
            p = &paths[i];
    }
    if(p == NULL)
        p = &paths[0];
    now = mono_ns();
    if(p->next_ns < now && now - p->next_ns > BURST_NS) {
        /* 大きく遅れたとき、取り戻そうとして一度に大量に送らない */
        p->next_ns = now - BURST_NS;
    }
    return p;
}

/* deadline まで待つ。その間に制御メッセージが届いたら 1、時刻になったら 0 */
static int wait_ctrl(uint64_t deadline)
{
    for(;;) {
        struct pollfd pfd;
        struct timespec ts, *timeout = NULL;
        uint64_t now = mono_ns();
        int r;

        if(deadline != UINT64_MAX) {
            if(now >= deadline)
                return 0;
            ts.tv_sec = (time_t)((deadline - now) / 1000000000ull);
            ts.tv_nsec = (long)((deadline - now) % 1000000000ull);
            timeout = &ts;
        }
        pfd.fd = ctrl;
        pfd.events = POLLIN;
        r = ppoll(&pfd, 1, timeout, NULL);
        if(r < 0 && errno == EINTR)
            continue;
        if(r < 0)
            return -1;
        return r > 0;
    }
}

/* seq番目のチャンクを、経路 p で1データグラムとして送る。
   成功 0、エラー -1。-a で手元の送信待ちがいっぱいなら送らずに 1（別の経路で送る） */
static int send_chunk(struct path *p, uint32_t seq)
{
    uint32_t len, hdr[4];
    const char *data = chunk_data(seq, &len);
    struct iovec iov[2];

    if(data == NULL)
        return -1;
    hdr[0] = htonl(session);
    hdr[1] = htonl(seq);
    if(adaptive) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        hdr[2] = htonl(((uint32_t)(p - paths) << 24) | (p->counter & 0xFFFFFF));
        hdr[3] = htonl((uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000));
    }
    iov[0].iov_base = hdr;
    iov[0].iov_len = hdr_len;
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
        if(errno == ENOBUFS || errno == EAGAIN || errno == EWOULDBLOCK) {
            if(adaptive) {
                /* この経路の手元の回線が、送ろうとしている速さに追いつかない。
                   ここで待つと他の経路も送れなくなるので、レートを下げて別の経路に回す */
                p->next_ns = mono_ns() + 1000000ull;
                p->mbps = p->mbps * 0.9 > ADAPT_MIN_MBPS ? p->mbps * 0.9 : ADAPT_MIN_MBPS;
                p->clean = 0;
                p->local_full++;
                return 1;
            }
            usleep(100);
            continue;
        }
        perror("send udp");
        return -1;
    }
    p->sent++;
    if(adaptive) {
        p->ring[p->counter % RING] = seq;
        p->tsent[p->counter % RING] = ntohl(hdr[3]);
        p->counter++;
        p->ivl_bits += wire_bits(len);
    }
    /* 送ったビット数 / レート だけ次の送信を遅らせる（ns = bit * 1000 / Mbps） */
    p->next_ns += (uint64_t)(wire_bits(len) * 1000.0 / p->mbps);
    return 0;
}

/*-------------------------- -a: レートの調整 ----------------------*/

static int active_paths(void)
{
    int i, n = 0;
    for(i = 0; i < npaths; i++)
        n += !paths[i].paused;
    return n;
}

/* 経路を一時停止し、まだ届いていないチャンクを他の経路で送り直す */
static void pause_path(struct path *p, uint32_t from, uint64_t now, const char *why)
{
    uint32_t c, n = p->counter - from;
    if(n > RING) {              /* 記録が残っていない古い分は、最後のNACKで送り直される */
        from = p->counter - RING;
        n = RING;
    }
    for(c = from; c != p->counter; c++)
        rescue_push(p->ring[c % RING]);
    p->rescued += n;
    rescued_total += n;
    p->resolved = p->counter;
    p->paused = 1;
    p->paused_at = now;
    p->pauses++;
    printf("[%6.2fs] path %d %s: 一時停止（%s）。未着 %u 個を他の経路で送り直す\n",
           elapsed_s(now), (int)(p - paths), p->spec, why, n);
}

/* 受信側から届いた、ある経路の状況でレートを決める。
   混んでいるかどうかは次の3つで判断する。
   - 落ちた：経路内の通し番号が5%以上抜けた
   - 詰まり始めた：同じデータグラムの、届いた間隔が送った間隔より15%以上長い
                   （途中の回線が送った速さに追いつかず、待ち行列が伸びている）
   - 待ち行列が長く、さらに伸びている：片道遅延が基準より20ms以上大きく、前回より増えた */
static void adapt(struct path *p, uint32_t received, uint32_t passed, int32_t owd, uint32_t span_us,
                  double dt, uint64_t now)
{
    uint32_t got = received - p->received, moved = passed - p->passed;
    uint32_t lost = moved > got ? moved - got : 0;
    uint32_t from = (int32_t)(passed - p->resolved) > 0 ? passed : p->resolved;
    uint32_t outstanding = p->counter - from;
    double bits = wire_bits(chunk), offered = p->ivl_bits / dt / 1e6;
    int32_t q = -1;
    int growing = 0, lossy = lost >= 2 && lost * 20 > moved;

    /* 送った間隔と届いた間隔を、同じデータグラムで比べる（ビット/µs = Mbps） */
    if(moved >= 8 && got >= 8 && span_us > 0 && p->counter - p->passed <= RING) {
        uint32_t send_span = p->tsent[(passed - 1) % RING] - p->tsent[p->passed % RING];
        if(send_span > 0 && send_span < 10000000u) {
            double sent_rate = (moved - 1) * bits / send_span;
            double recv_rate = (got - 1) * bits / span_us;
            growing = recv_rate < 0.85 * sent_rate;
        }
    }
    if(owd != HYB_NO_SAMPLE) {
        if(now - p->base_at >= ADAPT_BASE_WINDOW_NS) {
            p->base_prev = p->base_cur;
            p->base_cur = INT32_MAX;
            p->base_at = now;
        }
        if(owd < p->base_cur)
            p->base_cur = owd;
        q = owd - (p->base_cur < p->base_prev ? p->base_cur : p->base_prev);
        if(q > ADAPT_Q_HIGH_US && p->prev_q >= 0 && q > p->prev_q + 1000)
            growing = 1;
        /* 遅延が基準より大きいまま増えも減りもせず、落ちてもいない：
           経路の遅延そのものが変わった（回線の設定が変わった）とみて基準を更新する。
           これがないと、遅延が増えた経路を「少し混んでいる」と見て、速くなっていても上げ方が遅い */
        if(q >= ADAPT_Q_LOW_US && !growing && !lossy && got >= 4) {
            if(++p->high_streak >= 5) {
                p->base_cur = p->base_prev = owd;
                p->high_streak = 0;
                q = 0;
                printf("[%6.2fs] path %d %s: 遅延の基準を更新\n",
                       elapsed_s(now), (int)(p - paths), p->spec);
            }
        } else {
            p->high_streak = 0;
        }
    }
    p->prev_q = q;

    p->received = received;
    p->passed = passed;
    p->ivl_bits = 0;
    p->delivered = got * bits / dt / 1e6;
    if(moved > 0 || outstanding == 0)
        p->progress_at = now;

    if(p->paused) {
        /* 止める前に送った分がすべて片付いたか、しばらくたったら、少しずつ試す */
        if((int32_t)(passed - p->resolved) >= 0 || now - p->paused_at >= ADAPT_PAUSE_MAX_NS) {
            p->paused = 0;
            p->mbps = ADAPT_MIN_MBPS;
            p->next_ns = now;
            p->progress_at = now;
            printf("[%6.2fs] path %d %s: 再開（%.1f Mbps から試す）\n",
                   elapsed_s(now), (int)(p - paths), p->spec, p->mbps);
        }
        return;
    }
    if(outstanding > 0 && active_paths() > 1) {
        double backlog_s = p->delivered > 0 ? outstanding * bits / (p->delivered * 1e6) : 1e9;
        if(now - p->progress_at > ADAPT_STALL_NS) {
            pause_path(p, from, now, "何も届かない");
            return;
        }
        if(moved > 0 && backlog_s > ADAPT_PAUSE_BACKLOG_S) {
            char why[64];
            snprintf(why, sizeof(why), "未着分を届けるのに %.1f 秒かかる", backlog_s);
            pause_path(p, from, now, why);
            return;
        }
    }
    if(growing || lossy) {
        /* 混んでいる：今届いている速さより少し下げ、たまった分を減らす */
        double target = p->delivered * 0.85;
        p->mbps = target > ADAPT_MIN_MBPS ? target : ADAPT_MIN_MBPS;
        p->clean = 0;
    } else if(q >= 0 && q <= ADAPT_Q_HIGH_US && offered >= 0.7 * p->mbps) {
        /* 空いている：実際にこのレートで送れていたときだけ上げる。
           空いている報告が3回続いたら、回線が速くなったとみて1.5倍ずつ上げる */
        if(q < ADAPT_Q_LOW_US) {
            p->clean++;
            p->mbps *= p->clean >= 3 ? 1.5 : 1.25;
        } else {
            p->mbps *= 1.05;
        }
        if(p->mbps > ADAPT_MAX_MBPS)
            p->mbps = ADAPT_MAX_MBPS;
    }
}

/* 1秒ごとに、経路ごとの「送信レート(届いた速さ)」を表示する */
static void print_rates(uint64_t now)
{
    double total = 0;
    int i;
    printf("[%6.2fs]", elapsed_s(now));
    for(i = 0; i < npaths; i++) {
        if(paths[i].paused)
            printf(" P%d 停止", i);
        else
            printf(" P%d %.1f(%.1f)", i, paths[i].mbps, paths[i].delivered);
        total += paths[i].delivered;
    }
    printf("  届いた合計 %.1f Mbps\n", total);
    fflush(stdout);
}

static int on_stats(const struct hyb_msg *m)
{
    struct hyb_pstat st[HYB_MAX_PATHS];
    int found[MAX_PATHS] = {0};
    double dt = ntohl(m->reserved) / 1e6;
    uint64_t now = mono_ns();
    uint32_t i;

    if(!adaptive || m->count > HYB_MAX_PATHS
       || read_all(ctrl, st, sizeof(st[0]) * m->count) != 0) {
        fprintf(stderr, "bad STATS message\n");
        return -1;
    }
    if(dt < 0.01)
        dt = HYB_REPORT_MS / 1000.0;
    for(i = 0; i < m->count; i++) {
        uint32_t id = ntohl(st[i].path);
        if(id >= (uint32_t)npaths)
            continue;
        found[id] = 1;
        adapt(&paths[id], ntohl(st[i].received), ntohl(st[i].passed),
              (int32_t)ntohl((uint32_t)st[i].owd_us), ntohl(st[i].span_us), dt, now);
    }
    /* まだ1つも届いていない経路も、何も届かないまま止まっていないか確かめる */
    for(i = 0; i < (uint32_t)npaths; i++)
        if(!found[i])
            adapt(&paths[i], paths[i].received, paths[i].passed, HYB_NO_SAMPLE, 0, dt, now);
    if(now - last_print_ns >= 1000000000ull) {
        print_rates(now);
        last_print_ns = now;
    }
    return 0;
}

/*-------------------------- 制御メッセージ ------------------------*/

static void start_round(void)
{
    uint64_t now = mono_ns();
    int i;
    cur_r = cur_k = 0;
    round_count = 0;
    round_end_sent = 0;
    for(i = 0; i < npaths; i++)
        paths[i].next_ns = now;
}

static int on_nack(const struct hyb_msg *m)
{
    unsigned long long missing = 0;
    uint32_t r;

    if(m->count == 0 || m->count > HYB_MAX_RANGES) {
        fprintf(stderr, "unexpected control message type=%u\n", m->type);
        return -1;
    }
    if(read_all(ctrl, ranges, sizeof(struct hyb_range) * m->count) != 0) {
        fprintf(stderr, "failed to read NACK ranges\n");
        return -1;
    }
    nranges = m->count;
    for(r = 0; r < nranges; r++) {
        ranges[r].start = ntohl(ranges[r].start);
        ranges[r].len = ntohl(ranges[r].len);
        if((uint64_t)ranges[r].start + ranges[r].len > total_chunks) {
            fprintf(stderr, "bad NACK range\n");
            return -1;
        }
        missing += ranges[r].len;
    }
    printf("round %u: sent %u, missing %llu (%.3f%%) -> resend\n", round_no, round_count, missing,
           round_count ? 100.0 * (double)missing / round_count : 0.0);
    round_no++;
    if(round_no > max_rounds) {
        fprintf(stderr, "too many rounds (%lu)。UDPの宛先・中継・レートを確認してください\n",
                max_rounds);
        return -1;
    }
    start_round();
    return 0;
}

/* 制御メッセージを1つ処理する */
static int handle_ctrl(void)
{
    struct hyb_msg m;
    if(recv_msg(ctrl, &m) != 0) {
        fprintf(stderr, "control connection closed\n");
        return -1;
    }
    if(m.type == MSG_DONE) {
        done = 1;
        return 0;
    }
    if(m.type == MSG_STATS)
        return on_stats(&m);
    if(m.type == MSG_NACK)
        return on_nack(&m);
    fprintf(stderr, "unexpected control message type=%u\n", m.type);
    return -1;
}

int main(int argc, char **argv)
{
    long buf_len = DEFAULT_BUF_LEN, v;
    int opt, ctrl_port, i, chunk_given = 0;
    char *ctrl_host, *filename;
    struct sockaddr_in ctrlAddr;
    struct stat st;
    struct hyb_hello hello;
    struct hyb_msg m;
    uint64_t chunks64;
    struct timespec start_time, end_time;
    int yes = 1;
    double elapsed;

    while((opt = getopt(argc, argv, "ac:b:m:")) != -1) {
        switch(opt) {
        case 'a':
            adaptive = 1;
            break;
        case 'c':
            v = parse_size(optarg);
            if(v < 1 || v > HYB_MAX_CHUNK) {
                fprintf(stderr, "invalid chunk: %s\n", optarg);
                return 1;
            }
            chunk = (uint32_t)v;
            chunk_given = 1;
            break;
        case 'b':
            buf_len = parse_size(optarg);
            if(buf_len < 1 || buf_len > MAX_BUF_LEN) {
                fprintf(stderr, "invalid BUF_LEN: %s\n", optarg);
                return 1;
            }
            break;
        case 'm': {
            char *end;
            errno = 0;
            max_rounds = strtoul(optarg, &end, 10);
            if(errno || end == optarg || *end || optarg[0] == '-'
               || max_rounds < 1 || max_rounds > UINT32_MAX) {
                fprintf(stderr, "invalid max_rounds\n");
                return 1;
            }
            break;
        }
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if(argc - optind < 4 || argc - optind - 3 > MAX_PATHS) {
        usage(argv[0]);
        return 1;
    }
    if(adaptive) {
        hdr_len = HYB_ADAPT_HDR;
        if(!chunk_given)
            chunk = HYB_ADAPT_CHUNK;
        if(chunk > 65507 - HYB_ADAPT_HDR) {
            fprintf(stderr, "invalid chunk: %u\n", chunk);
            return 1;
        }
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
    rescue_q = malloc(sizeof(uint32_t) * RESCUE_CAP);
    if(block_buf == NULL || ranges == NULL || rescue_q == NULL) {
        perror("malloc");
        return 1;
    }

    /* STEP 2: UDPの宛先（経路）を用意する */
    for(i = optind + 3; i < argc; i++) {
        if(open_path(argv[i], &paths[npaths]) < 0) {
            fprintf(stderr, "invalid dest: %s（IP:PORT@Mbps の形で指定）\n", argv[i]);
            return 1;
        }
        if(adaptive) {
            /* 送信で待たない。手元が詰まったら別の経路で送る */
            fcntl(paths[npaths].sock, F_SETFL, O_NONBLOCK);
            paths[npaths].ring = calloc(RING, sizeof(uint32_t));
            paths[npaths].tsent = calloc(RING, sizeof(uint32_t));
            if(paths[npaths].ring == NULL || paths[npaths].tsent == NULL) {
                perror("calloc");
                return 1;
            }
            paths[npaths].base_cur = paths[npaths].base_prev = INT32_MAX;
            paths[npaths].prev_q = -1;
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
    hello.magic = htonl(adaptive ? HYB_MAGIC_ADAPTIVE : HYB_MAGIC);
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
        fprintf(stderr, "receiver did not send READY（受信側は hyb_send.out -a に対応した hyb_recv.out ですか）\n");
        return 1;
    }

    /* STEP 4: 1回目は全チャンク、2回目以降はNACKで言われたチャンクだけ送る。
       送る合間に制御メッセージ（-a の受信状況、NACK、DONE）を読む */
    ranges[0].start = 0;
    ranges[0].len = total_chunks;
    nranges = total_chunks > 0 ? 1 : 0;
    start_round();
    start_ns = last_print_ns = mono_ns();
    for(i = 0; i < npaths; i++)
        paths[i].progress_at = paths[i].base_at = start_ns;
    while(!done) {
        uint32_t seq;
        int source = peek_seq(&seq), w;

        if(source) {
            struct path *p = pick_path();
            w = wait_ctrl(p->next_ns);
            if(w < 0) {
                perror("poll");
                return 1;
            }
            if(w > 0) {     /* 送る前に制御メッセージを処理する（レートや送る順番が変わりうる） */
                if(handle_ctrl() < 0)
                    return 1;
                continue;
            }
            w = send_chunk(p, seq);
            if(w < 0)
                return 1;
            if(w > 0)       /* この経路は手元で詰まっていた。同じチャンクを別の経路で送る */
                continue;
            consume_seq(source);
            round_count++;
            total_sent++;
            if(round_no > 1)
                retransmitted++;
            continue;
        }
        /* STEP 5: 送り終えたことを伝え、受信側の返事を待つ */
        if(!round_end_sent) {
            if(send_msg(ctrl, MSG_ROUND_END, round_no, round_count) < 0) {
                perror("write ROUND_END");
                return 1;
            }
            round_end_sent = 1;
            continue;
        }
        w = wait_ctrl(UINT64_MAX);
        if(w < 0) {
            perror("poll");
            return 1;
        }
        if(handle_ctrl() < 0)
            return 1;
    }

    /* STEP 6: 完了（DONE受信）までの時間 */
    clock_gettime(CLOCK_REALTIME, &end_time);
    close(ctrl);
    elapsed = ts_diff(&start_time, &end_time);

    printf("File: %llu bytes, chunk %u bytes x %u, BUF_LEN(file read) %u bytes\n",
           (unsigned long long)file_size, chunk, total_chunks, chunks_per_block * chunk);
    for(i = 0; i < npaths; i++) {
        printf("Path %d: %s sent=%llu%s", i, paths[i].spec, paths[i].sent,
               paths[i].refused ? " (port unreachable あり)" : "");
        if(adaptive)
            printf(" rate=%.1fMbps delivered=%.1fMbps resent_elsewhere=%llu pauses=%u local_full=%lu",
                   paths[i].mbps, paths[i].delivered, paths[i].rescued, paths[i].pauses,
                   paths[i].local_full);
        printf("\n");
    }
    printf("Rounds: %u, datagrams sent: %llu (retransmitted %llu, resent from paused paths %llu)\n",
           round_no, total_sent, retransmitted, rescued_total);
    printf("File read count: %lu\n", file_read_count);
    printf("Time (送信開始→DONE受信): %.6f sec\n", elapsed);
    printf("Goodput: %.3f Mbps\n", (double)file_size * 8.0 / elapsed / 1e6);
    printf("CPU time (user+sys): %.6f sec\n", cpu_seconds());
    printf("RESULT prog=hyb_send chunk=%u bytes=%llu rounds=%u sent=%llu retrans=%llu"
           " time=%.6f goodput_mbps=%.3f cpu=%.6f adaptive=%d rescued=%llu\n",
           chunk, (unsigned long long)file_size, round_no, total_sent, retransmitted,
           elapsed, (double)file_size * 8.0 / elapsed / 1e6, cpu_seconds(), adaptive, rescued_total);
    return 0;
}
