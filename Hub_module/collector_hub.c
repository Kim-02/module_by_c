#include "collector_hub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#include <cjson/cJSON.h>
#include "th_sensor.h"

// ============================
// 내부 유틸
// ============================
static void ensure_fifo(const char* path) {
    if (!path) return;
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            fprintf(stderr, "❌ %s exists but not FIFO\n", path);
            return;
        }
        return;
    }
    if (mkfifo(path, 0666) != 0) {
        if (errno != EEXIST) perror("mkfifo");
    }
}

static double now_unix(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void now_local_iso(char* out, size_t outsz) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(out, outsz, "%Y-%m-%dT%H:%M:%S", &lt);
}

// RuleEngine과 동일 Heat Index 공식 (소수2자리)
static double calc_heat_index(double T, double RH) {
    double HI =
        -8.784695 +
        1.61139411 * T +
        2.338549 * RH -
        0.14611605 * T * RH -
        0.012308094 * T * T -
        0.016424828 * RH * RH +
        0.002211732 * T * T * RH +
        0.00072546 * T * RH * RH -
        0.000003582 * T * T * RH * RH;
    return (double)((int)(HI * 100.0 + (HI >= 0 ? 0.5 : -0.5))) / 100.0;
}

// ============================
// 캐시 구조
// ============================
typedef struct {
    int used;
    char deviceId[64];

    int has_hr;
    int has_st;
    double hr;
    double st;

    char last_ts[64];
} WatchCache;

struct CollectorHub {
    CollectorHubConfig cfg;

    // 콜백
    CollectorHubResultCallback cb;
    void* cb_ctx;

    // 실행 상태
    int running;
    pthread_mutex_t mtx;

    // env(TH)
    int has_env;
    double temp;
    double humi;

    // watch cache
    WatchCache* watch;
    int watch_cap;

    // threads
    pthread_t t_th;
    pthread_t t_watch;
    pthread_t t_rule_in;
    pthread_t t_rule_out;
};

// ============================
// 내부: device slot
// ============================
static int find_or_create_slot(struct CollectorHub* hub, const char* deviceId) {
    for (int i = 0; i < hub->watch_cap; i++) {
        if (hub->watch[i].used && strcmp(hub->watch[i].deviceId, deviceId) == 0) return i;
    }
    for (int i = 0; i < hub->watch_cap; i++) {
        if (!hub->watch[i].used) {
            hub->watch[i].used = 1;
            snprintf(hub->watch[i].deviceId, sizeof(hub->watch[i].deviceId), "%s", deviceId);
            hub->watch[i].has_hr = 0;
            hub->watch[i].has_st = 0;
            hub->watch[i].hr = 0;
            hub->watch[i].st = 0;
            hub->watch[i].last_ts[0] = '\0';
            return i;
        }
    }
    return -1;
}

// ============================
// 스레드 1) TH 폴링
// ============================
static void* th_thread(void* arg) {
    struct CollectorHub* hub = (struct CollectorHub*)arg;

    if (th_init(hub->cfg.th_ip, hub->cfg.th_port) != 0) {
        fprintf(stderr, "❌ [HUB][TH] th_init failed (%s:%d)\n",
                hub->cfg.th_ip, hub->cfg.th_port);
        // 그래도 루프는 돌면서 재시도/마지막값 유지 가능
    }

    while (hub->running) {
        THData d = th_read_once();

        pthread_mutex_lock(&hub->mtx);
        if (d.error_code == TH_OK) {
            hub->has_env = 1;
            hub->temp = d.temperature;
            hub->humi = d.humidity;
        }
        pthread_mutex_unlock(&hub->mtx);

        if (hub->cfg.log_th) {
            if (d.error_code == TH_OK) {
                printf("🌦️ [HUB][TH] T=%.2f H=%.2f\n", d.temperature, d.humidity);
            } else {
                printf("⚠️ [HUB][TH] read fail code=%d errno=%d\n", d.error_code, d.sys_errno);
            }
        }

        sleep(hub->cfg.collect_interval_sec);
    }

    th_close();
    return NULL;
}

// ============================
// 스레드 2) watch FIFO 리더
//   - watch_udp 모듈이 /tmp/th_fifo에 쓰는
//     {"deviceId","ts","heartRate","skin_temperature"} 라인을 읽음
// ============================
static void* watch_thread(void* arg) {
    struct CollectorHub* hub = (struct CollectorHub*)arg;

    ensure_fifo(hub->cfg.watch_fifo_path);

    FILE* fp = fopen(hub->cfg.watch_fifo_path, "r");
    if (!fp) {
        perror("fopen watch_fifo");
        return NULL;
    }

    char line[4096];

    while (hub->running) {
        if (!fgets(line, sizeof(line), fp)) {
            if (feof(fp)) {
                fclose(fp);
                fp = fopen(hub->cfg.watch_fifo_path, "r");
                if (!fp) { perror("reopen watch_fifo"); sleep(1); continue; }
            }
            continue;
        }

        cJSON* root = cJSON_Parse(line);
        if (!root) continue;

        const cJSON* jDev = cJSON_GetObjectItemCaseSensitive(root, "deviceId");
        const cJSON* jTs  = cJSON_GetObjectItemCaseSensitive(root, "ts");
        const cJSON* jHr  = cJSON_GetObjectItemCaseSensitive(root, "heartRate");
        const cJSON* jSt  = cJSON_GetObjectItemCaseSensitive(root, "skin_temperature");

        const char* dev = (cJSON_IsString(jDev) && jDev->valuestring) ? jDev->valuestring : "unknown";

        pthread_mutex_lock(&hub->mtx);
        int slot = find_or_create_slot(hub, dev);
        if (slot >= 0) {
            WatchCache* wc = &hub->watch[slot];

            if (cJSON_IsString(jTs) && jTs->valuestring) {
                snprintf(wc->last_ts, sizeof(wc->last_ts), "%s", jTs->valuestring);
            }

            if (cJSON_IsNumber(jHr)) { wc->hr = jHr->valuedouble; wc->has_hr = 1; }
            if (cJSON_IsNumber(jSt)) { wc->st = jSt->valuedouble; wc->has_st = 1; }
        }
        pthread_mutex_unlock(&hub->mtx);

        if (hub->cfg.log_watch) {
            printf("⌚ [HUB][WATCH] %s", line);
        }

        cJSON_Delete(root);
    }

    fclose(fp);
    return NULL;
}

// ============================
// 스레드 3) rulebase_in writer
//   - 5초마다 deviceId별로 SENSOR 메시지 1줄씩 전송
//   - 룰베이스 입력 포맷:
//     {"type":"SENSOR","seq":..,"deviceId":"..","hi":..,"hr":..,"st":..,"now_unix":..,"now_local":".."}
// ============================
static void* rule_in_thread(void* arg) {
    struct CollectorHub* hub = (struct CollectorHub*)arg;

    ensure_fifo(hub->cfg.rulebase_in_fifo_path);

    // 주의: reader(rulebase)가 아직 없으면 open("w")가 block 될 수 있음
    FILE* out = fopen(hub->cfg.rulebase_in_fifo_path, "w");
    if (!out) {
        perror("fopen rulebase_in");
        return NULL;
    }

    long seq = 0;

    while (hub->running) {
        double nu = now_unix();
        char local_iso[64];
        now_local_iso(local_iso, sizeof(local_iso));

        pthread_mutex_lock(&hub->mtx);

        int has_env = hub->has_env;
        double temp = hub->temp;
        double humi = hub->humi;

        for (int i = 0; i < hub->watch_cap; i++) {
            if (!hub->watch[i].used) continue;

            double hi = has_env ? calc_heat_index(temp, humi) : 0.0;

            cJSON* msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "type", "SENSOR");
            cJSON_AddNumberToObject(msg, "seq", (double)(++seq));
            cJSON_AddStringToObject(msg, "deviceId", hub->watch[i].deviceId);
            cJSON_AddNumberToObject(msg, "hi", hi);

            if (hub->watch[i].has_hr) cJSON_AddNumberToObject(msg, "hr", hub->watch[i].hr);
            else cJSON_AddNullToObject(msg, "hr");

            if (hub->watch[i].has_st) cJSON_AddNumberToObject(msg, "st", hub->watch[i].st);
            else cJSON_AddNullToObject(msg, "st");

            cJSON_AddNumberToObject(msg, "now_unix", nu);
            cJSON_AddStringToObject(msg, "now_local", local_iso);

            char* line = cJSON_PrintUnformatted(msg);
            if (line) {
                fprintf(out, "%s\n", line);
                fflush(out);

                if (hub->cfg.log_rule_in) {
                    printf("➡️ [HUB][RB_IN] %s\n", line);
                }

                free(line);
            }
            cJSON_Delete(msg);
        }

        pthread_mutex_unlock(&hub->mtx);

        sleep(hub->cfg.collect_interval_sec);
    }

    fclose(out);
    return NULL;
}

// ============================
// 스레드 4) rulebase_out reader
//   - RESULT 라인을 콜백으로 넘김
// ============================
static void* rule_out_thread(void* arg) {
    struct CollectorHub* hub = (struct CollectorHub*)arg;

    ensure_fifo(hub->cfg.rulebase_out_fifo_path);

    FILE* in = fopen(hub->cfg.rulebase_out_fifo_path, "r");
    if (!in) {
        perror("fopen rulebase_out");
        return NULL;
    }

    char line[8192];

    while (hub->running) {
        if (!fgets(line, sizeof(line), in)) {
            if (feof(in)) {
                fclose(in);
                in = fopen(hub->cfg.rulebase_out_fifo_path, "r");
                if (!in) { perror("reopen rulebase_out"); sleep(1); continue; }
            }
            continue;
        }

        if (hub->cfg.log_rule_out) {
            printf("⬅️ [HUB][RB_OUT] %s", line);
        }

        if (hub->cb) {
            hub->cb(line, hub->cb_ctx);
        }
    }

    fclose(in);
    return NULL;
}

// ============================
// 외부 API
// ============================
CollectorHub* collector_hub_create(const CollectorHubConfig* cfg,
                                   CollectorHubResultCallback cb,
                                   void* cb_ctx) {
    if (!cfg) return NULL;

    struct CollectorHub* hub = (struct CollectorHub*)calloc(1, sizeof(struct CollectorHub));
    if (!hub) return NULL;

    hub->cfg = *cfg;
    hub->cb = cb;
    hub->cb_ctx = cb_ctx;

    hub->running = 0;
    pthread_mutex_init(&hub->mtx, NULL);

    // defaults
    if (!hub->cfg.watch_fifo_path) hub->cfg.watch_fifo_path = "/tmp/th_fifo";
    if (!hub->cfg.rulebase_in_fifo_path) hub->cfg.rulebase_in_fifo_path = "/tmp/rulebase_in.fifo";
    if (!hub->cfg.rulebase_out_fifo_path) hub->cfg.rulebase_out_fifo_path = "/tmp/rulebase_out.fifo";

    if (!hub->cfg.th_ip) hub->cfg.th_ip = "192.168.0.20";
    if (hub->cfg.th_port <= 0) hub->cfg.th_port = 8887;

    if (hub->cfg.collect_interval_sec <= 0) hub->cfg.collect_interval_sec = 5;
    if (hub->cfg.max_devices <= 0) hub->cfg.max_devices = 64;

    hub->watch_cap = hub->cfg.max_devices;
    hub->watch = (WatchCache*)calloc((size_t)hub->watch_cap, sizeof(WatchCache));
    if (!hub->watch) {
        pthread_mutex_destroy(&hub->mtx);
        free(hub);
        return NULL;
    }

    return hub;
}

int collector_hub_start(CollectorHub* hub) {
    if (!hub) return -1;
    if (hub->running) return 0;

    // FIFO 준비 (경로는 "허브 설정에서" 결정)
    ensure_fifo(hub->cfg.watch_fifo_path);
    ensure_fifo(hub->cfg.rulebase_in_fifo_path);
    ensure_fifo(hub->cfg.rulebase_out_fifo_path);

    hub->running = 1;

    // 스레드 시작
    if (pthread_create(&hub->t_th, NULL, th_thread, hub) != 0) return -2;
    if (pthread_create(&hub->t_watch, NULL, watch_thread, hub) != 0) return -3;
    if (pthread_create(&hub->t_rule_in, NULL, rule_in_thread, hub) != 0) return -4;
    if (pthread_create(&hub->t_rule_out, NULL, rule_out_thread, hub) != 0) return -5;

    return 0;
}

void collector_hub_stop(CollectorHub* hub) {
    if (!hub || !hub->running) return;
    hub->running = 0;

    // 스레드 종료 대기
    pthread_join(hub->t_th, NULL);
    pthread_join(hub->t_watch, NULL);
    pthread_join(hub->t_rule_in, NULL);
    pthread_join(hub->t_rule_out, NULL);
}

void collector_hub_destroy(CollectorHub* hub) {
    if (!hub) return;
    if (hub->running) collector_hub_stop(hub);

    if (hub->watch) free(hub->watch);
    pthread_mutex_destroy(&hub->mtx);
    free(hub);
}