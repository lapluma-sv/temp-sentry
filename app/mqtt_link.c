#define _DEFAULT_SOURCE

#include "mqtt_link.h"
#include "sensor.h"

#include <errno.h>
#include <mosquitto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>

#define KEEPALIVE_S   30   /* 心跳间隔，broker 超过 1.5 倍没收到就判掉线 */
#define RECONNECT_MS  3000 /* 断线后多久重试一次 */
#define TOPIC_MAX     128
#define PAYLOAD_MAX   256

static struct mosquitto *g_mosq;
static int  g_epfd = -1;
static int  g_mqtt_fd = -1;     /* 已注册进 epoll 的 fd，-1 表示还没建好 */
static int  g_connected;
static long long g_last_try_ms; /* 上次发起连接的时刻，用于限速重连 */

static char g_topic_event[TOPIC_MAX];
static char g_topic_cmd[TOPIC_MAX];
static char g_topic_telemetry[TOPIC_MAX];

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* fd 会随重连变化，每次变化都把旧的摘掉、新的挂上 */
static void sync_socket(void)
{
    int fd = mosquitto_socket(g_mosq);
    if (fd == g_mqtt_fd) {
        return;
    }

    if (g_mqtt_fd >= 0) {
        epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_mqtt_fd, NULL);
        g_mqtt_fd = -1;
    }
    if (fd < 0) {
        return;
    }

    /* 只等可读：可写交给 tick() 里的 loop_write 兜底，避免 EPOLLOUT 空转 */
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        fprintf(stderr, "[MQTT] 注册 socket 到 epoll 失败: %s\n", strerror(errno));
        return;
    }
    g_mqtt_fd = fd;
}

static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)userdata;
    if (rc != 0) {
        fprintf(stderr, "[MQTT] 连接被拒绝: %s\n", mosquitto_connack_string(rc));
        return;
    }
    g_connected = 1;
    printf("[MQTT] 已连接 broker，订阅 %s\n", g_topic_cmd);
    fflush(stdout);
    mosquitto_subscribe(mosq, NULL, g_topic_cmd, 1);
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    (void)userdata;
    g_connected = 0;
    if (rc != 0) { /* rc == 0 是我们自己正常退出时主动断的，不用报 */
        fprintf(stderr, "[MQTT] 连接断开（rc=%d），稍后自动重连\n", rc);
    }
}

/* 极简 JSON 取值：只认 "key": <数字> 这种形式，够用就不引第三方库 */
static int json_get_number(const char *json, const char *key, double *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(json, pat);
    if (p == NULL) {
        return 0;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ':') {
        return 0;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) {
        return 0;
    }
    *out = v;
    return 1;
}

/* 收到 edge/<node>/cmd：改成新的额定值 / 波动范围 */
static void on_message(struct mosquitto *mosq, void *userdata,
                       const struct mosquitto_message *msg)
{
    (void)mosq;
    (void)userdata;

    if (msg->payloadlen <= 0) {
        return;
    }

    char *body = malloc((size_t)msg->payloadlen + 1);
    if (body == NULL) {
        return;
    }
    memcpy(body, msg->payload, (size_t)msg->payloadlen);
    body[msg->payloadlen] = '\0';

    printf("[MQTT] 收到下发 %s: %s\n", msg->topic, body);

    double nominal = 0.0;
    double tolerance = 0.0;
    if (json_get_number(body, "nominal", &nominal) &&
        json_get_number(body, "tolerance", &tolerance)) {
        sensor_set_threshold(nominal, tolerance);
    } else {
        fprintf(stderr, "[MQTT] 下发格式不对，需要 {\"nominal\":x,\"tolerance\":y}\n");
    }
    fflush(stdout);

    free(body);
}

int mqtt_link_init(const char *host, int port, const char *node_id)
{
    snprintf(g_topic_event, sizeof(g_topic_event), "edge/%s/event", node_id);
    snprintf(g_topic_cmd, sizeof(g_topic_cmd), "edge/%s/cmd", node_id);
    snprintf(g_topic_telemetry, sizeof(g_topic_telemetry), "edge/%s/telemetry", node_id);

    mosquitto_lib_init();

    /* clean_session = true：本阶段不要求断线期间的消息补发 */
    g_mosq = mosquitto_new(NULL, true, NULL);
    if (g_mosq == NULL) {
        fprintf(stderr, "[MQTT] mosquitto_new 失败\n");
        return -1;
    }

    mosquitto_connect_callback_set(g_mosq, on_connect);
    mosquitto_disconnect_callback_set(g_mosq, on_disconnect);
    mosquitto_message_callback_set(g_mosq, on_message);

    int rc = mosquitto_connect_async(g_mosq, host, port, KEEPALIVE_S);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] connect_async(%s:%d) 失败: %s\n",
                host, port, mosquitto_strerror(rc));
        return -1;
    }

    g_last_try_ms = now_ms();
    printf("[MQTT] 正在连接 %s:%d，上报 topic %s\n", host, port, g_topic_event);
    fflush(stdout);
    return 0;
}

int mqtt_link_attach(int epfd)
{
    g_epfd = epfd;
    sync_socket(); /* 首次连接可能还没建好 socket，tick() 会补上 */
    return 0;
}

int mqtt_link_fd(void)
{
    return g_mqtt_fd;
}

void mqtt_link_on_readable(void)
{
    mosquitto_loop_read(g_mosq, 1);
}

void mqtt_link_tick(void)
{
    if (g_mosq == NULL) {
        return;
    }

    mosquitto_loop_misc(g_mosq); /* 心跳 + 超时检测 */

    if (!g_connected) {
        long long t = now_ms();
        if (t - g_last_try_ms >= RECONNECT_MS) {
            g_last_try_ms = t;
            if (mosquitto_reconnect_async(g_mosq) == MOSQ_ERR_SUCCESS) {
                printf("[MQTT] 重连中...\n");
                fflush(stdout);
            }
        }
    }

    sync_socket(); /* 重连后 fd 变了要重新挂进 epoll */

    /* 无条件写出排队数据：connect_async 的 CONNECT 报文也在这里发出去，
     * 若只在 connected 后才写，握手永远完不成（CONNACK 依赖 CONNECT 先发出） */
    mosquitto_loop_write(g_mosq, 1);
}

/* 事件上报公共实现：告警 / 恢复只是事件名和等级不同 */
static void publish_event(const char *event, const char *level,
                          double value, double low, double high)
{
    if (g_mosq == NULL) {
        return;
    }
    if (!g_connected) {
        fprintf(stderr, "[MQTT] 未连接 broker，本次事件丢弃\n");
        return;
    }

    /* 回调只给了上下限，还原成判定用的额定值 / 波动 */
    double nominal   = (low + high) / 2.0;
    double tolerance = (high - low) / 2.0;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long ts_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    char payload[PAYLOAD_MAX];
    int n = snprintf(payload, sizeof(payload),
                     "{\"event\":\"%s\",\"level\":\"%s\",\"nominal\":%.1f,"
                     "\"tolerance\":%.1f,\"measured\":%.2f,\"ts\":%lld}",
                     event, level, nominal, tolerance, value, ts_ms);
    if (n < 0 || (size_t)n >= sizeof(payload)) {
        return;
    }

    int rc = mosquitto_publish(g_mosq, NULL, g_topic_event, n, payload, 1, 0);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] publish 失败: %s\n", mosquitto_strerror(rc));
        return;
    }

    printf("[MQTT] 已上报 %s: %s\n", g_topic_event, payload);
    fflush(stdout);
}

/* 越界告警：进入越界状态的那一刻触发 */
void mqtt_link_publish_alert(double value, double low, double high)
{
    publish_event("temp_violation", "error", value, low, high);
}

/* 恢复事件：回到范围内的那一刻触发，与告警成对，上位机据此关闭告警 */
void mqtt_link_publish_recovered(double value, double low, double high)
{
    publish_event("temp_recovered", "info", value, low, high);
}

/* 心跳遥测：当前温度 + 状态，QoS 0，丢了等下一条；未连接时静默跳过 */
int mqtt_link_publish_telemetry(double temp, int out_of_range)
{
    if (g_mosq == NULL || !g_connected) {
        return 0;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long ts_ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    char payload[PAYLOAD_MAX];
    int n = snprintf(payload, sizeof(payload),
                     "{\"temp\":%.2f,\"state\":\"%s\",\"ts\":%lld}",
                     temp, out_of_range ? "out_of_range" : "normal", ts_ms);
    if (n < 0 || (size_t)n >= sizeof(payload)) {
        return 0;
    }

    int rc = mosquitto_publish(g_mosq, NULL, g_topic_telemetry, n, payload, 0, 0);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[MQTT] 遥测 publish 失败: %s\n", mosquitto_strerror(rc));
        return 0;
    }

    printf("[MQTT] 遥测 %s: %s\n", g_topic_telemetry, payload);
    fflush(stdout);
    return 1;
}

void mqtt_link_fini(void)
{
    if (g_mosq == NULL) {
        return;
    }
    if (g_epfd >= 0 && g_mqtt_fd >= 0) {
        epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_mqtt_fd, NULL);
        g_mqtt_fd = -1;
    }

    mosquitto_disconnect(g_mosq);
    mosquitto_destroy(g_mosq);
    g_mosq = NULL;
    mosquitto_lib_cleanup();
}
