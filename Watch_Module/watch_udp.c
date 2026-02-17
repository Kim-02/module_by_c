#include "watch_udp.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include <cjson/cJSON.h>

#define DEV_ID_LEN 64
#define TS_LEN 64

typedef struct {
    int used;
    char deviceId[DEV_ID_LEN];
    char last_ts[TS_LEN];
    int has_hr;
    int has_st;
    double heartRate;
    double skin_temperature;
} DeviceCache;

static int ensure_fifo(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) { //TODO 에러 처리 부분에서 파일을 지우고 fifo로 바꾼다던지 추가 처리가 필요할 듯
            fprintf(stderr, "❌ %s exists but not FIFO\n", path);
            return -1;
        }
        return 0;
    }
    if (mkfifo(path, 0666) != 0) {
        perror("mkfifo");
        return -1;
    }
    return 0;
}

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

static int find_or_create_slot(DeviceCache* cache, int max_dev, const char* deviceId) {
    for (int i = 0; i < max_dev; i++) {
        if (cache[i].used && strncmp(cache[i].deviceId, deviceId, DEV_ID_LEN) == 0) {
            return i;
        }
    }
    for (int i = 0; i < max_dev; i++) {
        if (!cache[i].used) {
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

static void fifo_write_line(FILE* fifo, const DeviceCache* dc) {
    const char* ts = (dc->last_ts[0] ? dc->last_ts : "-");

    cJSON* out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "deviceId", dc->deviceId);
    cJSON_AddStringToObject(out, "ts", ts);

    if (dc->has_hr) cJSON_AddNumberToObject(out, "heartRate", dc->heartRate);
    else cJSON_AddNullToObject(out, "heartRate");

    if (dc->has_st) cJSON_AddNumberToObject(out, "skin_temperature", dc->skin_temperature);
    else cJSON_AddNullToObject(out, "skin_temperature");

    char* line = cJSON_PrintUnformatted(out);
    if (line) {
        fprintf(fifo, "%s\n", line);
        fflush(fifo);
        free(line);
    }
    cJSON_Delete(out);
}

int watch_udp_run(const WatchUdpConfig* cfg) {
    if (!cfg || !cfg->fifo_path || !cfg->bind_ip) return -1;

    const int port = (cfg->port > 0 ? cfg->port : 5005); // TODO 5005 포트가 고정된다면 굳이 포트를 바꾸지 말고 하드코딩으로 넣어도 될듯
    const int max_dev = (cfg->max_devices > 0 ? cfg->max_devices : 64);

    // 1) FIFO 준비
    //TODO 각 분기마다 에러처리 후 return하는 음수가 다른데 무슨 의미가 있는지?
    if (ensure_fifo(cfg->fifo_path) != 0) return -2; //TODO 이 부분도 굳이 경로가 바뀔 필요가 없다면 하드코딩

    FILE* fifo = fopen(cfg->fifo_path, "a");
    if (!fifo) { perror("fopen FIFO"); return -3; }

    // 2) UDP 소켓 준비
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); fclose(fifo); return -4; }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr(cfg->bind_ip);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("bind (Address already in use?)");
        fprintf(stderr, "👉 이미 %d 포트를 쓰는 프로세스가 있으면 종료하거나 포트를 바꿔야 합니다.\n", port);
        close(sock);
        fclose(fifo);
        return -5;
    }

    DeviceCache* cache = (DeviceCache*)calloc((size_t)max_dev, sizeof(DeviceCache));
    if (!cache) {
        fprintf(stderr, "❌ calloc failed\n");
        close(sock);
        fclose(fifo);
        return -6;
    }

    printf("📡 [watch_udp_bridge] UDP listen %s:%d → FIFO %s\n", cfg->bind_ip, port, cfg->fifo_path);

    unsigned char buf[4096];

    //TODO 이 부분 이렇게 구현해도 병목이 일어나지 않는지 조사 필요할 듯
    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&from, &fromlen);
        if (n <= 0) continue;
        buf[n] = '\0';

        if (cfg->log_raw) {
            printf("📥 RAW: %s\n", (char*)buf);
        }

        cJSON* root = cJSON_Parse((const char*)buf);
        if (!root) continue; //TODO 이 부분 에러처리가 왜 없는지?

        char deviceId[DEV_ID_LEN] = "unknown";
        char type[32] = "";
        char ts[TS_LEN] = "";

        json_get_string(root, "deviceId", deviceId, sizeof(deviceId));
        json_get_string(root, "type", type, sizeof(type));
        json_get_string(root, "ts", ts, sizeof(ts));

        double value = 0.0;
        int has_value = json_get_number(root, "value", &value);

        int slot = find_or_create_slot(cache, max_dev, deviceId); //TODO 이 부분이 시간을 많이 잡아먹을 것으로 보임
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

            // ✅ 매 패킷마다 “현재 캐시 상태”를 FIFO로 출력
            //TODO FIFO에 작성한 걸 MQ로 보내는 코드 추가해야할듯
            //MQ에 보낼 때 어떻게 device를 구분할 건지도 고민해봐야할 문제
            fifo_write_line(fifo, dc);
        }

        cJSON_Delete(root);
    } //TODO 루프가 끝나고(종료) 어떻게 파일을 정리할 지 코드 추가 필요 

    // (실제로는 여기 도달 안 함)
    free(cache);
    close(sock);
    fclose(fifo);
    return 0;
}