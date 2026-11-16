/*
 * fserver.c -- 最終課題 ファイルサーバ（node3 で実行）
 *
 *   ./fserver.out filename [port]
 *
 * 接続ごとに fork し，1 本の接続で次の要求を何度でも処理する．
 *   "SIZE\r\n"            -> "SIZE <ファイルサイズ>\n" を返す
 *   "GET <off> <len>\r\n" -> ファイルの [off, off+len) をそのまま送る（チャンク要求）
 *   "GET\r\n" など        -> ファイル全体を送って close（exp43 の fclient と互換）
 *
 * 送信には sendfile() を使い，ファイル→ソケットのコピーをカーネル内で行う．
 */
#include "common.h"
#include <sys/sendfile.h>

/* fd の [off, off+len) をソケットへ送る．成功 0，失敗 -1 */
static int send_range(int sock, int fd, off_t off, size_t len)
{
    while (len > 0) {
        ssize_t m = sendfile(sock, fd, &off, len);
        if (m < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (m == 0) return -1;              /* ファイルの終端を越えた */
        len -= (size_t)m;
    }
    return 0;
}

/* 1 本の接続を処理する（子プロセス） */
static void serve(int sock, const char *filename)
{
    char line[REQ_MAX];
    struct stat st;
    long long served = 0;
    int nreq = 0;
    int fd = open(filename, O_RDONLY);

    if (fd < 0 || fstat(fd, &st) < 0) {
        perror("open");
        return;
    }

    while (read_line(sock, line, sizeof(line)) == 0) {
        long long off, len;
        if (strcmp(line, "SIZE") == 0) {
            char reply[64];
            int n = snprintf(reply, sizeof(reply), "SIZE %lld\n", (long long)st.st_size);
            if (write_all(sock, reply, n) < 0) break;
        } else if (sscanf(line, "GET %lld %lld", &off, &len) == 2) {
            if (off < 0 || len < 0 || off + len > st.st_size) {
                fprintf(stderr, "bad range: %s\n", line);
                break;
            }
            if (send_range(sock, fd, off, (size_t)len) < 0) break;
            served += len;
            nreq++;
        } else {
            /* 範囲指定なし：ファイル全体を送って終了（4-1 の方式） */
            if (send_range(sock, fd, 0, (size_t)st.st_size) == 0)
                served += st.st_size;
            nreq++;
            break;
        }
    }
    close(fd);
    printf("[fserver %d] served %lld bytes in %d requests\n", getpid(), served, nreq);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int sock0, sock, yes = 1;
    int port = SERVER_PORT;
    struct sockaddr_in addr, peer;
    socklen_t plen;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s filename [port]\n", argv[0]);
        return 1;
    }
    if (argc > 2) port = atoi(argv[2]);
    if (access(argv[1], R_OK) != 0) {
        perror(argv[1]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);               /* 相手が先に切っても落ちない */
    signal(SIGCHLD, SIG_IGN);               /* 子プロセスを自動回収 */

    sock0 = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(sock0, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(sock0, 64) < 0) {
        perror("listen");
        return 1;
    }
    printf("fserver: file=%s port=%d\n", argv[1], port);
    fflush(stdout);

    for (;;) {
        plen = sizeof(peer);
        sock = accept(sock0, (struct sockaddr *)&peer, &plen);
        if (sock < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        printf("accept from %s:%d\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        fflush(stdout);
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        if (fork() == 0) {
            close(sock0);
            serve(sock, argv[1]);
            close(sock);
            _exit(0);
        }
        close(sock);
    }
    close(sock0);
    return 0;
}
