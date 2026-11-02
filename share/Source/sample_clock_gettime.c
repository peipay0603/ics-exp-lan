/*                                                                  */
/*  FILENAME     :  sample_clock_gettime.c                          */
/*  DESCRIPTION  :  usleep()による休止                              */
/*                  clock_gettime()を用いた時間計測                  */
/*  DATE         :  Sep. 01, 2020                                   */
/*  UPDATE       :                                                  */
/*                                                                  */

#include <stdio.h>
#include <unistd.h>
#include <time.h>
/* #include <sys/time.h> */

int main(void)
{
    unsigned int sec;       /* 秒 */
    int nsec;               /* ナノ秒 */
    double time;            /* 時間 */
    unsigned int w_usec;    /* 待ち時間（マイクロ秒単位） */

    struct timespec start_time, end_time;

    /* 最小測定時間: 何も処理しない場合 */
    printf("最小測定時間\n");
        
    /* 処理前の時刻計測 */
    clock_gettime(CLOCK_REALTIME, &start_time);

    /* 経過時間を測定したい処理 */
    /* usleep(w_usec); */

    /* 処理後の時刻計測 */
    clock_gettime(CLOCK_REALTIME, &end_time);

    /* 経過時間の計算 */
    sec = end_time.tv_sec - start_time.tv_sec;
    nsec = end_time.tv_nsec - start_time.tv_nsec;

    /* time = (double)sec + (double)nsec / (1000 * 1000 * 1000); */
    time = (double)sec + (double)nsec * 1e-9;

    printf("0 time: %f\n", time);

    /* ----------------------------------*/
    /* 1msecのスリープ */
    w_usec = 1e3;   /* 1,000 usec */
    printf("\n");
    printf("usleep: %u usec\n", w_usec);
        /* 処理前の時刻計測 */
    clock_gettime(CLOCK_REALTIME, &start_time);

    /* 時間計n測をする処理 */
    usleep(w_usec);

    /* 処理後の時刻計測 */
    clock_gettime(CLOCK_REALTIME, &end_time);

    /* 経過時間の計算 */
    sec = end_time.tv_sec - start_time.tv_sec;
    nsec = end_time.tv_nsec - start_time.tv_nsec;

    time = (double)sec + (double)nsec * 1e-9;

    printf("time: %f\n", time);

    /* ----------------------------------*/
    /* 1秒のスリープ */
    printf("\n");
    printf("sleep: 1 sec\n");

    /* 処理前の時刻計測 */
    clock_gettime(CLOCK_REALTIME, &start_time);

    /* 時間計測をする処理 */
    sleep(1);

    /* 処理後の時刻計測 */
    clock_gettime(CLOCK_REALTIME, &end_time);

    /* 経過時間の計算 */
    sec = end_time.tv_sec - start_time.tv_sec;
    nsec = end_time.tv_nsec - start_time.tv_nsec;

    time = (double)sec + (double)nsec * 1e-9;

    printf("time: %f\n", time);

    return 0;
}

/* Local Variables: */
/* compile-command: "gcc sample_clock_gettime.c -o test_clock.out -lrt" */
/* End: */
