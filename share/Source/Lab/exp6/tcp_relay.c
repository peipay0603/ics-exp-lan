/* TCPの逐次中継。データと逆向きの保存完了通知を、半閉鎖を保って渡す。 */
#include "common.h"
#include <poll.h>

struct direction {
    int from, to, eof;
    char *buf;
    size_t offset, length;
    unsigned long long bytes;
};

/* 両方向ともノンブロッキング。大きい送信で逆方向の受信を止めない。 */
static int forward(int client, int next, long buf_len, int idle_timeout,
                   unsigned long long *sent, unsigned long long *replied)
{
    struct direction d[2] = {{0}};
    int sockets[2] = {client, next};
    uint64_t last_activity = mono_ns();
    int failed = 0, i;

    for(i = 0; i < 2; i++) {
        d[i].from = i;
        d[i].to = 1 - i;
        d[i].buf = malloc((size_t)buf_len);
        if(d[i].buf == NULL) {
            failed = 1;
            goto done;
        }
    }
    while(!d[0].eof || !d[1].eof || d[0].length || d[1].length) {
        struct pollfd p[2] = {{client, 0, 0}, {next, 0, 0}};
        uint64_t now = mono_ns();
        uint64_t limit = (uint64_t)idle_timeout * 1000000000ull;
        int r;

        if(now - last_activity >= limit) {
            errno = ETIMEDOUT;
            failed = 1;
            break;
        }
        for(i = 0; i < 2; i++) {
            if(!d[i].eof && !d[i].length)
                p[d[i].from].events |= POLLIN;
            if(d[i].length)
                p[d[i].to].events |= POLLOUT;
        }
        for(i = 0; i < 2; i++)
            if(!p[i].events)
                p[i].fd = -1; /* 読み終えたfdのPOLLHUPによる空回りを防ぐ */
        r = poll(p, 2, (int)((limit - (now - last_activity) + 999999) / 1000000));
        if(r < 0 && errno == EINTR)
            continue;
        if(r < 0) {
            failed = 1;
            break;
        }
        for(i = 0; i < 2; i++) {
            struct direction *dir = &d[i];
            int fresh = 0;
            if(!dir->eof && !dir->length
               && (p[dir->from].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
                ssize_t n = read(sockets[dir->from], dir->buf, (size_t)buf_len);
                if(n > 0) {
                    dir->length = (size_t)n;
                    dir->offset = 0;
                    fresh = 1;
                    last_activity = mono_ns();
                } else if(n == 0) {
                    dir->eof = 1;
                    shutdown(sockets[dir->to], SHUT_WR);
                } else if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    failed = 1;
                    goto done;
                }
            }
            if(dir->length && (fresh || (p[dir->to].revents & (POLLOUT | POLLHUP | POLLERR | POLLNVAL)))) {
                ssize_t n = write(sockets[dir->to], dir->buf + dir->offset, dir->length);
                if(n > 0) {
                    dir->offset += (size_t)n;
                    dir->length -= (size_t)n;
                    dir->bytes += (unsigned long long)n;
                    last_activity = mono_ns();
                } else if(n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    if(n == 0)
                        errno = EIO;
                    failed = 1;
                    goto done;
                }
            }
        }
    }
done:
    if(failed)
        perror("relay transfer");
    *sent = d[0].bytes;
    *replied = d[1].bytes;
    free(d[0].buf);
    free(d[1].buf);
    return failed;
}

int main(int argc, char **argv)
{
    long buf_len = DEFAULT_BUF_LEN;
    int opt, port, next_port, listener, yes = 1, idle_timeout = 30;
    struct sockaddr_in local, next;

    while((opt = getopt(argc, argv, "b:t:")) != -1) {
        if(opt == 'b') {
            buf_len = parse_size(optarg);
            if(buf_len >= 1 && buf_len <= MAX_BUF_LEN)
                continue;
        } else if(opt == 't') {
            idle_timeout = parse_timeout(optarg);
            if(idle_timeout > 0)
                continue;
        }
        fprintf(stderr, "invalid BUF_LEN or timeout\n");
        return 1;
    }
    if(argc - optind != 3) {
        fprintf(stderr, "Usage: %s [-b BUF_LEN] [-t idle_seconds] listen_port next_ip next_port\n", argv[0]);
        return 1;
    }
    port = parse_port(argv[optind]);
    next_port = parse_port(argv[optind + 2]);
    if(port < 0 || next_port < 0 || make_addr(argv[optind + 1], next_port, &next) < 0) {
        fprintf(stderr, "invalid destination or port\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if(listener < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons((unsigned short)port);
    if(bind(listener, (struct sockaddr *)&local, sizeof(local)) < 0 || listen(listener, 5) < 0) {
        perror("bind/listen");
        return 1;
    }
    printf("tcp relay: port %d -> %s:%d BUF_LEN=%ld idle_timeout=%d\n",
           port, argv[optind + 1], next_port, buf_len, idle_timeout);
    fflush(stdout);
    for(;;) {
        int client, downstream, failed, r, error = 0;
        socklen_t error_len = sizeof(error);
        struct pollfd connecting;
        unsigned long long bytes, reply_bytes;

        client = accept(listener, NULL, NULL);
        if(client < 0) {
            if(errno == EINTR)
                continue;
            perror("accept");
            return 1;
        }
        downstream = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if(downstream < 0) {
            close(client);
            continue;
        }
        r = connect(downstream, (struct sockaddr *)&next, sizeof(next));
        if(r < 0 && errno == EINPROGRESS) {
            connecting = (struct pollfd){downstream, POLLOUT, 0};
            do {
                r = poll(&connecting, 1, idle_timeout * 1000);
            } while(r < 0 && errno == EINTR);
            if(r > 0 && getsockopt(downstream, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0) {
                errno = error;
                r = error ? -1 : 0;
            } else {
                if(r == 0)
                    errno = ETIMEDOUT;
                r = -1;
            }
        }
        if(r < 0 || fcntl(client, F_SETFL, O_NONBLOCK) < 0) {
            perror("connect/configure next");
            close(downstream);
            close(client);
            continue;
        }
        failed = forward(client, downstream, buf_len, idle_timeout, &bytes, &reply_bytes);
        close(downstream);
        close(client);
        printf("RESULT prog=tcp_relay buf_len=%ld bytes=%llu reply_bytes=%llu status=%s\n",
               buf_len, bytes, reply_bytes, failed ? "ERROR" : "FORWARDED");
        fflush(stdout);
    }
}
