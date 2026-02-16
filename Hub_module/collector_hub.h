#ifndef COLLECTOR_HUB_H
#define COLLECTOR_HUB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef struct {
    // ---------- FIFOs ----------
    const char* watch_fifo_path;       // watch_udp가 쓰는 FIFO (예: "/tmp/th_fifo")
    const char* rulebase_in_fifo_path; // C -> RuleBase (예: "/tmp/rulebase_in.fifo")
    const char* rulebase_out_fifo_path;// RuleBase -> C (예: "/tmp/rulebase_out.fifo")

    // ---------- TH(Modbus) ----------
    const char* th_ip;                 // 예: "192.168.0.20"
    int th_port;                       // 예: 8887

    // ---------- Hub behavior ----------
    int collect_interval_sec;          // Rule step 주기 (예: 5)
    int max_devices;                   // deviceId 캐시 수 (예: 64)

    // 로그 옵션
    int log_th;                        // 1이면 TH 폴링 로그
    int log_watch;                     // 1이면 watch FIFO 수신 로그
    int log_rule_in;                   // 1이면 rulebase_in write 로그
    int log_rule_out;                  // 1이면 rulebase_out read 로그
} CollectorHubConfig;

// rulebase_out에서 RESULT 라인(JSON)을 받았을 때 호출되는 콜백
typedef void (*CollectorHubResultCallback)(const char* json_line, void* user_ctx);

// opaque handle
typedef struct CollectorHub CollectorHub;

// 생성/시작/정지/해제
CollectorHub* collector_hub_create(const CollectorHubConfig* cfg,
                                   CollectorHubResultCallback cb,
                                   void* cb_ctx);

// 내부 스레드 시작 (TH 폴링, watch FIFO 리더, rule_in writer, rule_out reader)
int collector_hub_start(CollectorHub* hub);

// 종료 요청 + join
void collector_hub_stop(CollectorHub* hub);

// 메모리 해제( stop 이후 호출 권장 )
void collector_hub_destroy(CollectorHub* hub);

#ifdef __cplusplus
}
#endif

#endif