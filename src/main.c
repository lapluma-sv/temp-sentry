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

/* ---------- 帧格式（与 chatroom 项目的应用层协议一致） ----------
 *   7 字节头：A5 A5 | 类型 | 4 字节网络序正文长度
 *   正文    ：长度 ≤ 1024
 *   3 字节尾：1 字节校验（正文逐字节异或）| A5 A5
 * MCU 的帧共 15 字节：A5 A5 01 00 00 00 05 <5 字节 ASCII 数字> <校验> A5 A5
 */
#define PROTO_MAGIC       0xA5
#define PROTO_HEADER_SIZE 7
#define PROTO_TAIL_SIZE   3
#define PROTO_MAX_BODY    1024
#define PROTO_BUF_SIZE    (PROTO_HEADER_SIZE + PROTO_MAX_BODY + PROTO_TAIL_SIZE)

#define MSG_TYPE_TEXT 0x01 /* MCU 上报数据用的消息类型 */

static uint8_t g_rx[PROTO_BUF_SIZE]; /* 接收缓冲：拼接半帧、拆开粘包 */
static size_t  g_rx_len;             /* 缓冲里待解析的字节数 */

/* 解析结果：最新一帧正文里的数字，后续业务逻辑直接用这个变量 */
static double g_value;
static int    g_has_value; /* 是否解析到过有效数值 */

static uint64_t g_frames; /* 成功解析的帧数 */
static uint64_t g_errors; /* 长度非法 / 校验失败的次数 */

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

/* 丢掉缓冲里最前面的 n 字节 */
static void rx_consume(size_t n)
{
    if (n >= g_rx_len) {
        g_rx_len = 0;
        return;
    }
    memmove(g_rx, g_rx + n, g_rx_len - n);
    g_rx_len -= n;
}

/* 取出正文里的数字存进 g_value */
static void parse_body(const uint8_t *body, size_t len)
{
    char text[PROTO_MAX_BODY + 1];
    memcpy(text, body, len);
    text[len] = '\0';

    char *end = NULL;
    double value = strtod(text, &end);
    if (end == text) {
        fprintf(stderr, "正文不是数字，丢弃: \"%s\"\n", text);
        g_errors++;
        return;
    }

    g_value = value;
    g_has_value = 1;
    g_frames++;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf);

    printf("[%s.%03ld] 数值 = %g\n", stamp, ts.tv_nsec / 1000000, g_value);
    fflush(stdout);
}

/*
 * 数据处理入口：串口读到的数据全部交到这里，分帧和字段解析都在这里做。
 * 一次 read 可能拿到半帧、也可能拿到多帧，所以先攒进 g_rx 再一帧一帧取。
 */
static void handle_data(const uint8_t *data, size_t len)
{
    if (len > sizeof(g_rx) - g_rx_len) {
        fprintf(stderr, "接收缓冲已满，丢弃 %zu 字节未解析数据\n", g_rx_len);
        g_rx_len = 0;
    }
    memcpy(g_rx + g_rx_len, data, len);
    g_rx_len += len;

    while (1) {
        /* 1. 找帧头 A5 A5，帧头之前的字节都是垃圾 */
        size_t start = 0;
        while (start + 1 < g_rx_len &&
               !(g_rx[start] == PROTO_MAGIC && g_rx[start + 1] == PROTO_MAGIC)) {
            start++;
        }
        if (start + 1 >= g_rx_len) {
            /* 没有完整帧头：只留末尾那个 A5（可能是帧头的前一半） */
            size_t keep = (g_rx_len > 0 && g_rx[g_rx_len - 1] == PROTO_MAGIC) ? 1 : 0;
            if (keep == 1) {
                g_rx[0] = PROTO_MAGIC;
            }
            g_rx_len = keep;
            return;
        }
        if (start > 0) {
            rx_consume(start);
        }

        /* 2. 帧头还没收全，等后续数据 */
        if (g_rx_len < PROTO_HEADER_SIZE) {
            return;
        }

        /* 3. 取正文长度（网络序，对应 chatroom 里的 htonl/ntohl） */
        uint32_t body_len = ((uint32_t)g_rx[3] << 24) | ((uint32_t)g_rx[4] << 16) |
                            ((uint32_t)g_rx[5] << 8) | (uint32_t)g_rx[6];
        if (body_len > PROTO_MAX_BODY) {
            /* 长度非法：这个帧头不可信，跳过 1 字节重新找 */
            g_errors++;
            rx_consume(1);
            continue;
        }

        size_t total = PROTO_HEADER_SIZE + (size_t)body_len + PROTO_TAIL_SIZE;
        if (g_rx_len < total) {
            return; /* 半帧，等剩下的字节 */
        }

        /* 4. 校验尾魔数和正文异或校验 */
        uint8_t checksum = 0;
        for (size_t i = 0; i < body_len; i++) {
            checksum ^= g_rx[PROTO_HEADER_SIZE + i];
        }
        if (g_rx[total - 2] != PROTO_MAGIC || g_rx[total - 1] != PROTO_MAGIC ||
            g_rx[total - 3] != checksum) {
            g_errors++;
            rx_consume(1); /* 校验不过：跳过 1 字节，继续往后找帧头 */
            continue;
        }

        if (g_rx[2] == MSG_TYPE_TEXT) {
            parse_body(g_rx + PROTO_HEADER_SIZE, body_len);
        } else {
            fprintf(stderr, "未知消息类型: 0x%02X\n", g_rx[2]);
        }

        /* 5. 这一帧处理完，继续处理后面剩下的数据（粘包） */
        rx_consume(total);
    }
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

    printf("退出：共收到 %llu 字节，解析 %llu 帧，异常 %llu 次\n",
           (unsigned long long)total_bytes, (unsigned long long)g_frames,
           (unsigned long long)g_errors);
    if (g_has_value) {
        printf("最后一帧数值 = %g\n", g_value);
    }

    serial_close(fd);
    return 0;
}
