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

    /* ---- 21VOC 通信诊断计数器 ----
       界面和上位机都可以拿它判断链路状态：
         voc_rx_bytes  长期不涨 → 模块没在发 / 线没接对 / 波特率不对
         voc_rx_bytes 在涨而 voc_frames_ok 不涨
                       → 字节到了，但帧收不全或校验不过（波特率/协议）*/
    uint32_t voc_rx_bytes;
    uint32_t voc_frames_ok;
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
 * 各阈值的合法取值范围
 *
 * 界面滑块的范围和上位机下发时的钳制都用这一份，只在这里定义一次。
 * 分成两处定义的话，改一边不生效，而且编译器只给警告不给错误
 * （docs/工业环境检测系统-问题处理.md 里的 P-12 就是这么栽的）。
 * ---------------------------------------------------------------- */
#define CFG_TEMP_MIN     20.0f      /* ℃   */
#define CFG_TEMP_MAX     40.0f
#define CFG_HUMI_MIN      0.0f      /* %RH  */
#define CFG_HUMI_MAX    100.0f
#define CFG_SMOKE_MIN     0.0f      /* 0~100 相对指数 */
#define CFG_SMOKE_MAX   100.0f
#define CFG_GAS_MIN       0.0f
#define CFG_GAS_MAX     100.0f
#define CFG_LIGHT_MIN     0.0f
#define CFG_LIGHT_MAX   100.0f
#define CFG_ECO2_MIN      400       /* ppm */
#define CFG_ECO2_MAX     2000

/* ----------------------------------------------------------------
 * 警告级的绝对门限（介于"正常"和"报警阈值"之间的那一级）
 *
 * 这些是**独立的绝对值**，不从用户的报警阈值推出来。理由：
 * 一个量"偏高"的绝对范围是固定的（26℃ 就是偏热，不管用户把报警阈值
 * 设成 28 还是 40）；而从阈值按百分比推会在阈值时算出荒唐的结果 ——
 * 28℃ 的 80% 是 22.4℃，那是完全正常的室温，状态灯会常年亮黄。
 *
 * 这组值只用于"警告"，报警仍然完全按用户在界面上/上位机设的阈值走。
 * 如果用户把阈值设得比这里还低，就会直接跳到报警级（报警判定在前），
 * 不会出现"先警告再报警"的倒挂，这是预期行为。
 * ---------------------------------------------------------------- */
#define CFG_TEMP_WARN     26.0f     /* ℃    */
#define CFG_HUMI_WARN     65.0f     /* %RH  */
#define CFG_ECO2_WARN      800      /* ppm  */
#define CFG_INDEX_WARN    60.0f     /* 0~100 指数（光照/烟雾/有毒气体）*/

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

/* VocTask 用：更新通信诊断计数器（界面用它判断链路通没通） */
void env_set_voc_debug(uint32_t rx_bytes, uint32_t frames_ok);

/* 取 / 存配置（内部上锁） */
void cfg_get(cfg_t *out);
void cfg_set(const cfg_t *in);

/* 按当前阈值算报警等级。注意光照不参与 —— 它的方向是反的，
   属于"开灯控制"而不是报警。 */
alarm_t env_alarm_level(const env_t *e, const cfg_t *c);

#endif /* APP_ENV_H */
