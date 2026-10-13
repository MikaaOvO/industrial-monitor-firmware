#ifndef APP_TASKS_H
#define APP_TASKS_H

#include <stdint.h>

/* ================================================================
 * 四个任务
 *
 *   Task_Lvgl    —— LVGL 引擎，唯一的 lv_* 调用者（由 main.c 的
 *                   StartDefaultTask 调用，那是 CubeMX 建的任务）
 *   Task_Sensor  —— ADC1 三路采集 + 滤波 + 换算
 *   Task_Voc     —— UART4 收 21VOC 的 12 字节帧
 *   Task_Host    —— USART2 与上位机收发（遥测上报 + 阈值下发）
 * ================================================================ */

void Task_Lvgl(void *argument);
void Task_Sensor(void *argument);
void Task_Voc(void *argument);
void Task_Host(void *argument);
void Task_Output(void *argument);

/* 在 main.c 的 "USER CODE BEGIN RTOS_THREADS" 里调一次。
   创建 Sensor / Voc / Host / Output 四个任务（LVGL 任务由 CubeMX 的
   defaultTask 承担，不重复创建）。 */
void app_tasks_create(void);

/* 建完任务后剩余的 FreeRTOS 堆字节数。
   低于 1KB 就该去 CubeMX 调大 TOTAL_HEAP_SIZE 了。 */
uint32_t app_tasks_heap_left(void);

#endif /* APP_TASKS_H */
