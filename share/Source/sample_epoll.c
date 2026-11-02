/*                                                                  */
/*  FILENAME     :  sample_epoll.c                                  */
/*  DESCRIPTION  :  epollをもちいてタイムアウト付で標準入力から読み込む   */
/*  DATE         :  Sep. 01, 2020                                   */
/*  UPDATE       :                                                  */
/*                                                                  */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/epoll.h>

#define MAX_EVENTS 10
#define MAX_LEN 256

int epoll_ctl_add_in(int epfd, int fd);

int epoll_ctl_add_in(int epfd, int fd)
{
    struct epoll_event ev; /* イベント */

    memset(&ev, 0, sizeof(ev)); /* 0クリア */
    ev.events = EPOLLIN;        /* read()可能というイベント */
    ev.data.fd = fd;            /* 関連付けるfd */
    if(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct epoll_event events[MAX_EVENTS]; /* イベントとイベントの配列 */
    int fd = 0;     /* 標準入力 */
    int epfd;       /* epollインスタンスのファイルデスクリプタ */
    int nfds;       /* 要求するI/Oが可能なｆｄの数 */
    int i;
    int n;

    char buf[MAX_LEN];
    int cnt = 0;

    /* STEP 1: epollインスタンスの作成 */
    epfd = epoll_create(MAX_EVENTS);
    if(epfd < 0) {
        perror("epoll_create");
        return 1;
    }

    /* STEP 2: 監視するfdとイベントを登録 */
    if(epoll_ctl_add_in(epfd, fd) != 0) {
        perror("epoll_ctrl_add_in");
        return 1;
    }
       
    for( ; ; ) {

        /* STEP 3: イベント発生をを待つ */
        /* nfdsには処理が可能になったイベント数が返される */
        nfds = epoll_wait(epfd, events, MAX_EVENTS, 3000);
        if(nfds < 0) {
            perror("epoll_wait");
            return 1;
        }

        /* タイムアウト */
        if(nfds == 0) {
            printf("timeout %d\n", cnt++);
            continue;
        }
        
        /* 処理可能なデスクリプタが events[] 登録されている */
        /* STEP 4: 順次確認して必要な処理を行う */
        for(i = 0; i < nfds; i++) {
            if(events[i].data.fd == fd) {
                n = read(0, buf, MAX_LEN);
                if(n > 0)
                    write(0, buf, n);
            }
        }
        if(strncmp(buf, "quit", 4) == 0)
            break;
    }

    return 0;
}

/* Local Variables: */
/* compile-command: "gcc sample_epoll.c -o sample_epoll.out" */
/* End: */
