#include "mp_common.h"

struct connection {
    int sock, file;
    char request[4096];
    size_t request_length;
    char buf[MP_BUFFER];
    size_t used, sent;
    unsigned long long remaining;
};

static void disconnect_client(struct connection *c)
{
    if(c->sock >= 0)
        close(c->sock);
    if(c->file >= 0)
        close(c->file);
    memset(c, 0, sizeof(*c));
    c->sock = c->file = -1;
}

static int serve(struct connection *c, unsigned long long size)
{
    int rounds;
    for(rounds = 0; rounds < 32; rounds++) {
        if(c->sent < c->used) {
            ssize_t n = write(c->sock, c->buf + c->sent, c->used - c->sent);
            if(n < 0 && errno == EINTR)
                continue;
            if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0;
            if(n <= 0)
                return -1;
            c->sent += (size_t)n;
            if(c->sent < c->used)
                continue;
            c->sent = c->used = 0;
        }
        if(c->remaining > 0) {
            size_t want = c->remaining < MP_BUFFER ? (size_t)c->remaining : MP_BUFFER;
            ssize_t n = read(c->file, c->buf, want);
            if(n < 0 && errno == EINTR)
                continue;
            if(n <= 0)
                return -1;
            c->used = (size_t)n;
            c->remaining -= (unsigned long long)n;
            continue;
        }
        char *end = memchr(c->request, '\n', c->request_length);
        if(end == NULL)
            return 0;
        size_t length = (size_t)(end - c->request) + 1;
        char line[MP_LINE], extra;
        unsigned long long offset, count;
        if(length >= sizeof(line))
            return -1;
        memcpy(line, c->request, length);
        line[length] = '\0';
        memmove(c->request, c->request + length, c->request_length - length);
        c->request_length -= length;
        if(sscanf(line, "GET %llu %llu %c", &offset, &count, &extra) != 2 ||
           count == 0 || offset > size || count > size - offset ||
           lseek(c->file, (off_t)offset, SEEK_SET) < 0)
            return -1;
        c->remaining = count;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static struct connection clients[MP_CONNECTIONS];
    struct epoll_event events[MP_EVENTS];
    struct stat info;
    int port, listener, epfd, i;
    if(argc != 3 || (port = mp_port(argv[2])) < 0) {
        fprintf(stderr, "Usage: %s input_file port\n", argv[0]);
        return 1;
    }
    int source = open(argv[1], O_RDONLY);
    if(source < 0 || fstat(source, &info) < 0 || !S_ISREG(info.st_mode)) {
        perror("input file");
        if(source >= 0)
            close(source);
        return 1;
    }
    close(source);
    signal(SIGPIPE, SIG_IGN);
    for(i = 0; i < MP_CONNECTIONS; i++) {
        memset(&clients[i], 0, sizeof(clients[i]));
        clients[i].sock = clients[i].file = -1;
    }

    /* STEP 1: ソケットを作り、接続を待つ */
    listener = mp_listen(port);
    epfd = epoll_create(MP_EVENTS);
    if(listener < 0 || epfd < 0 ||
       mp_event(epfd, EPOLL_CTL_ADD, listener, EPOLLIN, MP_CONNECTIONS) < 0) {
        perror("listen/epoll");
        return 1;
    }
    printf("port=%d bytes=%llu\n", port, (unsigned long long)info.st_size);
    fflush(stdout);
    while(1) {
        int nfds = epoll_wait(epfd, events, MP_EVENTS, -1);
        if(nfds < 0) {
            if(errno == EINTR)
                continue;
            perror("epoll_wait");
            return 1;
        }
        for(i = 0; i < nfds; i++) {
            int index = (int)events[i].data.u32;
            if(index == MP_CONNECTIONS) {
                int sock;
                while((sock = accept(listener, NULL, NULL)) >= 0) {
                    int slot;
                    for(slot = 0; slot < MP_CONNECTIONS; slot++)
                        if(clients[slot].sock < 0)
                            break;
                    if(slot == MP_CONNECTIONS || mp_nonblock(sock) < 0) {
                        close(sock);
                        continue;
                    }
                    struct connection *c = &clients[slot];
                    c->sock = sock;
                    c->file = open(argv[1], O_RDONLY);
                    struct stat current;
                    if(c->file < 0 || fstat(c->file, &current) < 0 ||
                       current.st_size != info.st_size || current.st_mtime != info.st_mtime) {
                        disconnect_client(c);
                        continue;
                    }
                    c->used = (size_t)snprintf(c->buf, sizeof(c->buf), "SIZE %llu\n",
                                             (unsigned long long)info.st_size);
                    if(mp_event(epfd, EPOLL_CTL_ADD, sock, EPOLLIN | EPOLLOUT,
                                slot) < 0)
                        disconnect_client(c);
                }
                continue;
            }
            struct connection *c = &clients[index];
            if(c->sock < 0)
                continue;

            /* STEP 2: 要求された位置と長さを読み取る */
            if(events[i].events & EPOLLIN) {
                ssize_t n = read(c->sock, c->request + c->request_length,
                                 sizeof(c->request) - c->request_length);
                if(n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                    disconnect_client(c);
                    continue;
                }
                if(n > 0)
                    c->request_length += (size_t)n;
            }

            /* STEP 3: 指定された範囲をファイルから読み、送信する */
            if(serve(c, (unsigned long long)info.st_size) < 0 ||
               c->request_length == sizeof(c->request) ||
               (events[i].events & EPOLLERR)) {
                disconnect_client(c);
                continue;
            }
            uint32_t mask = EPOLLIN;
            if(c->used > c->sent || c->remaining ||
               memchr(c->request, '\n', c->request_length) != NULL)
                mask |= EPOLLOUT;
            if(mp_event(epfd, EPOLL_CTL_MOD, c->sock, mask, index) < 0)
                disconnect_client(c);
        }
    }
}
