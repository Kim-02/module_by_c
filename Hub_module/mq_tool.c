#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mqueue.h>
#include "common.h" // 큐 이름과 구조체 크기를 땡겨옴

void print_usage() {
    printf("Usage: ./mq_tool [init|clean]\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_curmsgs = 0;

    if (strcmp(argv[1], "init") == 0) {
        // 1. 온습도용 큐 생성
        attr.mq_msgsize = sizeof(THMsg);
        mqd_t q1 = mq_open(TH_QUEUE_NAME, O_RDWR | O_CREAT, 0666, &attr);
        
        // 2. 워치용 큐 생성
        attr.mq_msgsize = sizeof(WatchMsg);
        mqd_t q2 = mq_open(WATCH_QUEUE_NAME, O_RDWR | O_CREAT, 0666, &attr);

        if (q1 != (mqd_t)-1 && q2 != (mqd_t)-1) {
            printf("✅ MQ Created: %s (size: %ld)\n", TH_QUEUE_NAME, sizeof(THMsg));
            printf("✅ MQ Created: %s (size: %ld)\n", WATCH_QUEUE_NAME, sizeof(WatchMsg));
        } else {
            perror("❌ MQ Creation Failed");
        }
        mq_close(q1);
        mq_close(q2);

    } else if (strcmp(argv[1], "clean") == 0) {
        // 기존 큐 삭제 (초기화용)
        mq_unlink(TH_QUEUE_NAME);
        mq_unlink(WATCH_QUEUE_NAME);
        printf("🧹 All MQs unlinked (cleaned).\n");
    } else {
        print_usage();
    }

    return 0;
}
