#define _DEFAULT_SOURCE

#include "mqtt_link.h"
#include "protocol.h"
#include "sensor.h"
#include "serial_port.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

/* 200ms 是为了让 MQTT 的心跳 / 重连及时跑起来；串口有数据时 epoll 会立刻返回，
 * 这个值只在空闲时决定 tick() 的节奏 */
#define POLL_TIMEOUT_MS  200
#define EPOLL_MAX_EVENTS 8

/* 心跳遥测周期：文档口径 30s 一条，证明节点存活 */
#define TELEMETRY_PERIOD_MS 30000

static volatile sig_atomic_t g_running = 1;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

/* 越界上报：打印一份日志，同时 publish 事件 JSON 到 broker */
static void on_temp_alert(double value, double low, double high)
{
    fprintf(stderr, "[越界] 数值 %g 不在 [%g, %g] 内\n", value, low, high);
    mqtt_link_publish_alert(value, low, high);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "用法: %s [选项]\n"
            "  -d <设备>       串口设备节点，默认 /dev/ttyS3\n"
            "  -b <波特率>     默认 115200\n"
            "  -H <broker地址> MQTT broker 地址，默认 127.0.0.1（broker 在 PC 时填 PC 的 IP）\n"
            "  -p <broker端口> MQTT broker 端口，默认 1883\n"
            "  -t <节点编号>   本节点 id，决定 topic，默认 node01\n"
            "  -h              显示本帮助\n",
            prog);
}

int main(int argc, char *argv[])
{
    const char *device = "/dev/ttyS3";
    int baudrate = 115200;
    const char *broker_host = "127.0.0.1";
    int broker_port = 1883;
    const char *node_id = "node01";

    int opt;
    while ((opt = getopt(argc, argv, "d:b:H:p:t:h")) != -1) {
        switch (opt) {
        case 'd': device = optarg; break;
        case 'b': baudrate = atoi(optarg); break;
        case 'H': broker_host = optarg; break;
        case 'p': broker_port = atoi(optarg); break;
        case 't': node_id = optarg; break;
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

    if (mqtt_link_init(broker_host, broker_port, node_id) < 0) {
        serial_close(fd);
        return 1;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        fprintf(stderr, "epoll_create1 失败: %s\n", strerror(errno));
        mqtt_link_fini();
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
        mqtt_link_fini();
        serial_close(fd);
        return 1;
    }

    /* MQTT 的 socket 也挂进同一个 epoll，两个 fd 共用一个事件循环 */
    mqtt_link_attach(epfd);

    printf("开始接收（Ctrl-C 退出）\n");

    struct epoll_event events[EPOLL_MAX_EVENTS];

    /* 心跳遥测：连上 broker 且解析到过温度就立刻发第一条，之后每 30s 一条；
     * 上一次发送成功才更新时间戳，未连接期间不占用周期 */
    double     telemetry_value  = 0.0;
    long long  last_telemetry_ms = 0;

    while (g_running) {
        int n = epoll_wait(epfd, events, EPOLL_MAX_EVENTS, POLL_TIMEOUT_MS);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "epoll_wait 失败: %s\n", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == fd) {
                sensor_poll(fd); /* 取数据 + 分帧 + 解析数值 */
            } else if (events[i].data.fd == mqtt_link_fd()) {
                mqtt_link_on_readable();
            }
        }

        /* 有没有事件都要跑：MQTT 心跳、重连、把排队的数据写出去 */
        mqtt_link_tick();

        if (sensor_get_value(&telemetry_value)) {
            long long now = now_ms();
            if (last_telemetry_ms == 0 ||
                now - last_telemetry_ms >= TELEMETRY_PERIOD_MS) {
                if (mqtt_link_publish_telemetry(telemetry_value,
                                                sensor_get_state())) {
                    last_telemetry_ms = now;
                }
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
    mqtt_link_fini();
    serial_close(fd);
    return 0;
}