#ifndef TEMP_SENTRY_SERIAL_PORT_H
#define TEMP_SENTRY_SERIAL_PORT_H

#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

/* 串口参数，未显式赋值的字段由 serial_open 填默认值 */
struct serial_config {
    const char *device;   /* 设备节点，如 /dev/ttyUSB0 */
    int         baudrate; /* 波特率，如 115200 */
    int         databits; /* 数据位 5~8，0 表示默认 8 */
    char        parity;   /* 'N'/'E'/'O'，0 表示默认 'N' */
    int         stopbits; /* 停止位 1 或 2，0 表示默认 1 */
};

/*
 * 打开并配置串口：原始模式（不做输入/输出加工）、非阻塞。
 * 成功返回文件描述符，失败返回 -1 并设置 errno。
 */
int serial_open(const struct serial_config *cfg);

/* 关闭串口，fd < 0 时直接返回 */
void serial_close(int fd);

/*
 * 读取串口数据。
 * 返回 >0：实际读到的字节数；返回 0：暂时无数据；返回 -1：出错，
 * errno 为 EAGAIN/EWOULDBLOCK/EINTR 时属于正常情况，可继续循环。
 */
ssize_t serial_read(int fd, void *buf, size_t len);

/* 波特率数值转换为 termios 的 Bxxx 常量，不支持时返回 (speed_t)-1 */
speed_t serial_baud_to_speed(int baudrate);

#endif
