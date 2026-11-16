/*
 * fclient.c -- 最終課題 ファイル受信クライアント（node1 で実行）
 *              複数経路の同時利用 ＋ 速度による経路変更
 *
 *   ./fclient.out [-o outfile] [-c chunk_KiB] [-d depth] [-m min_Mbps] [-p probe_sec]
 *                 [-S] [-v] [-s 予備経路 ...] 経路 [経路 ...]
 *
 *   経路の書き方:  IP:PORT[xN][/dD][@Mbps][=名前]
 *       xN    … 同じ経路に N 本接続
 *       /dD   … この経路の先出し要求数（遅延の大きい経路用）
 *       @Mbps … 計画上の速度（予測の初期値に使う）
 *       =名前 … 表示用の経路名（例 =3-4-5-1）
 *   例: ./fclient.out 172.23.0.50:20001@40=3-4-5-1 172.21.0.30:10000@10=3-1 \
 *                     -s 172.20.0.20:20005=3-2-1
 *
 * 動作:
 *   1. すべての経路（使用中・予備）に接続し，"SIZE" でファイルサイズを得る
 *   2. ファイルを 256 KiB のチャンクに分け，使用中の経路の接続が次のチャンクを
 *      取りに行く（速い経路ほど多く運ぶ）．1 接続あたり depth 個まで要求を先出し
 *   3. 監視スレッドが 0.5 秒ごとに経路ごとの速度（指数移動平均）を測り，
 *      - 使用中の経路が min_Mbps 未満の状態が続いたら「降格」して予備に回す
 *        （受信待ちのチャンクはすぐ他の経路に回し，接続は張り直す）
 *      - 予備の経路には probe_sec 秒ごとに 1 チャンクだけ送らせて速度を測り，
 *        min_Mbps 以上出たら「昇格」して使い始める（最初の 64 KiB で遅いと
 *        分かったらそのチャンクは待たずに打ち切る）
 *   4. 残りが少なくなったら，到着時間を予測して，遅い経路には新しいチャンクを
 *      渡さない（最後に遅い経路を待つのを防ぐ）
 *   5. 未要求のチャンクがなくなったら，受信中のチャンクを十分速い空いた経路でも
 *      取りに行き（終盤の重複要求），先に届いた方を採用する
 *   6. 受け取ったチャンクは pwrite() でファイルの正しい位置に書く
 *   -S を付けると 3〜4 を行わない（経路を固定した複数経路転送．比較用）
 *
 * 出力: 総データ転送量，ダウンロード時間，実効スループット，経路ごとの内訳，
 *       経路の昇格・降格の記録
 */
#include "common.h"
#include <pthread.h>
#include "output_file.h"

#define MAX_PATHS 32
#define MAX_CONNS 64
#define MAX_DEPTH 64
#define TICK_MS   500          /* 監視の間隔 */
#define WARMUP    2.0          /* 使い始めてからこの秒数は降格しない */
#define DEMOTE_TICKS 4         /* 遅い状態がこの回数続いたら降格（2 秒） */
#define PROBE_BYTES (64 * 1024) /* プローブはこの量を受け取った時点で速度を判定する */

enum { TODO = 0, INFLIGHT = 1, DONE = 2 };                /* チャンクの状態 */
enum { ACTIVE = 0, STANDBY = 1, DEAD = 2 };               /* 経路の役割 */
static const char *role_name[] = { "active", "standby", "dead" };

struct path {
    char   label[96];          /* IP:PORT */
    char   name[32];           /* 3-4-5-1 など */
    double planned;            /* 計画上の速度 [Mbps]（0 なら不明） */
    int    role;
    int    nconn;
    double since;              /* 今の役割になった時刻 */
    /* 速度の測定（mu で保護） */
    long long bytes;           /* 受信バイト数（累計） */
    long long last_bytes;      /* 前回の監視時点の bytes */
    double ewma;               /* 速度の指数移動平均 [Mbps]（使用中の間） */
    int    slow_ticks;
    int    probe_due;          /* 予備：次にプローブしてよい */
    double last_probe;
    double probe_mbps;         /* 直近のプローブ結果 */
    int    probes, promotions, demotions;
    int    chunks;             /* 採用されたチャンク数 */
    int    dup_sent, dup_wasted;
    double active_time;        /* 使用中だった合計時間 */
};

struct req {
    int    k;                  /* チャンク番号 */
    int    probe;              /* プローブ用の要求か */
    int    dup;                /* 重複要求か */
};

struct conn {
    struct path *p;
    struct sockaddr_in addr;
    int    sock;
    int    depth;
    pthread_t th;
    struct req queue[MAX_DEPTH];
    int    qhead, qcount;
    int    kick;               /* 降格されたので接続を張り直す */
    int    failed;             /* この接続だけが再試行上限に達した */
};

/* ---- 共有状態（mu で保護） ---- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static struct path paths[MAX_PATHS];
static struct conn conns[MAX_CONNS];
static int npath, nconn;
static long long fsize = -1;
static long long chunk_size = 256 * 1024;   /* 既定 256 KiB */
static int  depth = 4;                      /* 既定 4（256KiB×4 = 1MiB を先出し） */
static double min_mbps = 5.0;               /* これ未満の経路は使わない */
static double probe_sec = 3.0;              /* 予備経路をプローブする間隔 */
static int  adaptive = 1;                   /* 0: 経路固定（-S） */
static int  verbose = 0;
static int  nchunks, next_todo, todo_left, inflight, written;
static unsigned char *state, *copies;
static int  out_fd, finished;
static int  transfer_error, socket_timeout = 30;
static volatile sig_atomic_t interrupted;
static struct timespec t_start;

static double now_sec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return elapsed_sec(&t_start, &t);
}

static long long chunk_len(int k)
{
    long long off = (long long)k * chunk_size;
    return (off + chunk_size <= fsize) ? chunk_size : fsize - off;
}

static int in_queue(struct conn *c, int k)
{
    for (int i = 0; i < c->qcount; i++)
        if (c->queue[(c->qhead + i) % MAX_DEPTH].k == k) return 1;
    return 0;
}

/* 使用中の経路の速度の合計と最大（mu を持って呼ぶ） */
static void active_rates(double *sum, double *max)
{
    *sum = 0; *max = 0;
    for (int i = 0; i < npath; i++) {
        if (paths[i].role != ACTIVE) continue;
        double r = paths[i].ewma > 0 ? paths[i].ewma : paths[i].planned;
        *sum += r;
        if (r > *max) *max = r;
    }
}

/* 未要求のチャンクを 1 つ取る（mu を持って呼ぶ） */
static int take_todo(void)
{
    while (next_todo < nchunks && state[next_todo] != TODO) next_todo++;
    if (next_todo >= nchunks) return -1;
    int k = next_todo++;
    state[k] = INFLIGHT;
    copies[k] = 1;
    todo_left--;
    inflight++;
    return k;
}

/* 次に要求するチャンクを選ぶ（mu を持って呼ぶ）．なければ -1 */
static int pick_chunk(struct conn *c, struct req *r)
{
    struct path *p = c->p;
    r->probe = r->dup = 0;
    if (finished || p->role == DEAD) return -1;

    if (p->role == STANDBY) {
        /* 予備経路：プローブの順番が来たときだけ 1 チャンク */
        if (!adaptive || !p->probe_due || c->qcount > 0 || todo_left == 0) return -1;
        r->k = take_todo();
        if (r->k < 0) return -1;
        p->probe_due = 0;
        p->probes++;
        r->probe = 1;
        return r->k;
    }

    double my = (p->ewma > 0 ? p->ewma : p->planned) / p->nconn;   /* この接続の速度 */
    double sum, max;
    active_rates(&sum, &max);

    if (todo_left > 0) {
        /* 到着時間の予測：残りが少ないとき，自分が受け取るより全体が終わる方が
         * ずっと早いなら新しいチャンクを取らない（速い経路に任せる） */
        if (adaptive && my > 0 && sum > 0) {
            double remain = (double)(todo_left + inflight) * chunk_size * 8 / 1e6;   /* Mbit */
            double t_all  = remain / sum;
            double t_mine = (c->qcount + 1) * (double)chunk_size * 8 / 1e6 / my;
            if (t_all < 3.0 && t_mine > t_all * 1.5 + 0.1) return -1;
        }
        r->k = take_todo();
        return r->k;
    }

    /* 終盤：受信中のチャンクを重複して取りに行く（十分速い経路だけ） */
    if (c->qcount > 0) return -1;
    if (adaptive && max > 0 && my * p->nconn < max * 0.25) return -1;
    for (int i = 0; i < nchunks; i++) {
        if (state[i] == INFLIGHT && copies[i] < 2 && !in_queue(c, i)) {
            copies[i]++;
            p->dup_sent++;
            r->k = i;
            r->dup = 1;
            return i;
        }
    }
    return -1;
}

/* 受信待ちのチャンクを未要求に戻す（mu を持って呼ぶ） */
static void give_back(struct conn *c)
{
    for (int i = 0; i < c->qcount; i++) {
        int k = c->queue[(c->qhead + i) % MAX_DEPTH].k;
        if (state[k] == INFLIGHT && --copies[k] == 0) {
            state[k] = TODO;
            todo_left++;
            inflight--;
            if (k < next_todo) next_todo = k;
        }
    }
    c->qcount = 0;
    c->qhead = 0;
}

static int do_connect(struct conn *c)
{
    int one = 1;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval timeout = { socket_timeout, 0 };
    char line[REQ_MAX], extra;
    long long size;
    if (sock < 0) return -1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect_timeout(sock, &c->addr, socket_timeout) < 0 ||
        write_all(sock, "SIZE\r\n", 6) < 0 || read_line(sock, line, sizeof(line)) < 0)
        goto fail;
    if (sscanf(line, "SIZE %lld %c", &size, &extra) != 1 || size < 0 ||
        (fsize >= 0 && size != fsize)) {
        errno = EPROTO;
        goto fail;
    }
    pthread_mutex_lock(&mu);
    if (finished || interrupted) { pthread_mutex_unlock(&mu); goto fail; }
    if (fsize < 0) fsize = size;
    c->sock = sock;
    pthread_mutex_unlock(&mu);
    return 0;
fail:
    close(sock);
    return -1;
}

static void close_connection(struct conn *c)
{
    pthread_mutex_lock(&mu);
    int sock = c->sock;
    c->sock = -1;
    pthread_mutex_unlock(&mu);
    if (sock >= 0) close(sock);
}

static int failed_attempt(struct conn *c, int *retries)
{
    if (++*retries < 5) return 0;
    pthread_mutex_lock(&mu);
    give_back(c);
    c->failed = 1;
    int alive = 0;
    for (int i = 0; i < nconn; i++)
        if (conns[i].p == c->p && !conns[i].failed) alive++;
    c->p->nconn = alive;
    if (!alive) c->p->role = DEAD;
    fprintf(stderr, "[%6.2fs] connection %s: 5 attempts without a complete chunk, removed\n",
            now_sec(), c->p->label);
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return -1;
}

static void on_signal(int signum)
{
    (void)signum;
    interrupted = 1;
}

static int save_chunk(const char *buf, size_t length, off_t offset)
{
    while (length) {
        ssize_t n = pwrite(out_fd, buf, length, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        length -= (size_t)n;
        offset += n;
        buf += n;
    }
    return 0;
}
static void *worker(void *arg)
{
    struct conn *c = arg;
    struct path *p = c->p;
    char *buf = malloc(chunk_size);
    char reqbuf[MAX_DEPTH * 48];
    int retries = 0;

    if (!buf) {
        pthread_mutex_lock(&mu);
        transfer_error = finished = 1;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
        return NULL;
    }

    for (;;) {
        pthread_mutex_lock(&mu);
        if (finished || p->role == DEAD) {
            pthread_mutex_unlock(&mu);
            break;
        }
        /* 接続が切れている／降格された：受信待ちを戻して張り直す */
        if (c->sock < 0 || c->kick) {
            give_back(c);
            c->kick = 0;
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&mu);
            close_connection(c);
            usleep(200000);
            if (do_connect(c) < 0) {
                if (failed_attempt(c, &retries) < 0) break;
            }
            continue;
        }

        /* 要求を depth 個まで先出しする */
        int n = 0;
        while (c->qcount < c->depth) {
            struct req r;
            if (pick_chunk(c, &r) < 0) break;
            n += snprintf(reqbuf + n, sizeof(reqbuf) - n, "GET %lld %lld\r\n",
                          (long long)r.k * chunk_size, chunk_len(r.k));
            c->queue[(c->qhead + c->qcount) % MAX_DEPTH] = r;
            c->qcount++;
        }
        if (c->qcount == 0) {
            /* 仕事がない：状況が変わるまで（最大 0.1 秒）待つ */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100 * 1000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&cv, &mu, &ts);
            pthread_mutex_unlock(&mu);
            continue;
        }
        struct req head = c->queue[c->qhead];
        pthread_mutex_unlock(&mu);

        if (n > 0 && write_all(c->sock, reqbuf, n) < 0) goto broken;

        /* 先頭のチャンクを受信する（応答は要求順に届く） */
        long long len = chunk_len(head.k);
        struct timespec tf, te;
        if (read_full(c->sock, buf, 1) < 0) goto broken;
        clock_gettime(CLOCK_MONOTONIC, &tf);                   /* 最初の 1 バイトが届いた */
        if (head.probe && len > PROBE_BYTES) {
            /* プローブ：最初の 64 KiB で速度を判定し，遅ければ待たずに打ち切る
             * （遅い経路で 1 チャンク全部を待つと，次の判定まで何秒もかかるため） */
            if (read_full(c->sock, buf + 1, PROBE_BYTES - 1) < 0) goto broken;
            clock_gettime(CLOCK_MONOTONIC, &te);
            double dt = elapsed_sec(&tf, &te);
            double mbps = dt > 0 ? (PROBE_BYTES - 1) * 8.0 / dt / 1e6 : 1e9;
            if (mbps < min_mbps) {
                pthread_mutex_lock(&mu);
                p->probe_mbps = mbps > 0 ? mbps : 0.001;
                p->last_probe = now_sec();
                p->bytes += PROBE_BYTES;
                c->kick = 1;                /* 受信待ちのチャンクは戻して接続を張り直す */
                pthread_mutex_unlock(&mu);
                continue;
            }
            if (read_full(c->sock, buf + PROBE_BYTES, (size_t)(len - PROBE_BYTES)) < 0) goto broken;
        } else if (len > 1 && read_full(c->sock, buf + 1, (size_t)len - 1) < 0) {
            goto broken;
        }
        clock_gettime(CLOCK_MONOTONIC, &te);
        retries = 0;  /* 接続成功ではなく、チャンクを最後まで受けたときだけリセット */

        int mine = 0;
        pthread_mutex_lock(&mu);
        c->qhead = (c->qhead + 1) % MAX_DEPTH;
        c->qcount--;
        p->bytes += len;
        if (head.probe) {
            /* プローブの速度：往復時間を除くため最初の 1 バイト以降で測る */
            double dt = elapsed_sec(&tf, &te);
            p->probe_mbps = dt > 0 ? (len - 1) * 8.0 / dt / 1e6 : 1e9;
            p->last_probe = now_sec();
        }
        if (state[head.k] != DONE) {
            state[head.k] = DONE;
            inflight--;
            mine = 1;
        } else {
            p->dup_wasted++;
        }
        pthread_mutex_unlock(&mu);

        if (mine) {
            if (save_chunk(buf, (size_t)len, (off_t)head.k * chunk_size) < 0) {
                perror("pwrite");
                pthread_mutex_lock(&mu);
                transfer_error = finished = 1;
                pthread_cond_broadcast(&cv);
                pthread_mutex_unlock(&mu);
                break;
            }
            pthread_mutex_lock(&mu);
            p->chunks++;
            if (++written == nchunks) finished = 1;
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&mu);
        }
        continue;

    broken:
        pthread_mutex_lock(&mu);
        int intentional = finished || c->kick;
        pthread_mutex_unlock(&mu);
        if (verbose && !intentional)
            fprintf(stderr, "[%6.2fs] connection %s closed, reconnecting\n", now_sec(), p->label);
        close_connection(c);
        if (!intentional && failed_attempt(c, &retries) < 0) break;
    }
    close_connection(c);
    free(buf);
    return NULL;
}

static void set_role(struct path *p, int role, const char *why)
{
    double t = now_sec();
    if (p->role == ACTIVE) p->active_time += t - p->since;
    printf("[%6.2fs] path %-8s %-20s %s -> %s (%s)\n", t, p->name, p->label,
           role_name[p->role], role_name[role], why);
    p->role = role;
    p->since = t;
    p->slow_ticks = 0;
    p->ewma = 0;
}

/* 監視スレッド：経路ごとの速度を測り，昇格・降格を決める */
static void *monitor(void *arg)
{
    (void)arg;
    double prev = now_sec();
    int rr = 0;                         /* プローブの順番（ラウンドロビン） */
    char why[96];

    pthread_mutex_lock(&mu);
    for (;;) {
        /* TICK_MS ごとに起きる．転送が終わったらすぐ抜ける（usleep だと終了が遅れる） */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += TICK_MS * 1000000L;
        while (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        while (!finished && pthread_cond_timedwait(&cv, &mu, &ts) == 0)
            ;
        if (finished) break;
        double t = now_sec(), dt = t - prev;
        prev = t;

        int nactive = 0;
        for (int i = 0; i < npath; i++)
            if (paths[i].role == ACTIVE) nactive++;

        for (int i = 0; i < npath; i++) {
            struct path *p = &paths[i];
            double rate = (p->bytes - p->last_bytes) * 8.0 / dt / 1e6;
            p->last_bytes = p->bytes;
            if (p->role != ACTIVE) continue;

            /* 仕事を抱えている間だけ速度を更新する（終盤の待ちを遅さと誤解しない） */
            int busy = 0;
            for (int j = 0; j < nconn; j++)
                if (conns[j].p == p && conns[j].qcount > 0) busy = 1;
            if (!busy) continue;
            p->ewma = p->ewma > 0 ? 0.5 * p->ewma + 0.5 * rate : rate;
            if (verbose)
                printf("[%6.2fs]   %-8s %6.2f Mbps (ewma %6.2f)\n", t, p->name, rate, p->ewma);

            /* 降格：使い始めてしばらく経っても min_Mbps に届かない状態が続いた */
            if (t - p->since > WARMUP && p->ewma < min_mbps && todo_left > 0) {
                if (++p->slow_ticks >= DEMOTE_TICKS && nactive > 1) {
                    snprintf(why, sizeof(why), "%.2f Mbps < %.1f", p->ewma, min_mbps);
                    set_role(p, STANDBY, why);
                    p->demotions++;
                    p->last_probe = t;
                    nactive--;
                    for (int j = 0; j < nconn; j++)        /* 受信待ちをすぐ他へ回す */
                        if (conns[j].p == p && conns[j].qcount > 0) {
                            conns[j].kick = 1;
                            if (conns[j].sock >= 0) shutdown(conns[j].sock, SHUT_RDWR);
                        }
                }
            } else {
                p->slow_ticks = 0;
            }
        }

        /* 予備経路：プローブの結果が min_Mbps 以上なら昇格 */
        for (int i = 0; i < npath; i++) {
            struct path *p = &paths[i];
            if (p->role != STANDBY || p->probe_mbps <= 0) continue;
            if (p->probe_mbps >= min_mbps) {
                snprintf(why, sizeof(why), "probe %.2f Mbps", p->probe_mbps);
                set_role(p, ACTIVE, why);
                p->promotions++;
                nactive++;
            }
            p->probe_mbps = 0;
        }
        /* 次のプローブを 1 本だけ許可する（同時に測って帯域を食わないように） */
        if (todo_left > 0) {
            for (int n = 0; n < npath; n++) {
                struct path *p = &paths[(rr + n) % npath];
                if (p->role == STANDBY && !p->probe_due && t - p->last_probe >= probe_sec) {
                    p->probe_due = 1;
                    p->last_probe = t;
                    rr = (rr + n + 1) % npath;
                    break;
                }
            }
        }
        /* 使える経路がなくなったら予備を全部使う */
        if (nactive == 0)
            for (int i = 0; i < npath; i++)
                if (paths[i].role == STANDBY) set_role(&paths[i], ACTIVE, "no active path");
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* "IP:PORT[xN][/dD][@Mbps][=名前]" を読む */
static void parse_path(const char *s, int role)
{
    char spec[160], ip[48], *q, *end;
    int port, mult = 1, d = depth;
    double mbps = 0;
    char name[32] = "";

    if (strlen(s) >= sizeof(spec)) { fprintf(stderr, "path too long\n"); exit(1); }
    snprintf(spec, sizeof(spec), "%s", s);
    if ((q = strchr(spec, '=')) != NULL) { *q = '\0'; snprintf(name, sizeof(name), "%s", q + 1); }
    if ((q = strchr(spec, '@')) != NULL) {
        *q = '\0'; errno = 0; mbps = strtod(q + 1, &end);
        if (errno || end == q + 1 || *end || !isfinite(mbps) || mbps < 0) {
            fprintf(stderr, "bad rate: %s\n", s); exit(1);
        }
    }
    if ((q = strchr(spec, '/')) != NULL) {
        *q = '\0';
        errno = 0;
        long value = q[1] == 'd' ? strtol(q + 2, &end, 10) : -1;
        if (q[1] != 'd' || errno || end == q + 2 || *end || value < 1 || value > MAX_DEPTH) {
            fprintf(stderr, "bad depth in %s (1..%d)\n", s, MAX_DEPTH);
            exit(1);
        }
        d = (int)value;
    }
    q = strchr(spec, ':');
    if (!q || (size_t)(q - spec) >= sizeof(ip)) goto bad_path;
    *q++ = '\0';
    snprintf(ip, sizeof(ip), "%s", spec);
    char *count = strchr(q, 'x');
    if (count) {
        *count++ = '\0'; errno = 0;
        long value = strtol(count, &end, 10);
        if (errno || end == count || *end || value < 1 || value > MAX_CONNS) goto bad_path;
        mult = (int)value;
    }
    port = valid_port(q);
    if (port < 0) {
bad_path:
        fprintf(stderr, "bad path: %s (IP:PORT[xN][/dD][@Mbps][=name])\n", s);
        exit(1);
    }
    if (npath >= MAX_PATHS || nconn + mult > MAX_CONNS) {
        fprintf(stderr, "too many paths/connections\n");
        exit(1);
    }
    struct path *p = &paths[npath++];
    memset(p, 0, sizeof(*p));
    snprintf(p->label, sizeof(p->label), "%s:%d", ip, port);
    snprintf(p->name, sizeof(p->name), "%s", name[0] ? name : "-");
    p->planned = mbps;
    p->role = role;
    p->nconn = mult;
    for (int i = 0; i < mult; i++) {
        struct conn *c = &conns[nconn++];
        memset(c, 0, sizeof(*c));
        c->p = p;
        c->depth = d;
        c->sock = -1;
        c->addr.sin_family = AF_INET;
        c->addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip, &c->addr.sin_addr) != 1) {
            fprintf(stderr, "bad ip: %s\n", ip);
            exit(1);
        }
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-o outfile] [-c chunk_KiB] [-d depth] [-m min_Mbps] [-p probe_sec]\n"
        "          [-t timeout_sec] [-S] [-v] [-s standby_path ...] path [path ...]\n"
        "  path = IP:PORT[xN][/dD][@Mbps][=name]\n", prog);
    exit(1);
}

static long long number(const char *text, long long min, long long max)
{
    char *end;
    errno = 0;
    long long n = strtoll(text, &end, 10);
    return errno || end == text || *end || n < min || n > max ? -1 : n;
}

static double positive_rate(const char *text)
{
    char *end;
    errno = 0;
    double n = strtod(text, &end);
    return errno || end == text || *end || !isfinite(n) || n < 0 ? -1 : n;
}

int main(int argc, char **argv)
{
    int opt;
    char *outfile = "received.dat";
    char *standby[MAX_PATHS];
    int nstandby = 0;
    struct timespec t1;
    pthread_t mon;

    while ((opt = getopt(argc, argv, "o:c:d:m:p:t:s:Sv")) != -1) {
        switch (opt) {
        case 'o': outfile = optarg; break;
        case 'c': chunk_size = number(optarg, 1, 65536) * 1024; break;
        case 'd': depth = (int)number(optarg, 1, MAX_DEPTH); break;
        case 'm': min_mbps = positive_rate(optarg); break;
        case 'p': probe_sec = positive_rate(optarg); break;
        case 't': socket_timeout = (int)number(optarg, 1, 3600); break;
        case 's': if (nstandby >= MAX_PATHS) usage(argv[0]); standby[nstandby++] = optarg; break;
        case 'S': adaptive = 0; break;
        case 'v': verbose = 1; break;
        default: usage(argv[0]);
        }
    }
    if (optind >= argc || chunk_size <= 0 || depth < 1 || depth > MAX_DEPTH ||
        min_mbps < 0 || probe_sec <= 0 || socket_timeout < 1) usage(argv[0]);
    for (int i = optind; i < argc; i++) parse_path(argv[i], ACTIVE);
    if (adaptive)                                   /* -S のときは予備経路を使わない */
        for (int i = 0; i < nstandby; i++) parse_path(standby[i], STANDBY);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 計測開始：接続の確立（中継の接続も含む）からファイル保存の完了まで */
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* STEP 1: 各接続でSIZEを確認。先頭が不通でも、到達可能な経路を使う。 */
    for (int i = 0; i < nconn; i++) {
        if (do_connect(&conns[i]) < 0) {
            fprintf(stderr, "connect %s failed (will retry)\n", conns[i].p->label);
        }
    }

    if (fsize < 0 || interrupted) {
        fprintf(stderr, "no path supplied a valid SIZE\n");
        return 1;
    }
    long long count = fsize / chunk_size + (fsize % chunk_size != 0);
    if (count > INT_MAX) { fprintf(stderr, "too many chunks\n"); return 1; }
    nchunks = (int)count;
    todo_left = nchunks;
    state  = calloc(nchunks > 0 ? nchunks : 1, 1);
    copies = calloc(nchunks > 0 ? nchunks : 1, 1);
    if (!state || !copies) { perror("calloc"); return 1; }

    out_fd = output_open(outfile);
    if (out_fd < 0 || ftruncate(out_fd, fsize) < 0) {
        perror(outfile);
        return 1;
    }

    /* STEP 3: 接続ごとのスレッドと監視スレッドを起動 */
    finished = (nchunks == 0);
    for (int i = 0; i < npath; i++) {
        paths[i].since = now_sec();
        paths[i].last_probe = -probe_sec;          /* 予備経路は最初に順番に測る */
    }
    int started = 0, monitoring = 0;
    for (int i = 0; i < nconn; i++) {
        int error = pthread_create(&conns[i].th, NULL, worker, &conns[i]);
        if (error) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            pthread_mutex_lock(&mu);
            transfer_error = finished = 1;
            pthread_mutex_unlock(&mu);
            break;
        }
        started++;
    }
    if (adaptive && started == nconn) {
        int error = pthread_create(&mon, NULL, monitor, NULL);
        if (!error) monitoring = 1;
        else {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            pthread_mutex_lock(&mu);
            transfer_error = finished = 1;
            pthread_mutex_unlock(&mu);
        }
    }

    /* STEP 4: 全チャンクがファイルに書かれるまで待つ */
    pthread_mutex_lock(&mu);
    for (;;) {
        int alive = 0;
        for (int i = 0; i < npath; i++)
            if (paths[i].role != DEAD) alive = 1;
        if (finished || !alive || interrupted) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&cv, &mu, &ts);
    }
    int ok = (written == nchunks && !transfer_error && !interrupted);
    finished = 1;
    double tend = now_sec();
    for (int i = 0; i < npath; i++)
        if (paths[i].role == ACTIVE) paths[i].active_time += tend - paths[i].since;
    pthread_cond_broadcast(&cv);
    /* ソケット所有者が閉じる前にshutdown。出力fdは全worker停止まで閉じない。 */
    for (int i = 0; i < nconn; i++)
        if (conns[i].sock >= 0) shutdown(conns[i].sock, SHUT_RDWR);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < started; i++) pthread_join(conns[i].th, NULL);
    if (monitoring) pthread_join(mon, NULL);
    for (int i = started; i < nconn; i++) close_connection(&conns[i]);
    if (!ok) {
        fprintf(stderr, "transfer failed: %d / %d chunks\n", written, nchunks);
        return 1;
    }
    if (output_commit() < 0) { perror("publish output"); return 1; }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    /* STEP 5: 結果の表示 */
    double sec = elapsed_sec(&t_start, &t1);
    long long wire = 0;
    printf("=== per path ===\n");
    for (int i = 0; i < npath; i++) {
        struct path *p = &paths[i];
        wire += p->bytes;
        printf("%-9s %-20s chunks=%4d (%5.1f%%) %6.2f Mbps  used %5.1fs  +%d/-%d  probes=%d dup=%d wasted=%d\n",
               p->name, p->label, p->chunks, nchunks ? 100.0 * p->chunks / nchunks : 0.0,
               p->bytes * 8.0 / sec / 1e6, p->active_time, p->promotions, p->demotions,
               p->probes, p->dup_sent, p->dup_wasted);
    }
    printf("=== result ===\n");
    printf("File            : %s\n", outfile);
    printf("Total bytes     : %lld (%.2f MB)\n", fsize, fsize / 1e6);
    printf("Received on wire: %lld (duplicates %lld)\n", wire, wire - fsize);
    printf("Chunk size      : %lld KiB x %d chunks, depth %d, %d paths / %d connections, %s\n",
           chunk_size / 1024, nchunks, depth, npath, nconn, adaptive ? "adaptive" : "static (-S)");
    printf("Download time   : %.3f sec\n", sec);
    printf("Throughput      : %.3f Mbps\n", fsize * 8.0 / sec / 1e6);
    return 0;
}
