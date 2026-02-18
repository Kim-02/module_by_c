#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>  // mq_ 관련 함수를 위해 필수
#include <string.h>
#include <time.h>
#include "common.h"

int main(void) {
    // 1. 큐 열기
    mqd_t q = mq_open(TH_QUEUE_NAME, O_RDONLY);
    if (q == (mqd_t)-1) {
        fprintf(stderr, "mq_open(consumer) failed: %s\n", strerror(errno));
        return 1;
    }

    // 2. 큐 속성 조회
    struct mq_attr attr;
    if (mq_getattr(q, &attr) == -1) {
        fprintf(stderr, "mq_getattr failed: %s\n", strerror(errno));
        mq_close(q);
        return 1;
    }

    char buf[8192];
    if ((long)sizeof(buf) < attr.mq_msgsize) {
        fprintf(stderr, "buffer too small: need %ld bytes\n", attr.mq_msgsize);
        mq_close(q);
        return 1;
    }

    printf("Waiting for messages on %s...\n", TH_QUEUE_NAME);

    // 3. 무한 루프 추가 (데이터를 계속 받기 위해)
    while (1) {
        unsigned int prio = 0;
        ssize_t n = mq_receive(q, buf, attr.mq_msgsize, &prio);
        
        if (n == -1) {
            fprintf(stderr, "mq_receive failed: %s\n", strerror(errno));
            break; // 에러 발생 시 루프 탈출
        }

        if (n != (ssize_t)sizeof(THMsg)) {
            fprintf(stderr, "unexpected message size: %zd\n", n);
            continue; // 다음 메시지 기다리기
        }

        THMsg msg;
        memcpy(&msg, buf, sizeof(msg));

        // 시간 형식 yy-MM-dd HH:mm:ss 형식 맞췄음 (워치에서 이렇게 보냄 TS)
        time_t raw_time = (time_t)(msg.ts_ms / 1000);
        struct tm *lt = localtime(&raw_time);
        char formatted_time[20];
        strftime(formatted_time, sizeof(formatted_time), "%y-%m-%d %H:%M:%S", lt);

        printf("[RECV] prio=%u | t=%.1f h=%.1f err=%d errno=%d | 시간:%s \n",
                prio, msg.temperature, msg.humidity, msg.error_code, msg.sys_errno,
                formatted_time);
    }

    mq_close(q);
    return 0;
}
