#ifndef SENSOR_H
#define SENSOR_H

#include <stdint.h>

/* 数据层：从串口取帧、解析出数值并保存。协议细节都在 protocol 层，这里只管数据 */

/* 越界上报接口：本层只负责判定，具体怎么上报（阶段二走 MQTT publish）由外部注册 */
typedef void (*sensor_alert_fn)(double value, double low, double high);

/* 恢复上报接口：从越界回到范围内时触发，与告警成对 */
typedef void (*sensor_recover_fn)(double value, double low, double high);

/* 注册越界上报回调；传 NULL 表示不上报 */
void sensor_set_alert_fn(sensor_alert_fn fn);

/* 注册恢复上报回调；传 NULL 表示不上报 */
void sensor_set_recover_fn(sensor_recover_fn fn);

/* 更新判定阈值：正常范围变成 [nominal - tolerance, nominal + tolerance]。
 * 阶段二由上位机通过 MQTT 下发参数时调用 */
void sensor_set_threshold(double nominal, double tolerance);

/* 从串口取数据并解析，内部一直取到没有完整帧为止 */
void sensor_poll(int fd);

/* 取最新数值：返回 1 表示解析到过，0 表示还没有 */
int sensor_get_value(double *out);

/* 当前判定状态：返回 1 表示越界，0 表示在范围内 */
int sensor_get_state(void);

uint64_t sensor_frame_count(void);
uint64_t sensor_error_count(void);

#endif