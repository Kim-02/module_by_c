#ifndef VITAL_MODULE_H
#define VITAL_MODULE_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int port;                 // UDP listen port (default 5005)
    const char* bind_ip;      // "0.0.0.0"
    int max_devices;          // e.g. 64
    int log_raw;              // 1이면 RAW 수신 로그 출력
} WatchUdpConfig;

/**
 * 워치 UDP(JSON) 수신 루프.
 * - deviceId별로 HR/SKIN_TEMP 캐시 유지
 * - 매 패킷마다 WatchMsg(구조체)로 MQ(/mq_watch)에 전송
 *
 * return: 0 정상 종료(보통 SIGINT로 빠져나옴), <0 에러
 */
int watch_udp_run(const WatchUdpConfig* cfg);

#ifdef __cplusplus
}
#endif

#endif
