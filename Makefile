CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -g -std=c11 -Iinclude
LDFLAGS ?=

TARGET  := temp-sentry
SRCS    := src/main.c src/serial_port.c
OBJS    := $(SRCS:.c=.o)
DEPS    := $(wildcard include/*.h)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
