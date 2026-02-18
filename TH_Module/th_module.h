#ifndef TH_MODULE_H
#define TH_MODULE_H
#define TH_OK               TH_MODULE_OK
#define TH_ERR_NOT_INIT     TH_MODULE_ERR_NOT_INIT
#define TH_ERR_READ_FAIL    TH_MODULE_ERR_READ_FAIL
#define TH_ERR_BAD_VALUE    TH_MODULE_ERR_BAD_VALUE

#ifdef __cplusplus
extern "C" {
#endif

// 에러코드
enum {
    TH_MODULE_OK = 0,
    TH_MODULE_ERR_NOT_INIT   = -1,  // 초기화/연결 안 됨
    TH_MODULE_ERR_READ_FAIL  = -2,  // Modbus read 실패(통신/타임아웃/센서응답 등)
    TH_MODULE_ERR_BAD_VALUE  = -3,  // 값 무결성 실패(범위 밖/쓰레기값)
};


// 내부에서 사용하는 구조체
typedef struct {
    float temperature;
    float humidity;
    int error_code;
    int sys_errno;   // 시스템 errno(실패 시 디버깅용), 0은 정상 
    //TODO errorno가 꼭 필요한지 검토해야할 듯 error code와의 차이점?
    // 굳이 필요한건 아니고 오류 발생했을 때 에러코드는 위에 정의한 enum 내부에서 출력되는거고 errno는 리눅스 표준 오류 코드라 그냥 어떤 오류인지 정확히 구분하기 위한? 그래서 없어도 됨
} THData;


// th_module 내부 함수
int th_module_init(const char* ip, int port); // 초기화 및 네트워크 연결(0은 성공, -1은 실패)
THData th_module_read_once(void);             // 단일 데이터 읽기 (스레드 루프 내에서 호출용)
void th_module_close(void);                   // 자원 해제

#ifdef __cplusplus
}
#endif

#endif
