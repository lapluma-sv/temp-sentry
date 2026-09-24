#define _DEFAULT_SOURCE

#include "sensor.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 接收缓冲：协议层从串口读到的字节先攒在这里，够一帧才解出来 */
static char g_rx[PROTOCOL_BUF_SIZE];
static int  g_rx_len;

/* 解析结果：最新一帧正文里的数字，后续业务逻辑读它 */
static double g_value;
static int    g_has_value;

static uint64_t g_frames; /* 成功解析的帧数 */
static uint64_t g_errors; /* 校验失败、长度非法等异常次数 */

/* 判定参数：额定值 60，上下波动 ±2 => 正常范围 [58, 62] */
static double g_nominal   = 60.0;
static double g_tolerance = 2.0;

/* 当前是否处于越界状态，用来只在"正常 -> 越界"翻转时上报一次，避免每帧刷屏 */
static int g_out_of_range;

/* 越界上报回调，由外部（阶段二的 MQTT 层）注册 */
static sensor_alert_fn g_alert_fn;

void sensor_set_alert_fn(sensor_alert_fn fn)
{
    g_alert_fn = fn;
}

/* 丢掉帧头 A5 A5 之前的垃圾字节；
 * 没有完整帧头时只留末尾那个 A5（可能是帧头的前一半，等下一批数据补齐） */
static void rx_align(void)
{
    int start = 0;
    while (start + 1 < g_rx_len &&
           !((uint8_t)g_rx[start] == PROTOCOL_MAGIC &&
             (uint8_t)g_rx[start + 1] == PROTOCOL_MAGIC)) {
        start++;
    }

    if (start + 1 >= g_rx_len) {
        int keep = (g_rx_len > 0 && (uint8_t)g_rx[g_rx_len - 1] == PROTOCOL_MAGIC) ? 1 : 0;
        if (keep) {
            g_rx[0] = (char)PROTOCOL_MAGIC;
        }
        g_rx_len = keep;
        return;
    }

    if (start > 0) {
        memmove(g_rx, g_rx + start, (size_t)(g_rx_len - start));
        g_rx_len -= start;
    }
}

/* 越界判定：数值不在 [额定值 - 波动, 额定值 + 波动] 内就触发上报。
 * 只在状态翻转的那一刻上报一次：恢复正常后状态清零，下次越界再报。 */
static void check_range(void)
{
    double low  = g_nominal - g_tolerance;
    double high = g_nominal + g_tolerance;
    int bad = (g_value < low || g_value > high);

    if (bad == g_out_of_range) {
        return; /* 状态没变，不重复上报 */
    }
    g_out_of_range = bad;

    if (bad && g_alert_fn != NULL) {
        g_alert_fn(g_value, low, high);
    }
}

/* 取正文里的数字：MCU 发的正文是 ASCII 数字，如 "58.08" */
static void parse_value(const char *msg)
{
    char *end = NULL;
    double value = strtod(msg, &end); /* 协议层已补 '\0' */
    if (end == msg) {
        fprintf(stderr, "正文不是数字，丢弃: \"%s\"\n", msg);
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

    check_range();
}

void sensor_poll(int fd)
{
    while (1) {
        rx_align();

        uint8_t type = 0;
        char msg[PROTOCOL_MAX_BODY_SIZE + 1];
        int ret = protocol_recv_msg(fd, g_rx, &g_rx_len, &type, msg, sizeof(msg));

        if (ret == PROTOCOL_ERR_WOULDBLOCK) {
            return; /* 还没有完整帧，等下次可读再来 */
        }
        if (ret < 0) {
            /* 帧非法：串口流不能像 TCP 那样断开重来，丢掉 1 字节重新对齐帧头 */
            g_errors++;
            if (g_rx_len <= 0) {
                return;
            }
            memmove(g_rx, g_rx + 1, (size_t)(g_rx_len - 1));
            g_rx_len--;
            continue;
        }

        if (type == MSG_TEXT) {
            parse_value(msg);
        } else {
            fprintf(stderr, "未知消息类型: 0x%02X\n", type);
        }
    }
}

int sensor_get_value(double *out)
{
    if (!g_has_value) {
        return 0;
    }
    if (out != NULL) {
        *out = g_value;
    }
    return 1;
}

uint64_t sensor_frame_count(void)
{
    return g_frames;
}

uint64_t sensor_error_count(void)
{
    return g_errors;
}