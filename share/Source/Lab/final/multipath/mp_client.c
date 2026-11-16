#include "mp_common.h"

#define MAX_LANES 64
#define MAX_PIPELINE 16

struct task {
    unsigned long long offset, length;
    int done, copies, owner;
    double assigned;
};

struct lane {
    int sock, path, connecting, ready;
    char host[256], port[16], header[MP_LINE];
    size_t header_length;
    char request[2048];
    size_t request_length, request_sent;
    size_t queue[MAX_PIPELINE];
    unsigned long long received[MAX_PIPELINE];
    int head, queued;
    unsigned long long bytes, useful;
    unsigned long chunks;
    double started, last_data;
};

struct transfer {
    struct task *tasks;
    size_t task_count, capacity;
    unsigned long long file_size, goal, next_offset, complete, chunk, limit;
    unsigned long long wire_bytes, duplicate_bytes;
    unsigned long requests, retries, hedges;
    int known, output, pipeline, hedge;
};

static unsigned long long number(const char *text)
{
    char *end;
    unsigned long long n;
    errno = 0;
    if(text[0] == '-')
        return 0;
    n = strtoull(text, &end, 10);
    return errno || end == text || *end ? 0 : n;
}

static void drop_lane(struct lane *lane, struct transfer *t)
{
    int i;
    if(lane->sock >= 0)
        close(lane->sock);
    lane->sock = -1;
    for(i = 0; i < lane->queued; i++) {
        size_t index = lane->queue[(lane->head + i) % MAX_PIPELINE];
        if(t->tasks[index].copies > 0)
            t->tasks[index].copies--;
    }
    lane->queued = 0;
}

static int save_at(int fd, const char *buf, size_t length,
                   unsigned long long offset)
{
    if(lseek(fd, (off_t)offset, SEEK_SET) < 0)
        return -1;
    while(length > 0) {
        ssize_t n = write(fd, buf, length);
        if(n < 0 && errno == EINTR)
            continue;
        if(n <= 0)
            return -1;
        buf += n;
        length -= (size_t)n;
    }
    return 0;
}

static int dispatch(struct lane *lane, int lane_index, struct transfer *t)
{
    if(!lane->ready || lane->sock < 0)
        return 0;
    if(lane->request_sent == lane->request_length)
        lane->request_length = lane->request_sent = 0;
    while(lane->queued < t->pipeline) {
        size_t index;
        int hedged = 0;
        for(index = 0; index < t->task_count; index++)
            if(!t->tasks[index].done && t->tasks[index].copies == 0)
                break;
        if(index == t->task_count && t->next_offset < t->goal) {
            if(t->task_count == t->capacity) {
                size_t capacity = t->capacity ? t->capacity * 2 : 256;
                struct task *tasks = realloc(t->tasks, capacity * sizeof(*tasks));
                if(tasks == NULL)
                    return -1;
                t->tasks = tasks;
                t->capacity = capacity;
            }
            struct task *task = &t->tasks[t->task_count++];
            memset(task, 0, sizeof(*task));
            task->offset = t->next_offset;
            task->length = t->goal - t->next_offset;
            if(task->length > t->chunk)
                task->length = t->chunk;
            t->next_offset += task->length;
        } else if(index == t->task_count) {
            if(!t->hedge || lane->queued != 0)
                break;
            double oldest = mp_now() - 0.2;
            size_t candidate = t->task_count;
            for(size_t j = 0; j < t->task_count; j++) {
                struct task *task = &t->tasks[j];
                if(!task->done && task->copies == 1 && task->owner != lane_index &&
                   task->assigned < oldest) {
                    oldest = task->assigned;
                    candidate = j;
                }
            }
            if(candidate == t->task_count)
                break;
            index = candidate;
            hedged = 1;
        } else {
            t->retries++;
        }
        struct task *task = &t->tasks[index];
        char line[MP_LINE];
        int length = snprintf(line, sizeof(line), "GET %llu %llu\n", task->offset, task->length);
        if(length < 0 || lane->request_length + (size_t)length > sizeof(lane->request))
            return -1;
        memcpy(lane->request + lane->request_length, line, (size_t)length);
        lane->request_length += (size_t)length;
        int slot = (lane->head + lane->queued) % MAX_PIPELINE;
        lane->queue[slot] = index;
        lane->received[slot] = 0;
        lane->queued++;
        if(task->copies == 0) {
            task->assigned = mp_now();
            task->owner = lane_index;
        }
        task->copies++;
        t->requests++;
        if(hedged) {
            t->hedges++;
            break;
        }
    }
    return 0;
}

static int receive_file(struct lane *lane, struct transfer *t, double *first_data)
{
    char buf[MP_BUFFER];
    ssize_t n = read(lane->sock, buf, sizeof(buf));
    if(n < 0)
        return errno == EAGAIN || errno == EINTR ? 0 : -1;
    if(n == 0)
        return -1;
    lane->last_data = mp_now();
    size_t position = 0, length = (size_t)n;
    if(!lane->ready) {
        while(position < length) {
            char ch = buf[position++];
            if(lane->header_length + 1 >= sizeof(lane->header))
                return -1;
            lane->header[lane->header_length++] = ch;
            if(ch == '\n') {
                unsigned long long size;
                char extra;
                lane->header[lane->header_length] = '\0';
                if(sscanf(lane->header, "SIZE %llu %c", &size, &extra) != 1 ||
                   size > (unsigned long long)LLONG_MAX ||
                   (t->known && size != t->file_size))
                    return -1;
                if(!t->known) {
                    t->file_size = size;
                    t->goal = t->limit && t->limit < size ? t->limit : size;
                    t->known = 1;
                }
                lane->ready = 1;
                break;
            }
        }
    }
    while(position < length) {
        if(lane->queued == 0)
            return -1;
        if(*first_data == 0)
            *first_data = mp_now();
        int slot = lane->head;
        struct task *task = &t->tasks[lane->queue[slot]];
        size_t count = length - position;
        unsigned long long left = task->length - lane->received[slot];
        if(left < count)
            count = (size_t)left;
        if(!task->done && save_at(t->output, buf + position, count,
                                  task->offset + lane->received[slot]) < 0)
            return -2;
        if(task->done)
            t->duplicate_bytes += count;
        t->wire_bytes += count;
        lane->bytes += count;
        lane->received[slot] += count;
        position += count;
        if(lane->received[slot] == task->length) {
            if(!task->done) {
                task->done = 1;
                t->complete += task->length;
                lane->useful += task->length;
            }
            task->copies--;
            lane->chunks++;
            lane->head = (lane->head + 1) % MAX_PIPELINE;
            lane->queued--;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct transfer transfer;
    struct lane lanes[MAX_LANES];
    struct epoll_event events[MP_EVENTS];
    char *paths[MAX_LANES], temporary[4096];
    int path_count = 0, per_path = 4, lane_count = 0, epfd = -1, status = 1;
    double started = mp_now(), first_data = 0, finished;
    memset(&transfer, 0, sizeof(transfer));
    transfer.output = -1;
    transfer.chunk = 1024 * 1024;
    transfer.pipeline = 4;
    transfer.hedge = 1;
    if(argc < 3)
        goto usage;
    for(int i = 2; i < argc; i++) {
        if(strcmp(argv[i], "--no-hedge") == 0) {
            transfer.hedge = 0;
        } else if(strcmp(argv[i], "--lanes") == 0 ||
                  strcmp(argv[i], "--pipeline") == 0 ||
                  strcmp(argv[i], "--chunk") == 0 || strcmp(argv[i], "--bytes") == 0) {
            char *option = argv[i];
            if(++i == argc)
                goto usage;
            unsigned long long value = number(argv[i]);
            if(value == 0 || value > (unsigned long long)LLONG_MAX)
                goto usage;
            if(strcmp(option, "--lanes") == 0) {
                if(value > MAX_LANES)
                    goto usage;
                per_path = (int)value;
            } else if(strcmp(option, "--pipeline") == 0) {
                if(value > MAX_PIPELINE)
                    goto usage;
                transfer.pipeline = (int)value;
            } else if(strcmp(option, "--chunk") == 0) {
                if(value < 4096 || value > 16 * 1024 * 1024)
                    goto usage;
                transfer.chunk = value;
            } else {
                transfer.limit = value;
            }
        } else {
            if(path_count == MAX_LANES || argv[i][0] == '-')
                goto usage;
            paths[path_count++] = argv[i];
        }
    }
    if(path_count == 0 || path_count * per_path > MAX_LANES)
        goto usage;
    struct stat existing;
    if(lstat(argv[1], &existing) == 0 || errno != ENOENT) {
        fprintf(stderr, "output already exists or cannot be checked: %s\n", argv[1]);
        return 1;
    }
    if(snprintf(temporary, sizeof(temporary), "%s.part.%ld", argv[1], (long)getpid()) >=
       (int)sizeof(temporary))
        return 1;
    transfer.output = open(temporary, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if(transfer.output < 0) {
        perror("open output");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    epfd = epoll_create(MP_EVENTS);
    if(epfd < 0)
        goto cleanup;

    /* STEP 1: 各経路へ複数のTCP接続を作る */
    for(int p = 0; p < path_count; p++) {
        char endpoint[272], *colon;
        if(strlen(paths[p]) >= sizeof(endpoint))
            goto cleanup;
        strcpy(endpoint, paths[p]);
        colon = strrchr(endpoint, ':');
        if(colon == NULL || mp_port(colon + 1) < 0)
            goto cleanup;
        *colon++ = '\0';
        if(strlen(endpoint) >= sizeof(lanes[0].host))
            goto cleanup;
        for(int j = 0; j < per_path; j++) {
            struct lane *lane = &lanes[lane_count];
            memset(lane, 0, sizeof(*lane));
            strcpy(lane->host, endpoint);
            strcpy(lane->port, colon);
            lane->path = p;
            lane->started = lane->last_data = mp_now();
            lane->sock = mp_connect(lane->host, lane->port);
            lane->connecting = 1;
            if(lane->sock >= 0 &&
               mp_event(epfd, EPOLL_CTL_ADD, lane->sock, EPOLLOUT, lane_count) < 0) {
                close(lane->sock);
                lane->sock = -1;
            }
            lane_count++;
        }
    }

    /* STEP 2: 読み書きできる接続をepollで調べる */
    while(!transfer.known || transfer.complete < transfer.goal) {
        int active = 0;
        for(int i = 0; i < lane_count; i++) {
            struct lane *lane = &lanes[i];
            if(lane->sock < 0)
                continue;
            double idle = mp_now() - lane->last_data;
            if((lane->connecting && idle > 5.0) || idle > 30.0) {
                fprintf(stderr, "timeout path=%d lane=%d\n", lane->path, i);
                drop_lane(lane, &transfer);
                continue;
            }
            if(dispatch(lane, i, &transfer) < 0)
                goto cleanup;
            uint32_t mask = EPOLLIN | EPOLLRDHUP;
            if(lane->connecting || lane->request_sent < lane->request_length)
                mask |= EPOLLOUT;
            if(mp_event(epfd, EPOLL_CTL_MOD, lane->sock, mask, i) < 0)
                goto cleanup;
            active++;
        }
        if(active == 0) {
            fprintf(stderr, "all paths failed before completing the file\n");
            goto cleanup;
        }
        int nfds = epoll_wait(epfd, events, MP_EVENTS, 50);
        if(nfds < 0) {
            if(errno == EINTR)
                continue;
            perror("epoll_wait");
            goto cleanup;
        }
        for(int i = 0; i < nfds; i++) {
            struct lane *lane = &lanes[events[i].data.u32];
            if(lane->sock < 0)
                continue;
            if(lane->connecting) {
                if(mp_connected(lane->sock) < 0) {
                    drop_lane(lane, &transfer);
                    continue;
                }
                lane->connecting = 0;
            }
            if(events[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
                /* STEP 3: 届いた部分を元の位置へ保存する */
                int result = receive_file(lane, &transfer, &first_data);
                if(result == -2) {
                    perror("write output");
                    goto cleanup;
                }
                if(result < 0) {
                    drop_lane(lane, &transfer);
                    continue;
                }
            }
            if(events[i].events & EPOLLERR) {
                drop_lane(lane, &transfer);
                continue;
            }
            /* STEP 4: 空いた接続へ次の範囲を割り当てる */
            if(dispatch(lane, (int)events[i].data.u32, &transfer) < 0)
                goto cleanup;
            if(lane->request_sent < lane->request_length) {
                ssize_t n = write(lane->sock, lane->request + lane->request_sent,
                                  lane->request_length - lane->request_sent);
                if(n < 0 && errno != EAGAIN && errno != EINTR)
                    drop_lane(lane, &transfer);
                else if(n > 0)
                    lane->request_sent += (size_t)n;
            }
        }
    }

    /* STEP 5: ファイルの完成を確認し、時間と速度を出力する */
    if(ftruncate(transfer.output, (off_t)transfer.goal) < 0)
        goto cleanup;
    int result = close(transfer.output);
    transfer.output = -1;
    if(result < 0 || link(temporary, argv[1]) < 0) {
        perror("finish output");
        goto cleanup;
    }
    unlink(temporary);
    finished = mp_now();
    double elapsed = finished - started;
    double data_time = first_data > 0 ? finished - first_data : elapsed;
    for(int p = 0; p < path_count; p++) {
        unsigned long long bytes = 0, useful = 0;
        unsigned long chunks = 0;
        for(int j = 0; j < lane_count; j++)
            if(lanes[j].path == p) {
                bytes += lanes[j].bytes;
                useful += lanes[j].useful;
                chunks += lanes[j].chunks;
            }
        printf("PATH %d %s bytes=%llu useful=%llu chunks=%lu\n",
               p, paths[p], bytes, useful, chunks);
    }
    printf("RESULT {\"file_bytes\":%llu,\"receive_bytes\":%llu,\"wire_bytes\":%llu,"
           "\"time_seconds\":%.9f,\"data_seconds\":%.9f,\"goodput_mbps\":%.6f,"
           "\"data_mbps\":%.6f,\"paths\":%d,\"lanes_per_path\":%d,\"requests\":%lu,"
           "\"retries\":%lu,\"hedges\":%lu,\"late_duplicate_bytes\":%llu}\n",
           transfer.file_size, transfer.complete, transfer.wire_bytes, elapsed, data_time,
           (double)transfer.complete * 8.0 / elapsed / 1e6,
           data_time > 0 ? (double)transfer.complete * 8.0 / data_time / 1e6 : 0,
           path_count, per_path, transfer.requests, transfer.retries,
           transfer.hedges, transfer.duplicate_bytes);
    status = 0;
cleanup:
    for(int i = 0; i < lane_count; i++)
        drop_lane(&lanes[i], &transfer);
    if(transfer.output >= 0)
        close(transfer.output);
    if(status != 0)
        unlink(temporary);
    if(epfd >= 0)
        close(epfd);
    free(transfer.tasks);
    return status;
usage:
    fprintf(stderr, "Usage: %s output_file [--lanes N] [--pipeline N] [--chunk bytes] "
                    "[--bytes N] [--no-hedge] host:port ...\n", argv[0]);
    return 1;
}
