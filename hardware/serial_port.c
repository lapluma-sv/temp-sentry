#define _DEFAULT_SOURCE

#include "serial_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

speed_t serial_baud_to_speed(int baudrate)
{
    switch (baudrate) {
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default:     return (speed_t)-1;
    }
}

/* 组装 c_cflag：波特率 + 数据位 + 校验 + 停止位 + 本地连接/接收使能 */
static int build_cflag(speed_t speed, int databits, char parity, int stopbits,
                       tcflag_t *out)
{
    tcflag_t cflag = CLOCAL | CREAD;

    switch (databits) {
    case 5: cflag |= CS5; break;
    case 6: cflag |= CS6; break;
    case 7: cflag |= CS7; break;
    case 8: cflag |= CS8; break;
    default:
        fprintf(stderr, "不支持的位数据位: %d\n", databits);
        return -1;
    }

    switch (parity) {
    case 'N':
    case 'n':
        break;
    case 'E':
    case 'e':
        cflag |= PARENB;
        break;
    case 'O':
    case 'o':
        cflag |= PARENB | PARODD;
        break;
    default:
        fprintf(stderr, "不支持的校验方式: %c\n", parity);
        return -1;
    }

    if (stopbits == 2) {
        cflag |= CSTOPB;
    } else if (stopbits != 1) {
        fprintf(stderr, "不支持的停止位: %d\n", stopbits);
        return -1;
    }

    /* 波特率常量占用 c_cflag 中的专用位段 */
    cflag &= ~(CBAUD);
    cflag |= speed;

    *out = cflag;
    return 0;
}

int serial_open(const struct serial_config *cfg)
{
    if (cfg == NULL || cfg->device == NULL) {
        errno = EINVAL;
        return -1;
    }

    int databits = cfg->databits ? cfg->databits : 8;
    char parity = cfg->parity ? cfg->parity : 'N';
    int stopbits = cfg->stopbits ? cfg->stopbits : 1;

    speed_t speed = serial_baud_to_speed(cfg->baudrate);
    if (speed == (speed_t)-1) {
        fprintf(stderr, "不支持的波特率: %d\n", cfg->baudrate);
        errno = EINVAL;
        return -1;
    }

    /* O_NONBLOCK：read 无数据时立即返回 EAGAIN，不会阻塞主循环
     * O_NOCTTY：不让串口成为本进程的控制终端，避免收到终端信号 */
    int fd = open(cfg->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "打开串口 %s 失败: %s\n", cfg->device, strerror(errno));
        return -1;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    if (tcgetattr(fd, &tio) < 0) {
        fprintf(stderr, "tcgetattr 失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* 原始模式：所有加工都关掉，收到的就是 MCU 发出的原始字节流 */
    tio.c_iflag = IGNPAR; /* 仅忽略校验错字节，其余输入加工全部关闭 */
    tio.c_oflag = 0;
    tio.c_lflag = 0; /* 关闭 ICANON/ECHO/ISIG 等行规程 */

    if (build_cflag(speed, databits, parity, stopbits, &tio.c_cflag) < 0) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    tio.c_cc[VMIN]  = 0; /* 配合 O_NONBLOCK，读不到数据立即返回 */
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, speed) < 0 || cfsetospeed(&tio, speed) < 0) {
        fprintf(stderr, "设置波特率失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        fprintf(stderr, "tcsetattr 失败: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* 清掉本次打开前驱动缓冲里的残留数据，避免半截帧 */
    tcflush(fd, TCIOFLUSH);

    printf("串口已打开: %s %d %d%c%d\n", cfg->device, cfg->baudrate,
           databits, parity, stopbits);
    fflush(stdout);
    return fd;
}

void serial_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}
