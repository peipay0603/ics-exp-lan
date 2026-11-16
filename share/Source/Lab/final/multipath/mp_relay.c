#include "mp_common.h"

struct buffer {
    char data[MP_BUFFER];
    size_t length, sent;
};

struct tunnel {
    int client, server, connecting;
    int client_eof, server_eof, client_shutdown, server_shutdown;
    struct buffer to_server, to_client;
};

static void close_tunnel(struct tunnel *t)
{
    if(t->client >= 0)
        close(t->client);
    if(t->server >= 0)
        close(t->server);
    memset(t, 0, sizeof(*t));
    t->client = t->server = -1;
}

static int receive_data(int fd, struct buffer *buf, int *eof)
{
    if(*eof || buf->length != 0)
        return 0;
    ssize_t n = read(fd, buf->data, sizeof(buf->data));
    if(n < 0)
        return errno == EAGAIN || errno == EINTR ? 0 : -1;
    if(n == 0)
        *eof = 1;
    else
        buf->length = (size_t)n;
    return 0;
}

static int send_data(int fd, struct buffer *buf)
{
    if(buf->length == 0)
        return 0;
    ssize_t n = write(fd, buf->data + buf->sent, buf->length - buf->sent);
    if(n < 0)
        return errno == EAGAIN || errno == EINTR ? 0 : -1;
    if(n == 0)
        return -1;
    buf->sent += (size_t)n;
    if(buf->sent == buf->length)
        buf->sent = buf->length = 0;
    return 0;
}

static int update_events(int epfd, struct tunnel *t, int slot)
{
    uint32_t client_events = EPOLLRDHUP, server_events = EPOLLRDHUP;
    if(!t->client_eof && t->to_server.length == 0)
        client_events |= EPOLLIN;
    if(t->to_client.length > 0)
        client_events |= EPOLLOUT;
    if(t->connecting)
        server_events = EPOLLOUT;
    else {
        if(!t->server_eof && t->to_client.length == 0)
            server_events |= EPOLLIN;
        if(t->to_server.length > 0)
            server_events |= EPOLLOUT;
    }
    return mp_event(epfd, EPOLL_CTL_MOD, t->client, client_events, slot * 2) < 0 ||
           mp_event(epfd, EPOLL_CTL_MOD, t->server, server_events, slot * 2 + 1) < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    static struct tunnel tunnels[MP_CONNECTIONS];
    struct epoll_event events[MP_EVENTS];
    int port, listener, epfd, i;
    if(argc != 4 || (port = mp_port(argv[1])) < 0 || mp_port(argv[3]) < 0) {
        fprintf(stderr, "Usage: %s listen_port upstream_host upstream_port\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    for(i = 0; i < MP_CONNECTIONS; i++) {
        memset(&tunnels[i], 0, sizeof(tunnels[i]));
        tunnels[i].client = tunnels[i].server = -1;
    }

    /* STEP 1: 中継用ソケットへの接続を待つ */
    listener = mp_listen(port);
    epfd = epoll_create(MP_EVENTS);
    if(listener < 0 || epfd < 0 ||
       mp_event(epfd, EPOLL_CTL_ADD, listener, EPOLLIN, MP_CONNECTIONS * 2) < 0) {
        perror("listen/epoll");
        return 1;
    }
    while(1) {
        int nfds = epoll_wait(epfd, events, MP_EVENTS, -1);
        if(nfds < 0) {
            if(errno == EINTR)
                continue;
            perror("epoll_wait");
            return 1;
        }
        for(i = 0; i < nfds; i++) {
            int tag = (int)events[i].data.u32;
            if(tag == MP_CONNECTIONS * 2) {
                int sock;
                while((sock = accept(listener, NULL, NULL)) >= 0) {
                    int slot;
                    for(slot = 0; slot < MP_CONNECTIONS; slot++)
                        if(tunnels[slot].client < 0)
                            break;
                    if(slot == MP_CONNECTIONS || mp_nonblock(sock) < 0) {
                        close(sock);
                        continue;
                    }

                    /* STEP 2: 上流のサーバー、または中継ノードへ接続する */
                    struct tunnel *t = &tunnels[slot];
                    t->client = sock;
                    t->server = mp_connect(argv[2], argv[3]);
                    t->connecting = 1;
                    if(t->server < 0 ||
                       mp_event(epfd, EPOLL_CTL_ADD, t->client, EPOLLIN, slot * 2) < 0 ||
                       mp_event(epfd, EPOLL_CTL_ADD, t->server, EPOLLOUT, slot * 2 + 1) < 0)
                        close_tunnel(t);
                }
                continue;
            }
            int slot = tag / 2;
            struct tunnel *t = &tunnels[slot];
            if(t->client < 0)
                continue;
            if(t->connecting && (tag % 2 == 1)) {
                if(mp_connected(t->server) < 0) {
                    close_tunnel(t);
                    continue;
                }
                t->connecting = 0;
            }

            /* STEP 3: 読めたデータをそのまま反対側へ送る */
            if(receive_data(t->client, &t->to_server, &t->client_eof) < 0 ||
               (!t->connecting && receive_data(t->server, &t->to_client, &t->server_eof) < 0) ||
               (!t->connecting && send_data(t->server, &t->to_server) < 0) ||
               send_data(t->client, &t->to_client) < 0 ||
               (events[i].events & EPOLLERR)) {
                close_tunnel(t);
                continue;
            }
            if(!t->connecting && t->client_eof && !t->to_server.length && !t->server_shutdown) {
                shutdown(t->server, SHUT_WR);
                t->server_shutdown = 1;
            }
            if(t->server_eof && !t->to_client.length && !t->client_shutdown) {
                shutdown(t->client, SHUT_WR);
                t->client_shutdown = 1;
            }
            if((t->client_eof && t->server_eof && !t->to_client.length && !t->to_server.length) ||
               update_events(epfd, t, slot) < 0)
                close_tunnel(t);
        }
    }
}
