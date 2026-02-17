#include <stdio.h>
#include <mqueue.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>

#include "common.h"
#include "th_module.h"

int main(void) {

    if (th_module_init("192.168.0.20", 8887) != 0) {
        printf("초기화 실패\n");
        return 1;
    }

    // 이거 큐가 존재해야 성공함 아니면 자동으로 꺼질거야
    mqd_t mq = mq_open(TH_QUEUE_NAME, O_WRONLY);
    if (mq == (mqd_t)-1) {
        perror("MQ 열기 실패");
        return 1;
    }

    while (1) {
        THData d = th_module_read_once();

        THMsg msg;
        msg.temperature = d.temperature;
        msg.humidity = d.humidity;
        msg.error_code = d.error_code;
        msg.sys_errno = d.sys_errno;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        msg.ts_ms = (uint64_t)ts.tv_sec * 1000ULL +
                    (uint64_t)(ts.tv_nsec / 1000000ULL);

        if (mq_send(mq, (char*)&msg, sizeof(msg), 0) == -1) {
            perror("mq_send 실패");
        }

        sleep(5);
    }

    mq_close(mq);
    th_module_close();
    return 0;
}
