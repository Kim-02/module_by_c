#include <modbus/modbus.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/time.h>

#include "th_sensor.h"

// ================================
// 내부 상태(전역)
// ================================
static modbus_t *ctx = NULL;

static char g_ip[64] = {0};
static int  g_port = 0;

// 센서/네트워크 환경에 맞게 조절 가능
static const int   SLAVE_ID = 1;
static const int   REG_ADDR = 0;
static const int   REG_CNT  = 2;

// 무결성 체크 범위(필요하면 센서 스펙에 맞춰 조정)
// 예: 산업용 온습도 센서 흔한 범위
static const float TEMP_MIN = -40.0f;
static const float TEMP_MAX =  85.0f;
static const float HUMI_MIN =   0.0f;
static const float HUMI_MAX = 100.0f;

// 타임아웃(무선 환경 고려)
static const int TIMEOUT_SEC = 1;
static const int TIMEOUT_USEC = 0;

// ================================
// 내부 유틸
// ================================
static int validate_range(float t, float h) {
    if (t < TEMP_MIN || t > TEMP_MAX) return 0;
    if (h < HUMI_MIN || h > HUMI_MAX) return 0;
    return 1;
}

static void apply_common_options(modbus_t *c) {
    // Slave ID 설정
    modbus_set_slave(c, SLAVE_ID);

    // 응답 타임아웃 설정
    struct timeval tv;
    tv.tv_sec = TIMEOUT_SEC;
    tv.tv_usec = TIMEOUT_USEC;
    modbus_set_response_timeout(c, tv.tv_sec, tv.tv_usec);
}

// ctx를 유지한 채로 재연결(가벼운 복구)
static int soft_reconnect(void) {
    if (!ctx) return -1;

    // 기존 연결 닫고 다시 연결
    modbus_close(ctx);
    if (modbus_connect(ctx) == -1) {
        return -1;
    }

    // 재연결 후 옵션 재적용(안전)
    apply_common_options(ctx);
    return 0;
}

// ctx 자체를 새로 만드는 복구(무거운 복구)
static int hard_recreate(void) {
    if (g_ip[0] == '\0' || g_port <= 0) return -1;

    if (ctx) {
        modbus_close(ctx);
        modbus_free(ctx);
        ctx = NULL;
    }

    ctx = modbus_new_tcp(g_ip, g_port);
    if (!ctx) return -1;

    apply_common_options(ctx);

    if (modbus_connect(ctx) == -1) {
        modbus_free(ctx);
        ctx = NULL;
        return -1;
    }
    return 0;
}

// ================================
// 외부 API
// ================================
int th_init(const char* ip, int port) {
    if (!ip || port <= 0) return -1;

    // ip/port 저장(하드 재생성용)
    snprintf(g_ip, sizeof(g_ip), "%s", ip);
    g_port = port;

    // 이미 ctx가 있다면 정리 후 재생성
    if (ctx) {
        th_close();
    }

    ctx = modbus_new_tcp(g_ip, g_port);
    if (!ctx) {
        return -1;
    }

    apply_common_options(ctx);

    if (modbus_connect(ctx) == -1) {
        modbus_free(ctx);
        ctx = NULL;
        return -1;
    }

    return 0;
}

THData th_read_once(void) {
    THData data;
    data.temperature = 0.0f;
    data.humidity = 0.0f;
    data.error_code = TH_OK;
    data.sys_errno = 0;

    if (!ctx) {
        data.error_code = TH_ERR_NOT_INIT;
        data.sys_errno = 0;
        return data;
    }

    uint16_t reg[REG_CNT];

    // 1) 1차 read
    int rc = modbus_read_input_registers(ctx, REG_ADDR, REG_CNT, reg);

    // 2) 실패하면 soft reconnect 1회 + 재시도
    if (rc != REG_CNT) {
        data.sys_errno = errno;

        if (soft_reconnect() == 0) {
            rc = modbus_read_input_registers(ctx, REG_ADDR, REG_CNT, reg);
        }
    }

    // 3) 그래도 실패하면 hard recreate 1회 + 재시도
    if (rc != REG_CNT) {
        data.sys_errno = errno;

        if (hard_recreate() == 0) {
            rc = modbus_read_input_registers(ctx, REG_ADDR, REG_CNT, reg);
        }
    }

    // 4) 최종 실패 처리
    if (rc != REG_CNT) {
        data.error_code = TH_ERR_READ_FAIL;
        // errno 갱신(최신 실패 기준)
        data.sys_errno = errno;
        return data;
    }

    // 5) 스케일링
    float t = reg[0] / 10.0f;
    float h = reg[1] / 10.0f;

    // 6) 무결성 체크
    if (!validate_range(t, h)) {
        data.error_code = TH_ERR_BAD_VALUE;
        data.temperature = t;
        data.humidity = h;
        data.sys_errno = 0;  // 이건 통신 실패가 아니라 값 이상이므로 errno 의미 없음
        return data;
    }

    data.temperature = t;
    data.humidity = h;
    data.error_code = TH_OK;
    data.sys_errno = 0;
    return data;
}

void th_close(void) {
    if (ctx) {
        modbus_close(ctx);
        modbus_free(ctx);
        ctx = NULL;
    }
}