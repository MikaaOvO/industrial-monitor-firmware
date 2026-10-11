#ifndef SENSOR_H
#define SENSOR_H

#include <stdbool.h>

/* ================================================================
 * ADC 采集（光照 / MQ-2 烟雾 / MQ-7 有毒气体）
 *
 * 由 Task_Sensor 使用。开机会启动 ADC1 + DMA 循环搬运，
 * 之后传感器数据由硬件自己更新，读的时候直接取最新值。
 *
 * 三路都是纯模拟量，输出是 0~100 的相对指数，不是绝对单位。
 * 端点由 sensor.c 里的 cal[] 表定义（需实测填入）。
 * ================================================================ */

/* 初始化：启动 ADC1 的 DMA 循环搬运。
   只需在任务进循环前调一次。返回 false 表示启动失败。 */
bool sensor_init(void);

/* 取一次滤波后的读数，三个参数都是 0~100 的相对指数：
 *   light_idx —— 光照（0~100，越大越亮）
 *   smoke_idx —— MQ-2 烟雾
 *   co_idx    —— MQ-7 有毒气体
 *
 * 内部做了中值滤波（去尖峰）+ 指数滑动平均（跨周期平滑）。
 * 阻塞约 5ms，调用周期给 100ms 很宽裕。
 */
void sensor_read(float *light_idx, float *smoke_idx, float *co_idx);

#endif /* SENSOR_H */
