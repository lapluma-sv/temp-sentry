#define _DEFAULT_SOURCE

#include "protocol.h"
#include "sensor.h"
#include "serial_port.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define POLL_TIMEOUT_MS  1000 /* MCU 每 1s 发一帧，超时可用于判离线 */
#define EPOLL_MAX_EVENTS 8

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* 越界上报：阶段二在这里 publish 事件到 edge/node01/event，现在先打印占位 */
static void on_temp_alert(double value, double low, double high)
{
    fprintf(stderr, "[越界] 数值 %g 不在 [%g, %g] 内\n", value, low, high);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "用法: %s [选项]\n"
            "  -d <设备>     串口设备节点，默认 /dev/ttyS3\n"
            "  -b <波特率>   默认 115200\n"
            "  -h            显示本帮助\n",
            prog);
}

int main(int argc, char *argv[])
{
    const char *device = "/dev/ttyS3";
    int baudrate = 115200;

    int opt;
    while ((opt = getopt(argc, argv, "d:b:h")) != -1) {
        switch (opt) {
        case 'd': device = optarg; break;
        case 'b': baudrate = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    /* 行缓冲，保证日志实时落盘 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    install_signal_handlers();

    sensor_set_alert_fn(on_temp_alert);

    struct serial_config cfg = {
        .device   = device,
        .baudrate = baudrate,
        .databits = 8,
        .parity   = 'N',
        .stopbits = 1,
    };

    int fd = serial_open(&cfg);
    if (fd < 0) {
        return 1;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        fprintf(stderr, "epoll_create1 失败: %s\n", strerror(errno));
        serial_close(fd);
        return 1;
    }

    /* 串口只注册水平触发：tty 的 poll 实现在边缘触发下容易漏数据 */
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        fprintf(stderr, "epoll_ctl 注册串口失败: %s\n", strerror(errno));
        close(epfd);
        serial_close(fd);
        return 1;
    }

    printf("开始接收（Ctrl-C 退出）\n");

    struct epoll_event events[EPOLL_MAX_EVENTS];

    while (g_running) {
        int n = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, POLL_TIMEOUT_MS);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "epoll_wait 失败: %s\n", strerror(errno));
            break;
        }
        if (n == 0) {
            /* 超时：MCU 正常情况下每秒都会来一帧，此处可做离线检测 */
            continue;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == fd) {
                sensor_poll(fd); /* 取数据 + 分帧 + 解析数值 */
            }
        }
    }

    printf("退出：共收到 %llu 字节，解析 %llu 帧，异常 %llu 次\n",
           (unsigned long long)protocol_rx_bytes(),
           (unsigned long long)sensor_frame_count(),
           (unsigned long long)sensor_error_count());

    double value = 0.0;
    if (sensor_get_value(&value)) {
        printf("最后一帧数值 = %g\n", value);
    }

    close(epfd);
    serial_close(fd);
    return 0;
}