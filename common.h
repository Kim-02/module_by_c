#pragma once
#include <stdint.h>

#define TH_QUEUE_NAME "/mq_th"
#define WQTCH_QUEUE_NAME "/mq_vital"

// 온습도
typedef struct {
  float temperature;
  float humidity;
  int error_code;
  int sys_errno;
  uint64_t ts_ms; // ts
} THMsg;

// 워치
typedef struct {
    char deviceId[64];
    double heartRate;
    double skin_temperature;
    int has_hr; // 심박수 포함 여부
    int has_st; // 체온 포함 여부
    uint64_t ts_ms;
} WatchMsg;
