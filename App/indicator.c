#include "indicator.h"
#include "main.h"       /* TIM_HandleTypeDef / htim1 等 HAL 类型 */

/* CubeMX 把 htim1 定义在 Core/Src/main.c 里，main.h 没有 extern 声明它 */
extern TIM_HandleTypeDef htim1;

/* ================================================================
 * 风扇
 *
 * TB6612 的真值表里 AIN1=H / AIN2=L 就是"正转"，所以转速和开关
 * 完全由 PWMA 的占空比决定：
 *
 *     duty = 0    → 停
 *     duty = 100  → 全速
 *
 * AIN1 / AIN2 / STBY 都在硬件上接死了，软件碰不到 —— 也就不可能
 * 出现"两个方向脚同时为高"（那在 TB6612 上是短路制动）这类软件失误。
 * ================================================================ */

/* ---- 三档转速（百分比），可调 ----
 *
 *   MANUAL      手动通风
 *   AUTO_WARN   自动模式 + 警告级
 *   AUTO_ALERT  自动模式 + 报警级
 *
 * 占空比按平均电压算：D × VM。VM 是 5V，所以 100% 就是持续给电机 5V。
 * 如果哪天发现电机长期运行明显发烫（不是温热，是手不敢一直按），
 * 把对应那一档调低即可。
 *
 * 但**别降到电机启动不了** —— 那会变成堵转：转子不转、电流比正常
 * 运转大得多，几秒钟就能烫到冒烟。症状是风扇不转、电机哼哼响、
 * 迅速发烫，看到立刻断电并调回去。
 */
#define FAN_DUTY_MANUAL      100    /* 手动通风 */
#define FAN_DUTY_AUTO_WARN    60    /* 自动 + 警告级 */
#define FAN_DUTY_AUTO_ALERT  100    /* 自动 + 报警级 */

/* 每次更新最多变化多少个百分点。避免 0 → 100 的启动电流冲击，
   也让转速变化看得顺眼（Task_Output 是 100ms 一次，所以 0→100 约 1 秒）。
   小风扇其实直接全速也没事，但斜率限制是电机驱动的常规做法。 */
#define FAN_SLEW_STEP         10

static uint32_t fan_cur_pct = 0;    /* 当前实际输出，用于限速 */

/* 按百分比设置占空比。
 *
 * 注意这里是从寄存器读 ARR 再算比较值，而不是把"CCR 就等于百分比"
 * 写死。因为 CCR 的满量程是 ARR+1，而 ARR 是可以在 CubeMX 里改的
 * （本工程配的是 99，即 20kHz）。从寄存器读的话，以后改 Counter Period
 * 这段代码不用跟着动，也就不会出现"改了 CubeMX 但代码里还按旧比例算"
 * 这种不报错的错位。 */
static void fan_set_pct(uint32_t pct)
{
    if (pct > 100) pct = 100;

    uint32_t span = __HAL_TIM_GET_AUTORELOAD(&htim1) + 1;
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, span * pct / 100);
}

/* 带斜率限制的设定：每调一次最多走 FAN_SLEW_STEP 个百分点。
   加速和减速都限速 —— 关风扇时也是平滑停下，不会瞬间断电。 */
static void fan_set_pct_slew(uint32_t target)
{
    if (target > 100) target = 100;

    if (target > fan_cur_pct) {
        fan_cur_pct += FAN_SLEW_STEP;
        if (fan_cur_pct > target) fan_cur_pct = target;
    } else if (target < fan_cur_pct) {
        fan_cur_pct = (fan_cur_pct > FAN_SLEW_STEP) ? (fan_cur_pct - FAN_SLEW_STEP) : 0;
        if (fan_cur_pct < target) fan_cur_pct = target;
    }

    fan_set_pct(fan_cur_pct);
}

void indicator_init(void)
{
    /* 先把比较值清零再启动，保证上电时风扇是停的 ——
       否则会先用 CubeMX 里 Pulse 的初值转一下。 */
    fan_cur_pct = 0;
    fan_set_pct(0);

    /* 启动 PWM。TIM1 是"高级定时器"，HAL_TIM_PWM_Start 内部会顺带打开
       主输出使能 MOE —— 不打开的话引脚上是不会有波形的，而且这一点
       从代码上完全看不出来（TIM1/TIM8 特有的坑）。 */
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
}

void indicator_update(const env_t *e, const cfg_t *c)
{
    if (e == NULL || c == NULL) return;

    alarm_t a = env_alarm_level(e, c);

    /* 手动通风优先开；自动模式在"警告及以上"就提前通风，不等真超标。
       两者是"或"的关系：
         - 手动开着时，自动逻辑压不住它
         - 反过来报警时，手动也关不掉 —— 刻意不允许在报警状态下
           手动把风机关掉 */
    uint32_t target = 0;

    if (c->manual_fan) {
        target = FAN_DUTY_MANUAL;
    } else if (c->auto_fan && a != ALARM_OK) {
        target = (a == ALARM_ALERT) ? FAN_DUTY_AUTO_ALERT : FAN_DUTY_AUTO_WARN;
    }

    fan_set_pct_slew(target);
}
