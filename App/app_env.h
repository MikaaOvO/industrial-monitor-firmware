#ifndef APP_ENV_H
#define APP_ENV_H

#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * 任务间共享数据
 *
 * 写入者：SensorTask（光照 / MQ-2 / MQ-7）
 *         VocTask  （温湿 / eCO2 / VOC / 甲醛）
 * 读取者：LvglTask（刷界面，在 ui.c 的定时器里）
 *         HostTask（组帧上报上位机）
 *
 * 约定：只能通过下面的 env_* / cfg_* 接口访问，不要直接碰全局变量。
 *       接口内部已经上锁，可以被任何任务随时调用。
 * ================================================================ */

/* 环境数据快照 */
typedef struct {
    /* ---- 来自 21VOC 模块（UART4，12 字节主动上报帧）----
       这一组是模块出厂标定好的数字量，有绝对单位 */
    float    temp_c;        /* 温度 ℃                        */
    float    humi_rh;       /* 湿度 %RH                      */
    uint16_t eco2_ppm;      /* eCO2 ppm（模块的等效估算值）  */
    uint16_t voc_ugm3;      /* VOC ug/m3                     */
    uint16_t hcho_ugm3;     /* 甲醛 ug/m3                    */
    bool     voc_ok;        /* 最近一帧校验是否通过          */
    bool     voc_online;    /* 模块是否在持续上报（超时判定）*/

    /* ---- 来自 ADC1（PA0/PA1/PC0，经 1:2 分压）----
       这三路是纯模拟量，没有绝对单位，用两点标定归一化成
       0~100 的相对指数（0=干净/全暗的一端，100=另一端）。
       阈值滑块就是这些指数的参照系：多少算异常由滑块决定。 */
    float    light_idx;     /* 光照   0~100（0=全暗，100=最亮）*/
    float    smoke_idx;     /* MQ-2  0~100                   */
    float    co_idx;        /* MQ-7  0~100                   */
} env_t;

/* 阈值与开关（界面和上位机都能改，两边共用这一份） */
typedef struct {
    /* 五路"超上限就报警"的阈值 */
    float    temp_hi;       /* 温度报警阈值 ℃                 */
    float    humi_hi;       /* 湿度报警阈值 %RH               */
    float    smoke_hi;      /* 烟雾报警阈值   0~100           */
    float    gas_hi;        /* 有毒气体报警阈值 0~100         */
    uint16_t eco2_hi;       /* CO2 报警阈值 ppm               */

    /* 一路相反方向的：光照是"低于阈值就开灯"（台账里的明暗控制），
       不参与报警，由灯光控制逻辑去读 */
    float    light_lo;      /* 开灯阈值 0~100（低于此值开灯）  */

    bool     auto_fan;      /* 自动通风                       */
    bool     manual_fan;    /* 手动通风（界面按钮 / 上位机可改）*/
} cfg_t;

/* 报警等级：界面状态灯和板上 LED 都按它走 */
typedef enum {
    ALARM_OK = 0,       /* 正常 —— 绿灯 */
    ALARM_WARN,         /* 警告 —— 黄灯 */
    ALARM_ALERT,        /* 报警 —— 红灯 */
} alarm_t;

/* ----------------------------------------------------------------
 * 调用顺序：osKernelInitialize() 之后、创建任务之前调一次。
 * 注意不能更早（要在 osKernelInitialize 之后才能建互斥锁）。
 * ---------------------------------------------------------------- */
void env_init(void);

/* 取环境快照（内部上锁，任何任务随时可调） */
void env_get(env_t *out);

/* SensorTask 用：三个 ADC 通道一次性更新（都是 0~100 指数） */
void env_set_adc(float light_idx, float smoke_idx, float co_idx);

/* VocTask 用：收到一帧合法数据 / 判定模块掉线 */
void env_set_voc(float t, float h, uint16_t eco2, uint16_t voc, uint16_t hcho);
void env_set_voc_lost(void);

/* 取 / 存配置（内部上锁） */
void cfg_get(cfg_t *out);
void cfg_set(const cfg_t *in);

/* 按当前阈值算报警等级。注意光照不参与 —— 它的方向是反的，
   属于"开灯控制"而不是报警。 */
alarm_t env_alarm_level(const env_t *e, const cfg_t *c);

#endif /* APP_ENV_H */
