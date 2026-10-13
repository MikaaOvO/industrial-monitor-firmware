#include "voc.h"
#include "uart_rx.h"    /* uart_rx_start_voc()：串口接收的机制层 */
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* ================================================================
 * ISR 和任务的分工
 *
 *   UART4 接收中断（优先级 5）—— 挂在 App/uart_rx.c 里
 *     └─ HAL_UART_RxCpltCallback → voc_feed_byte()   只攒字节
 *
 *   Task_Voc（任务上下文）
 *     └─ voc_take_frame() → voc_parse() → env_set_voc()
 *
 * 这个文件只管"一堆字节怎么组成一帧"，**不碰 HAL 的接收链路**。把字节
 * 捞出来、重新武装中断那部分在 uart_rx.c 里，两个串口共用一份 ——
 * 一个工程只能有一个 HAL_UART_RxCpltCallback 定义，所以不能在每个
 * 协议模块里各写一个。
 *
 * 特意**不在中断里碰 FreeRTOS 的任何 API**（不用 xTaskNotifyFromISR 之类）：
 * 中断只往一个静态缓冲区写字节、置一个 volatile 标志，任务自己轮询取。
 * 这样中断和 FreeRTOS 的优先级约束完全解耦 —— 就算以后有人把 UART4 的
 * NVIC 优先级调到 5 以下，也不会触发 configASSERT，只是收不到数据而已。
 *
 * 代价是最多 VOC_POLL_MS(20ms) 的处理延迟，而帧本身每秒才来一帧，
 * 完全可以忽略。
 * ================================================================ */

/* 攒帧用的缓冲区，由 ISR 写、由 voc_take_frame 读 */
static uint8_t  frame[VOC_FRAME_LEN];
static uint8_t  frame_idx;
static volatile bool frame_ready;   /* ISR 置位，任务清除 */

/* 诊断计数器。
   s_rx_bytes   在 ISR 里累加（volatile，任务只读）
   s_frames_ok  只在任务里读写，不需要 volatile
   都是 32 位对齐量，Cortex-M4 上的单个读写是原子的，不用保护。 */
static volatile uint32_t s_rx_bytes;
static uint32_t          s_frames_ok;

uint32_t voc_rx_bytes(void)  { return s_rx_bytes; }
uint32_t voc_frames_ok(void) { return s_frames_ok; }

void voc_note_ok_frame(void) { s_frames_ok++; }

/* 丢掉当前正在攒的半截帧，等下一帧重新对齐。
   启动时和串口出错后都会调。 */
void voc_feed_reset(void)
{
    frame_idx = 0;
}

bool voc_start(void)
{
    voc_feed_reset();
    frame_ready = false;

    /* 启动单字节中断接收。之后每收到一个字节进一次 UART4 中断，
       在 uart_rx.c 里重新武装，形成自维持的循环。
       中途不需要任务参与，所以帧与帧之间那一秒的空档不会漏字节。 */
    return uart_rx_start_voc();
}

void voc_feed_byte(uint8_t b)
{
    s_rx_bytes++;   /* 诊断计数器：只统计"中断里见到了多少个字节" */

    if (frame_idx == 0) {
        /* 还没和模块对上：只认帧头，其它字节一律丢掉。
           这样不管从帧中间哪个位置插进来，都能在下一帧自动对齐 ——
           比起"丢一个字节就整体错位"，这种设计能自愈。 */
        if (b == VOC_HEADER) {
            frame[0] = b;
            frame_idx = 1;
        }
        return;
    }

    frame[frame_idx++] = b;

    if (frame_idx >= VOC_FRAME_LEN) {
        frame_idx   = 0;        /* 下一帧从头开始重新找 */
        frame_ready = true;     /* 交给任务去校验和解析 */
    }
}

bool voc_take_frame(uint8_t *out)
{
    bool got = false;

    /* 关中断几百个周期，把整帧一次性抄走。
       不这么做的话，刚好在 ISR 写 frame[] 的中途去读，可能拿到半新半旧
       的一帧 —— 而这种错帧很可能会歪打正着地通过校验和，然后显示一个
       看起来正常但完全错误的数值。2µs 的中断遮蔽换这个确定性，划算。
       
       taskENTER_CRITICAL () /taskEXIT_CRITICAL () — FreeRTOS 临界区
       临界区代码，执行期间**禁止中断**，保证这一段代码不会被别的中断打断，
       用来保护**共享全局变量**，防止多任务 / 中断并发访问造成数据错乱。 */
    taskENTER_CRITICAL();
    if (frame_ready) {
        memcpy(out, frame, VOC_FRAME_LEN);
        frame_ready = false;
        got = true;
    }
    taskEXIT_CRITICAL();

    return got;
}

/* ---- 单位换算 ---- */

/* 温度：16 位，单位 0.1℃。
 *
 * 手册的负温度编码：最高位为 1 表示零下，幅度 = 0xFFFF - 原始值。
 * 注意它和标准 int16 补码**差 1**！手册的例子：-10 记作 0xFFF5，
 * 而 0xFFF5 按补码读出来是 -11。这里按手册的规则实现。
 * 室内监测基本遇不到零下，但别改成 (int16_t)raw 那种写法。 */
static float temp_from_bytes(uint8_t hi, uint8_t lo)
{
    uint16_t raw = ((uint16_t)hi << 8) | lo;

    if (raw & 0x8000) {                     /* 最高位为 1 = 零下 */
        int32_t mag = (int32_t)0xFFFF - (int32_t)raw;
        return -(float)mag * 0.1f;
    }
    return (float)raw * 0.1f;
}

/* 湿度：16 位，单位 0.1%RH */
static float humi_from_bytes(uint8_t hi, uint8_t lo)
{
    uint16_t raw = ((uint16_t)hi << 8) | lo;
    return (float)raw * 0.1f;
}

/* 校验和 */
bool voc_parse(const uint8_t *f, voc_data_t *out)
{
    if (f == NULL || out == NULL)   return false;
    if (f[0] != VOC_HEADER)         return false;

    /* 校验和：前 11 个字节累加，取反 +1（也就是 0 - sum，取低 8 位）。
       按手册的措辞写，方便对着手册核对。 */
    uint8_t sum = 0;
    for (int i = 0; i < VOC_FRAME_LEN - 1; i++) {
        sum += f[i];
    }
    uint8_t expect = (uint8_t)(~sum + 1);
    if (f[VOC_FRAME_LEN - 1] != expect) return false;

    /* 三个 16 位数都是**高位在前**。
       注意是 <<8 不是 *28 —— 手册上印的 "Data[n]*28" 是 PDF 把 2^8 的
       上标 8 掉了格式。用手册自己的例子验证：它说 0x013D = 317 = 31.7℃，
       而 0x01<<8 | 0x3D = 256 + 61 = 317 ✅；按 *28 算只有 89 ❌。 */
    out->voc_ugm3  = ((uint16_t)f[1] << 8) | f[2];
    out->hcho_ugm3 = ((uint16_t)f[3] << 8) | f[4];
    out->eco2_ppm  = ((uint16_t)f[5] << 8) | f[6];
    out->temp_c    = temp_from_bytes(f[7], f[8]);
    out->humi_rh   = humi_from_bytes(f[9], f[10]);

    return true;
}

/* 注意：HAL_UART_RxCpltCallback / HAL_UART_ErrorCallback 不在这里，
   在 App/uart_rx.c。一个工程只能有一份定义（HAL 里是 __weak），
   两个串口要共用同一个回调，所以统一放在那个"机制层"文件里分发。 */
