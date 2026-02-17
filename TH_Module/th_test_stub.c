// consumer.c
#include <mqueue.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "common.h"

int main(void) {
    // 큐 수신
    mqd_t q = mq_open(TH_QUEUE_NAME, O_RDONLY);
    if (q == (mqd_t)-1) {
        fprintf(stderr, "mq_open(consumer) failed: %s\n", strerror(errno));
        return 1;
    }

    // 큐 속성 조회: msgsize 알아내서 그 크기만큼 버퍼 준비
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

    unsigned int prio = 0;
    ssize_t n = mq_receive(q, buf, attr.mq_msgsize, &prio);
    if (n == -1) {
        fprintf(stderr, "mq_receive failed: %s\n", strerror(errno));
        break;
    }

    if (n != (ssize_t)sizeof(THMsg)) {
        fprintf(stderr, "unexpected message size: %zd\n", n);
        continue;
    }

    THMsg msg;
    memcpy(&msg, buf, sizeof(msg));

    printf("[RECV] prio=%u | t=%.1f h=%.1f err=%d errno=%d ts=%llu\n",
            prio, msg.temperature, msg.humidity, msg.error_code, msg.sys_errno,
            (unsigned long long)msg.ts_ms);
    
    mq_close(q);
    
    return 0;
}
