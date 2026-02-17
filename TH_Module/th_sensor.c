#include <modbus/modbus.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/time.h>
#include <mqueue.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "th_sensor.h"

// 전역 변수로 메시지큐 핸들러 관리
static mqd_t g_mq = (mqd_t)-1;

// ================================
// 내부 상태(전역)
// ================================
static modbus_t *ctx = NULL;

static char g_ip[64] = {0};
static int  g_port = 0;

// 센서/네트워크 환경에 맞게 조절 가능
static const int   SLAVE_ID = 1; //TODO 이 부분은 BT-NB114의 온습도계 번호와 맞아야 함 확인 필요
// 그 BT-NB114 보면 버튼 있는데 그거 8번 버튼 켜져 있으면 1번 맞을거야, 내가 8번 켜두고 써서 아마 안바꿨으면 1 맞아
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
        data.sys_errno = 0; // TODO 이 부분 왜 0인지? 설계대로라면 에러 코드마다 넘버가 따로 있는게 좋을듯
        // ㄴㄴ 이거 0이 통신 성공이라 오류면 에러코드 errno 출력함
        return data;
    }

    uint16_t reg[REG_CNT];

    // 1) 1차 read
    int rc = modbus_read_input_registers(ctx, REG_ADDR, REG_CNT, reg);

    // 2) 실패하면 soft reconnect 1회 + 재시도
    //TODO 복구 로직이 꼭 필요한지? 무결성 검증 후에 다시 반복을 한다던지 시간을 측정해봐야할 듯
    // 이거 우리 랩실은 문제될거 없어보이는데 좀 더 큰 환경(작업장)에서 통신 장애나 변수에 도움되라고 넣어둔거 지금 당장 테스트엔 필요없음 복구 로직
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

    // 7) 무결성 체크 및 일시적 노이즈 재시도
    if (!validate_range(t, h)) {
        // 일시적인 튐 현상일 수 있으므로 0.05초 대기 후 딱 한 번만 더 읽어봄
        usleep(50000); 
        if (modbus_read_input_registers(ctx, REG_ADDR, REG_CNT, reg) == REG_CNT) {
            t = reg[0] / 10.0f;
            h = reg[1] / 10.0f;
        }

        // 재시도 후에도 범위를 벗어나면 최종 에러 처리
        if (!validate_range(t, h)) {
            data.error_code = TH_ERR_BAD_VALUE;
            data.temperature = t;
            data.humidity = h;
            data.sys_errno = 0; // 통신 자체는 성공했으므로 OS 에러는 없음
            return data;
        }
    }

    // 모든 검증 통과 시 데이터 확정
    data.temperature = t;
    data.humidity = h;
    data.error_code = TH_OK;
    data.sys_errno = 0;
    return data;
}

void th_close(void) { //TODO 이 부분 MQ를 정리하는 코드 추가 필요
    if (ctx) {
        modbus_close(ctx);
        modbus_free(ctx);
        ctx = NULL;
    }

    // 메시지큐 핸들러 닫기
    if (g_mq != (mqd_t)-1) {
        mq_close(g_mq);
        g_mq = (mqd_t)-1;
    }
}

// 실행부
int main(void) {
    // 1. 센서 초기화 (IP와 포트는 환경에 맞게 설정)
    if (th_init("192.168.0.20", 8887) != 0) {
        fprintf(stderr, "센서 초기화 실패\n");
        return 1;
    }

    // 2. MQ 열기 (쓰기 전용)
    // 허브가 이미 큐 생성했다고 가정
    g_mq = mq_open(TH_QUEUE_NAME, O_WRONLY);
    if (g_mq == (mqd_t)-1) {
        perror("메시지큐 열기 실패 (Hub가 실행 중인지 확인)");
        th_close();
        return 1;
    }

    printf("🚀 온습도 수집 모듈 가동 (전송 주기: 5초)\n");

    while (1) {
        // 데이터 한 번 읽기
        THData data = th_read_once();

        // MQ 전송용 구조체에 데이터 복사
        THMsg msg;
        msg.temperature = data.temperature;
        msg.humidity = data.humidity;
        msg.error_code = data.error_code;
        msg.sys_errno = data.sys_errno;
        msg.ts_ms = (uint64_t)time(NULL) * 1000;

        // 3. MQ로 전송
        if (mq_send(g_mq, (const char*)&msg, sizeof(msg), 0) == -1) {
            perror("MQ 전송 실패");
        } else {
            if (data.error_code == 0) {
                printf("[SENT] %.1f°C / %.1f%%\n", msg.temperature, msg.humidity);
            }
        }

        sleep(5); // 5초 대기
    }

    th_close();
    return 0;
}


//TODO 구현한 함수들이 정상적으로 동작해서 MQ로 전송할 수 있도록 실행부 추가 필요 
