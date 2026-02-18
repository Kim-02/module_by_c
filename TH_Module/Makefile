CC = gcc
TARGET = th_module_main.o

# pkg-config를 사용하여 modbus 라이브러리의 헤더 위치와 링크 옵션을 가져옵니다.
MODBUS_CFLAGS = $(shell pkg-config --cflags libmodbus)
MODBUS_LIBS = $(shell pkg-config --libs libmodbus)

# 기존 옵션에 추가
CFLAGS = -Wall -Wextra -O2 $(MODBUS_CFLAGS)
LDFLAGS = -lrt $(MODBUS_LIBS)

SRCS = th_module_main.c th_module.c
HEADERS = th_module.h common.h

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
