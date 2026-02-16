#ifndef WATCH_UDP_H
#define WATCH_UDP_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int port;                 // UDP listen port (default 5005)
    const char* bind_ip;      // "0.0.0.0"
    const char* fifo_path;    // "/tmp/th_fifo"
    int max_devices;          // e.g. 64
    int log_raw;              // 1이면 RAW 수신 로그 출력
} WatchUdpConfig;

/**
 * 워치 UDP(JSON) 수신을 시작하고,
 * deviceId별 캐시(heartRate, skin_temperature, last_ts)를 유지하면서
 * 매 패킷마다 FIFO에 아래 JSON 1줄을 출력한다:
 * {"deviceId":"..","ts":"..","heartRate":..,"skin_temperature":..}
 *
 * - 이 함수는 무한 루프(블로킹)로 동작한다.
 * - 종료는 SIGINT(Ctrl+C) 등 외부에서 처리.
 *
 * return: 0 정상(사실상 무한루프), <0 에러
 */
int watch_udp_run(const WatchUdpConfig* cfg);

#ifdef __cplusplus
}
#endif

#endif