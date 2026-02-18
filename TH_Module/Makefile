CC = gcc
# pkg-config 설정
MODBUS_CFLAGS = $(shell pkg-config --cflags libmodbus)
MODBUS_LIBS = $(shell pkg-config --libs libmodbus)

CFLAGS = -Wall -Wextra -O2 $(MODBUS_CFLAGS)
LDFLAGS = -lrt $(MODBUS_LIBS)

# 생성할 실행 파일들
TARGET1 = th_module_main.o
TARGET2 = th_test_stub.o

# 각 타겟별 소스 파일
SRCS1 = th_module_main.c th_module.c
SRCS2 = th_test_stub.c
HEADERS = th_module.h common.h

# 기본 타겟: 두 가지 모두 빌드
all: $(TARGET1) $(TARGET2)

# 첫 번째 실행 파일 (메인 모듈)
$(TARGET1): $(SRCS1) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SRCS1) $(LDFLAGS)

# 두 번째 실행 파일 (테스트 스텁)
$(TARGET2): $(SRCS2) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SRCS2) $(LDFLAGS)

# 정리
clean:
	rm -f $(TARGET1) $(TARGET2)

.PHONY: all clean
