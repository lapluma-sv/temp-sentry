#define _DEFAULT_SOURCE

#include "serial_port.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define READ_BUF_SIZE 4096
#define POLL_TIMEOUT_SEC 1 /* MCU 每 1s 发一帧，超时可用于判离线 */

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

/*
 * 数据处理入口：串口读到的数据全部交到这里，分帧和字段解析都在这里扩展。
 * MCU 每 1s 发一帧 15 字节，形如：
 *   A5 A5 | 01 | 00 00 00 05 | 5 字节数据 | 校验 | A5 A5
 * 目前只把收到的原始内容打出来，方便确认数据。
 */
static void handle_data(const uint8_t *data, size_t len)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);

    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);

    printf("[%s.%03ld] %zu 字节  ", stamp, ts.tv_nsec / 1000000, len);

    /* 文本视图：可打印字符原样显示，其余用 '.' 占位 */
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        putchar((c >= 0x20 && c <= 0x7E) ? (int)c : '.');
    }

    printf("  | ");
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    putchar('\n');
    fflush(stdout);
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

    printf("开始接收（Ctrl-C 退出）\n");

    uint8_t buf[READ_BUF_SIZE];
    uint64_t total_bytes = 0;

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval tv = { .tv_sec = POLL_TIMEOUT_SEC, .tv_usec = 0 };

        int ready = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "select 失败: %s\n", strerror(errno));
            break;
        }
        if (ready == 0) {
            /* 超时：MCU 正常情况下每秒都会来一帧，此处可做离线检测 */
            continue;
        }

        ssize_t n = serial_read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            /* 设备被拔出等场景通常表现为 EIO */
            fprintf(stderr, "读串口失败: %s\n", strerror(errno));
            break;
        }
        if (n == 0) {
            continue;
        }

        total_bytes += (uint64_t)n;
        handle_data(buf, (size_t)n);
    }

    printf("退出：共收到 %llu 字节\n", (unsigned long long)total_bytes);

    serial_close(fd);
    return 0;
}
