CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -g -std=c11
LDFLAGS ?=

# 三层：hardware 硬件层 / driver 驱动层 / app 应用层
# libmosquitto：MQTT 客户端库，连 PC/板子上的 broker
MOSQ_CFLAGS := $(shell pkg-config --cflags libmosquitto)
MOSQ_LIBS   := $(shell pkg-config --libs libmosquitto)

INCLUDES := -Ihardware -Idriver -Iapp $(MOSQ_CFLAGS)

TARGET  := temp-sentry
SRCS    := hardware/serial_port.c driver/protocol.c app/sensor.c app/mqtt_link.c app/main.c
OBJS    := $(SRCS:.c=.o)
DEPS    := $(wildcard hardware/*.h driver/*.h app/*.h)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(OBJS) $(LDFLAGS) $(MOSQ_LIBS)

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
