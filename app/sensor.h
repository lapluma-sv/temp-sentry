#ifndef SENSOR_H
#define SENSOR_H

#include <stdint.h>

/* 数据层：从串口取帧、解析出数值并保存。协议细节都在 protocol 层，这里只管数据 */

/* 从串口取数据并解析，内部一直取到没有完整帧为止 */
void sensor_poll(int fd);

/* 取最新数值：返回 1 表示解析到过，0 表示还没有 */
int sensor_get_value(double *out);

uint64_t sensor_frame_count(void);
uint64_t sensor_error_count(void);

#endif