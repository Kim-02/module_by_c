#include <stdio.h>
#include "vital_module.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    WatchUdpConfig cfg;
    cfg.port = 5005;
    cfg.bind_ip = "0.0.0.0";
    cfg.max_devices = 64;
    cfg.log_raw = 0;

    printf("▶ watch_udp_main start\n");
    return watch_udp_run(&cfg);
}
