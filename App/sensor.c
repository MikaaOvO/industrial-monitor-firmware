#include "sensor.h"
#include "main.h"       /* ADC_HandleTypeDef 等 HAL 类型 */
#include "cmsis_os.h"
#include <stdint.h>

/* CubeMX 把 hadc1 定义在 Core/Src/main.c 里，而 main.h 没有 extern
   声明它，所以这里自己声明一次。ADC1 只有一个实例，不会有歧义。 */
extern ADC_HandleTypeDef hadc1;

/* ================================================================
 * ADC1 通道 → 数组下标的映射
 *
 * !!! 这张表必须和 CubeMX 里 ADC1 的 Rank 顺序严格一致 !!!
 *
 *   CubeMX Rank   通道        引脚    传感器   →  adc_buf 下标
 *   -----------   ---------   -----   ------      -----------
 *   Rank 1        Channel 0   PA0     MQ-2         [0]
 *   Rank 2        Channel 1   PA1     MQ-7         [1]
 *   Rank 3        Channel 10  PC0     光敏         [2]
 *
 * 以后往 CubeMX 里加通道（比如 PC1 的热敏），一定要加在序列最后，
 * 然后只改这里的 ADC_CH_COUNT。插在中间会让 MQ-2 读到 MQ-7 的值，
 * 而且不报错，极其难查。
 * ================================================================ */
#define ADC_CH_COUNT    3
#define CH_SMOKE        0
#define CH_CO           1
#define CH_LIGHT        2

/* DMA 的搬运目标。
 * 必须是普通 SRAM —— 千万不要加 __attribute__((section(".ccmram")))，
 * DMA 访问不到 CCM RAM，加了之后缓冲区永远不动。
 * LVGL 的堆可以搬去 CCM，这块不行。 */
static uint16_t adc_buf[ADC_CH_COUNT];

/* ================================================================
 * 两点标定表 —— 这是整个文件里唯一需要你实测填的地方
 *
 * 现在填的是占位值（0 和 4095），效果等于"直接输出 ADC 满量程百分比"。
 * 这样烧进去三个数字就会跟着传感器动，能立刻验证 ADC 通不通。
 *
 * 等你有空实测了，把每一行的 raw0 / raw100 换成实测的原始读数：
 *
 *   通道     0 点怎么造                    100 点怎么造
 *   ------   ---------------------------   --------------------------
 *   光敏     手指完全盖住 / 塞进抽屉        手机手电筒贴近照
 *   MQ-2     开窗通风静置 5~10 分钟         酒精棉片放在旁边（别贴上去）
 *   MQ-7     同上，干净空气就是 0           打火机气在旁边释放
 *
 * 注意：
 *   1) MQ 系列必须先预热完（约 5 分钟）再测 0 点，刚上电漂移很大。
 *   2) 打火机气千万别对着 21VOC 模块喷！那个 MEMS 传感器会被打坏，
 *      手册里明确写了。MQ 是半导体式，用打火机气没问题，但要离远点。
 *   3) 标完气体后 MQ 基线要几小时才恢复，所以先标光敏、再标气体。
 *
 * 为什么不用读 VREFINT 去校准 VDDA：因为标定是在你这块板子上做的，
 * 电源偏差和 1k/1k 分压的实际比值都会被这两个常量吸收掉。
 * ================================================================ */
static const struct {
    uint16_t raw0;      /* 0 点：干净空气 / 全暗时的原始读数   */
    uint16_t raw100;    /* 100 点：极端工况时的原始读数        */
} cal[ADC_CH_COUNT] = {
    /* raw0  raw100 */
    {    0,   4095 },   /* [0] MQ-2 烟雾     (PA0 / Rank 1) */
    {    0,   4095 },   /* [1] MQ-7 有毒气体 (PA1 / Rank 2) */
    {    0,   4095 },   /* [2] 光敏          (PC0 / Rank 3) */
};

/* ---- 运行时状态：指数滑动平均，程序自己维护，别手动改 ---- */
static float ema[ADC_CH_COUNT];
static bool  ema_seeded = false;

/* ---- 滤波参数 ---- */
#define MEDIAN_N    5       /* 每通道连续采 5 次，取中位（去掉尖峰）*/
#define MEDIAN_GAP  1       /* 两次采样间隔 ms。要 >0 才能拿到新值，
                               否则 5 次读到的都是同一轮 DMA 结果 */
#define EMA_ALPHA   0.35f   /* 滑动平均系数。越小越平滑、越迟钝。
                               0.35 大约 1 秒收敛（100ms 周期下） */

/* ================================================================ */

/* n 很小（5），直接插入排序后取中间那个 */
static uint16_t median_of(uint16_t *v, int n)
{
    for (int i = 1; i < n; i++) {
        uint16_t k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > k) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = k;
    }
    return v[n / 2];
}

bool sensor_init(void)
{
    /* 这个工程里的 F4 HAL 没有提供校准 API（adc_ex.c 里只有注入通道和
       多模式的函数），而且 HAL_ADC_Init 内部也没做。不校准的代价是几个
       LSB 的偏移，折算到 0~100 指数上不到 0.1 个点 —— 而两点标定本来
       就会把这类系统误差吸收掉，所以这里不折腾。 */

    /* 启动一次就够：ContinuousConvMode + DMA Circular + DMA Continuous
       Requests 都已在 CubeMX 里打开，之后硬件自己在转、自己在搬。
       第三个参数是"转换个数"，不是字节数。 */
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buf, ADC_CH_COUNT) != HAL_OK) {
        return false;
    }

    return true;
}

void sensor_read(float *light_idx, float *smoke_idx, float *co_idx)
{
    uint16_t s[ADC_CH_COUNT][MEDIAN_N];

    /* ---- 1. 采样：每通道连续取 MEDIAN_N 个点 ----
       adc_buf 里的每个元素都是 uint16_t，DMA 写半字是原子的，
       所以这里读单个元素不会读到"撕开"的值，不需要关中断。 */
    for (int k = 0; k < MEDIAN_N; k++) {
        for (int ch = 0; ch < ADC_CH_COUNT; ch++) {
            s[ch][k] = adc_buf[ch];
        }
        osDelay(MEDIAN_GAP);
    }

    float out[ADC_CH_COUNT];

    for (int ch = 0; ch < ADC_CH_COUNT; ch++) {
        /* ---- 2. 中值滤波：去掉单点尖峰 ---- */
        uint16_t raw = median_of(s[ch], MEDIAN_N);

        /* ---- 3. 两点标定：raw → 0~100 ---- */
        float idx;
        int32_t span = (int32_t)cal[ch].raw100 - (int32_t)cal[ch].raw0;
        if (span > 0) {
            /* 用 int32_t 做减法：raw 小于 raw0 时如果按无符号算会溢出成
               一个巨大的数，指数直接顶到 100。 */
            idx = 100.0f * (float)((int32_t)raw - (int32_t)cal[ch].raw0)
                        / (float)span;
        } else {
            idx = 0.0f;     /* 端点填反了，退化成 0，也顺便避免除零 */
        }

        /* 限幅：标定端点之外的读数夹到 0~100，别让界面显示 -3 或 137 */
        if (idx < 0.0f)   idx = 0.0f;
        if (idx > 100.0f) idx = 100.0f;

        /* ---- 4. 指数滑动平均：跨周期平滑，抑制数字跳动 ----
           第一次直接就位，不从 0 慢慢爬上来（否则开机头一秒界面会显示
           一个从 0 涨上去的假数值）。 */
        if (!ema_seeded) {
            ema[ch] = idx;
        } else {
            ema[ch] += (idx - ema[ch]) * EMA_ALPHA;
        }
        out[ch] = ema[ch];
    }

    ema_seeded = true;

    if (light_idx) *light_idx = out[CH_LIGHT];
    if (smoke_idx) *smoke_idx = out[CH_SMOKE];
    if (co_idx)    *co_idx    = out[CH_CO];
}
