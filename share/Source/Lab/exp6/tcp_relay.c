/* node3から接続して送るTCP版の逐次中継。旧GET要求方式とは別プログラム。 */
#include "common.h"

int main(int argc, char **argv)
{
    long buf_len = DEFAULT_BUF_LEN;
    int opt, port, next_port, listener, yes = 1;
    struct sockaddr_in local, next;
    char *buf;

    while((opt = getopt(argc, argv, "b:")) != -1) {
        if(opt != 'b' || (buf_len = parse_size(optarg)) < 1 || buf_len > MAX_BUF_LEN) {
            fprintf(stderr, "invalid BUF_LEN\n");
            return 1;
        }
    }
    if(argc - optind != 3) {
        fprintf(stderr, "Usage: %s [-b BUF_LEN] listen_port next_ip next_port\n", argv[0]);
        return 1;
    }
    port = parse_port(argv[optind]);
    next_port = parse_port(argv[optind + 2]);
    if(port < 0 || next_port < 0 || make_addr(argv[optind + 1], next_port, &next) < 0) {
        fprintf(stderr, "invalid destination or port\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    buf = malloc((size_t)buf_len);
    if(buf == NULL) {
        perror("malloc");
        return 1;
    }
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
    printf("tcp relay: port %d -> %s:%d BUF_LEN=%ld\n", port, argv[optind + 1], next_port, buf_len);
    fflush(stdout);

    for(;;) {
        int client, downstream, failed = 0;
        unsigned long long bytes = 0;
        ssize_t n;

        client = accept(listener, NULL, NULL);
        if(client < 0) {
            if(errno == EINTR)
                continue;
            perror("accept");
            return 1;
        }
        downstream = socket(AF_INET, SOCK_STREAM, 0);
        if(downstream < 0 || connect(downstream, (struct sockaddr *)&next, sizeof(next)) < 0) {
            perror("connect next");
            if(downstream >= 0)
                close(downstream);
            close(client);
            continue; /* 1回の失敗で待受を終了させない */
        }
        /* 全体を保存せず、届いた分を次へ渡す。ヘッダもそのまま転送する。 */
        for(;;) {
            n = read(client, buf, (size_t)buf_len);
            if(n < 0 && errno == EINTR)
                continue;
            if(n <= 0) {
                if(n < 0) {
                    perror("read client");
                    failed = 1;
                }
                break;
            }
            if(write_all(downstream, buf, (size_t)n) < 0) {
                perror("write next");
                failed = 1;
                break;
            }
            bytes += (unsigned long long)n;
        }
        shutdown(downstream, SHUT_WR);
        close(downstream);
        close(client);
        printf("RESULT prog=tcp_relay buf_len=%ld bytes=%llu status=%s\n",
               buf_len, bytes, failed ? "ERROR" : "FORWARDED");
        fflush(stdout);
    }
}
