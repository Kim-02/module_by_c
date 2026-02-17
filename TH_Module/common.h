#pragma once
#include <stdint.h>

#define TH_QUEUE_NAME "/th_sensor_q" //TODO POSIX MQ에 사용할 큐 이름

// 메시지 구조체
typedef struct {
  float temperature;
  float humidity;
  int error_code;
  int sys_errno;
  uint64_t ts_ms; // ts
} THMsg;
