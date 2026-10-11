#include "app_env.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* ================================================================
 * 共享数据的落地实现
 *
 * 一把互斥锁保护 g_env 和 g_cfg 两个结构体。
 * 这里没有用"队列传值"，因为传感器数据只关心最新值：
 * 队列会把过时的采样堆在后面，读的人拿到的反而是旧数据。
 * ================================================================ */

/* 配置默认值。必须和 ui.c 里滑块的初始值保持一致，
   这样开机时界面不用回读就已经是对的（ui_init 在调度器启动前跑，
   那时还不能取锁，只能靠两边默认值对齐）。 */
#define DEF_TEMP_HI     28.0f
#define DEF_HUMI_HI     70.0f
#define DEF_SMOKE_HI    15.0f
#define DEF_GAS_HI      15.0f
#define DEF_ECO2_HI     1000
#define DEF_LIGHT_LO    30.0f

/* 达到阈值的百分之多少就升级成"警告" */
#define WARN_RATIO      0.8f

static osMutexId_t env_mutex = NULL;

static env_t g_env;     /* 只在锁内读写 */
static cfg_t g_cfg;

/* 调度器还没启动时（只有 ui_init 那一小段）不能取锁，
   那时是单线程，直接裸读反而安全 */
static bool sched_running(void)
{
    return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

static void env_lock(void)
{
    if (env_mutex != NULL && sched_running()) {
        osMutexAcquire(env_mutex, osWaitForever);
    }
}

static void env_unlock(void)
{
    if (env_mutex != NULL && sched_running()) {
        osMutexRelease(env_mutex);
    }
}

void env_init(void)
{
    memset(&g_env, 0, sizeof(g_env));

    g_cfg.temp_hi    = DEF_TEMP_HI;
    g_cfg.humi_hi    = DEF_HUMI_HI;
    g_cfg.smoke_hi   = DEF_SMOKE_HI;
    g_cfg.gas_hi     = DEF_GAS_HI;
    g_cfg.eco2_hi    = DEF_ECO2_HI;
    g_cfg.light_lo   = DEF_LIGHT_LO;
    g_cfg.auto_fan   = false;
    g_cfg.manual_fan = false;

    /* 在 osKernelStart() 之前建好，任务一跑起来就能用 */
    if (env_mutex == NULL) {
        env_mutex = osMutexNew(NULL);
    }
}

void env_get(env_t *out)
{
    if (out == NULL) return;
    env_lock();
    *out = g_env;
    env_unlock();
}

void env_set_adc(float light_idx, float smoke_idx, float co_idx)
{
    env_lock();
    g_env.light_idx = light_idx;
    g_env.smoke_idx = smoke_idx;
    g_env.co_idx    = co_idx;
    env_unlock();
}

void env_set_voc(float t, float h, uint16_t eco2, uint16_t voc, uint16_t hcho)
{
    env_lock();
    g_env.temp_c     = t;
    g_env.humi_rh    = h;
    g_env.eco2_ppm   = eco2;
    g_env.voc_ugm3   = voc;
    g_env.hcho_ugm3  = hcho;
    g_env.voc_ok     = true;
    g_env.voc_online = true;
    env_unlock();
}

void env_set_voc_lost(void)
{
    env_lock();
    g_env.voc_ok     = false;
    g_env.voc_online = false;
    env_unlock();
}

void cfg_get(cfg_t *out)
{
    if (out == NULL) return;
    env_lock();
    *out = g_cfg;
    env_unlock();
}

void cfg_set(const cfg_t *in)
{
    if (in == NULL) return;
    env_lock();
    g_cfg = *in;
    env_unlock();
}

alarm_t env_alarm_level(const env_t *e, const cfg_t *c)
{
    if (e == NULL || c == NULL) return ALARM_OK;

    /* 21VOC 掉线时它那几路数据不可信，不参与报警判定 */
    bool voc = e->voc_online;

    /* 优先级：气体 > CO2 > 温度/湿度。
       气体超标是安全事件，直接报警；另外几路先判警告再判报警。
       注意光照不在这个函数里 —— 它是"低于阈值开灯"，方向相反。 */

    if (e->co_idx >= c->gas_hi || e->smoke_idx >= c->smoke_hi)  return ALARM_ALERT;
    if (voc && e->eco2_ppm >= c->eco2_hi)                       return ALARM_ALERT;
    if (voc && (e->temp_c >= c->temp_hi || e->humi_rh >= c->humi_hi)) return ALARM_ALERT;

    if (e->co_idx    >= c->gas_hi   * WARN_RATIO ||
        e->smoke_idx >= c->smoke_hi * WARN_RATIO)               return ALARM_WARN;
    if (voc && e->eco2_ppm >= (uint16_t)(c->eco2_hi * WARN_RATIO)) return ALARM_WARN;
    if (voc && (e->temp_c >= c->temp_hi * WARN_RATIO ||
                e->humi_rh >= c->humi_hi * WARN_RATIO))         return ALARM_WARN;

    return ALARM_OK;
}
