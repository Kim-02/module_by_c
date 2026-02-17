#pragma once
#include <stdint.h>

// 큐 이름 정의
#define WQTCH_QUEUE_NAME "/mq_vital"

// 워치 데이터 구조체 (사용자님의 캐시 로직 반영)
typedef struct {
    char deviceId[64];
    double heartRate;
    double skin_temperature;
    int has_hr; // 심박수 포함 여부
    int has_st; // 체온 포함 여부
    uint64_t ts_ms;
} WatchMsg;
