#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <mqueue.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "vital_module.h"
#include "common.h"

#define DEFAULT_PORT 5005
#define MAX_DEVICES 64

// 전역: 시그널 종료 제어 + MQ 핸들
static volatile sig_atomic_t g_keep_running = 1;
static mqd_t g_watch_mq = (mqd_t)-1;

typedef struct {
    int used;
    char deviceId[DEV_ID_LEN];
    char last_ts[TS_LEN]
    double heartRate;
    double skin_temperature;
    int has_hr;
    int has_st;    
} DeviceCache;

// 컨트롤 C로 루프 종료
void handle_sigint(int sig) {
    (void)sig;
    g_keep_running = 0;
}

// JSON 유틸 함수
static int json_get_string(cJSON* obj, const char* key, char* out, size_t outsz) {
    cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(it) || it->valuestring == NULL) return 0;
    strncpy(out, it->valuestring, outsz - 1);
    out[outsz - 1] = '\0';
    return 1;
}

static int json_get_number(cJSON* obj, const char* key, double* out) {
    cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) {
        *out = it->valuedouble;
        return 1;
    }
    return 0;
}

// 디바이스 아이디 슬롯 찾기/생성
static int find_or_create_slot(DeviceCache* cache, int max_dev, const char* deviceId) {
    // 64대 규모면 선형 탐색으로도 충분 (병목 거의 없음)
    for (int i = 0; i < max_dev; i++) {
        if (cache[i].used && strncmp(cache[i].deviceId, deviceId, DEV_ID_LEN) == 0) return i;
    }

    for (int i = 0; i < max_dev; i++) {
        if (!cache[i].used) {
            // 새 슬롯 명시 초기화 (A 개선)
            cache[i].used = 1;
            strncpy(cache[i].deviceId, deviceId, DEV_ID_LEN - 1);
            cache[i].deviceId[DEV_ID_LEN - 1] = '\0';

            cache[i].last_ts[0] = '\0';
            cache[i].has_hr = 0;
            cache[i].has_st = 0;
            cache[i].heartRate = 0.0;
            cache[i].skin_temperature = 0.0;
            return i;
        }
    }
    return -1;
}

// 현재 시간
static uint64_t now_ms_realtime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void send_to_mq(mqd_t q, const DeviceCache* dc) {
    if (q == (mqd_t)-1 || !dc) return;

    WatchMsg msg;
    memset(&msg, 0, sizeof(msg));

    strncpy(msg.deviceId, dc->deviceId, DEV_ID_LEN - 1);
    msg.deviceId[DEV_ID_LEN - 1] = '\0';

    msg.heartRate = dc->heartRate;
    msg.skin_temperature = dc->skin_temperature;
    msg.has_hr = dc->has_hr;
    msg.has_st = dc->has_st;
    msg.ts_ms = now_ms_realtime();

    if (mq_send(q, (const char*)&msg, sizeof(msg), 0) == -1) {
        // Hub가 느려서 큐가 찼거나, 기타 오류
        perror("⚠️ mq_send failed");
    }
}

int watch_udp_run(const WatchUdpConfig* cfg) {
    if (!cfg || !cfg->bind_ip) return -1;

    const int port    = (cfg->port > 0 ? cfg->port : DEFAULT_PORT);
    const int max_dev = (cfg->max_devices > 0 ? cfg->max_devices : MAX_DEVICES);

    signal(SIGINT, handle_sigint);

    // Hub가 먼저 MQ를 생성/오픈해둬야 함
    g_watch_mq = mq_open(WATCH_QUEUE_NAME, O_WRONLY);
    if (g_watch_mq == (mqd_t)-1) {
        perror("❌ mq_open failed (run hub first / create MQ first)");
        return -2;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        mq_close(g_watch_mq);
        g_watch_mq = (mqd_t)-1;
        return -4;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr(cfg->bind_ip);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("bind failed (Address already in use?)");
        close(sock);
        mq_close(g_watch_mq);
        g_watch_mq = (mqd_t)-1;
        return -5;
    }

    DeviceCache* cache = (DeviceCache*)calloc((size_t)max_dev, sizeof(DeviceCache));
    if (!cache) {
        fprintf(stderr, "❌ calloc failed\n");
        close(sock);
        mq_close(g_watch_mq);
        g_watch_mq = (mqd_t)-1;
        return -6;
    }

    printf("📡 [watch_udp] Listening %s:%d → MQ %s\n", cfg->bind_ip, port, WATCH_QUEUE_NAME);

    unsigned char buf[4096];

    while (g_keep_running) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);

        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr*)&from, &fromlen);

        if (n < 0) {
            if (errno == EINTR) break; // SIGINT로 종료
            continue;
        }
        if (n == 0) continue;

        buf[n] = '\0';

        if (cfg->log_raw) {
            printf("📥 RAW: %s\n", (char*)buf);
        }

        cJSON* root = cJSON_Parse((const char*)buf);
        if (!root) {
            if (cfg->log_raw) fprintf(stderr, "⚠️ JSON Parse Error: %s\n", buf);
            continue;
        }

        char deviceId[DEV_ID_LEN] = "unknown";
        char type[32] = "";
        char ts[TS_LEN] = "";

        json_get_string(root, "deviceId", deviceId, sizeof(deviceId));
        json_get_string(root, "type", type, sizeof(type));
        json_get_string(root, "ts", ts, sizeof(ts));

        double value = 0.0;
        int has_value = json_get_number(root, "value", &value);

        int slot = find_or_create_slot(cache, max_dev, deviceId);
        if (slot >= 0) {
            DeviceCache* dc = &cache[slot];

            if (ts[0]) {
                strncpy(dc->last_ts, ts, TS_LEN - 1);
                dc->last_ts[TS_LEN - 1] = '\0';
            }

            if (strcmp(type, "HEART_RATE") == 0) {
                if (has_value) { dc->heartRate = value; dc->has_hr = 1; }
            } else if (strcmp(type, "SKIN_TEMP") == 0) {
                if (has_value) { dc->skin_temperature = value; dc->has_st = 1; }
            }

            send_to_mq(g_watch_mq, dc);
        }

        cJSON_Delete(root);
    }

    printf("\n🧹 Cleaning up watch module...\n");
    free(cache);
    close(sock);
    if (g_watch_mq != (mqd_t)-1) {
        mq_close(g_watch_mq);
        g_watch_mq = (mqd_t)-1;
    }
    return 0;
}
