#ifndef TH_SENSOR_H
#define TH_SENSOR_H

#ifdef __cplusplus
extern "C" {
#endif

// 에러코드
enum {
    TH_OK = 0,
    TH_ERR_NOT_INIT   = -1,  // 초기화/연결 안 됨
    TH_ERR_READ_FAIL  = -2,  // Modbus read 실패(통신/타임아웃/센서응답 등)
    TH_ERR_BAD_VALUE  = -3,  // 값 무결성 실패(범위 밖/쓰레기값)
};

// 수집된 데이터를 담는 표준 구조체
typedef struct {
    float temperature;
    float humidity;
    int error_code;
    int sys_errno;   // 시스템 errno(실패 시 디버깅용), 0은 정상
} THData;

// th_sensor 내부 함수
int th_init(const char* ip, int port); // 초기화 및 네트워크 연결(0은 성공, -1은 실패)
THData th_read_once(void);             // 단일 데이터 읽기 (스레드 루프 내에서 호출용)
void th_close(void);                   // 자원 해제

#ifdef __cplusplus
}
#endif

#endif