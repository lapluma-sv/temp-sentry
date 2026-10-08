#ifndef MQTT_LINK_H
#define MQTT_LINK_H

/* 上报层：把越界事件 publish 到 broker，同时订阅上位机下发的判定参数。
 *
 * 本层不做阻塞等待：libmosquitto 的底层 socket fd 交给 main 的 epoll 统一管理，
 * 串口和 MQTT 共用同一个事件循环（评判要点之一）。用法：
 *   mqtt_link_init() -> mqtt_link_attach(epfd) -> 循环里 on_readable() + tick()
 */

/* 连接 broker；node_id 决定 topic 前缀 edge/<node_id>/event 与 /cmd。
 * 返回 0 表示初始化成功（连接结果通过回调体现，失败会由 tick() 自动重连） */
int mqtt_link_init(const char *host, int port, const char *node_id);

/* 把 MQTT socket 注册进主循环的 epoll；返回 0 成功 */
int mqtt_link_attach(int epfd);

/* 当前已注册进 epoll 的 MQTT fd，未建立时为 -1（重连后 fd 会变，需重新取） */
int mqtt_link_fd(void);

/* MQTT fd 可读时调用：读入数据并触发消息回调 */
void mqtt_link_on_readable(void);

/* 每次主循环迭代都要调用：跑心跳、按需重连、把排队的数据写出去 */
void mqtt_link_tick(void);

/* 事件上报（告警 / 恢复成对）：publish 事件 JSON 到 edge/<node_id>/event，QoS 1 */
void mqtt_link_publish_alert(double value, double low, double high);
void mqtt_link_publish_recovered(double value, double low, double high);

/* 心跳遥测：publish 当前温度与状态到 edge/<node_id>/telemetry，QoS 0。
 * 返回 1 表示已发出，0 表示未连接跳过（周期覆盖，等下一条即可） */
int mqtt_link_publish_telemetry(double temp, int out_of_range);

void mqtt_link_fini(void);

#endif
