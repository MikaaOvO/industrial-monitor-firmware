#ifndef VOC_H
#define VOC_H

#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * 21VOC 模块（广州传苣电子）
 *
 * 硬件：UART4，PC10=TX（可不接）、PC11=RX，9600 8N1，无校验位。
 *       模块 VCC=3.3V，TX 输出就是 3.3V 电平，可直连 PC11，不需要分压。
 *       开机立即上报，无需预热（这点比 MQ 系列省心）。
 *
 * 帧：固定 12 字节，模块主动上报，约每秒一帧。
 *
 *   [0]     帧头 0x2C
 *   [1..2]  VOC      ug/m3
 *   [3..4]  甲醛     ug/m3
 *   [5..6]  eCO2     ppm
 *   [7..8]  温度     0.1℃
 *   [9..10] 湿度     0.1%RH
 *   [11]    校验和 = 前 11 字节累加和，取反 +1
 *
 * 手册里有两个坑，实现在 voc.c 里，改的时候别踩回去：
 *   ① 手册印的是 Data[n]*28，实际是 *256（PDF 把 2^8 的上标吃掉了）。
 *   ② 负温度的编码和标准补码差 1。
 * ================================================================ */

#define VOC_FRAME_LEN   12      /* 帧长，固定 */
#define VOC_HEADER      0x2C    /* 帧头 */

/* 任务轮询周期。帧大约每秒一帧，20ms 完全够用，
   存在的意义只是让"取帧 → 写共享数据"这一步尽快发生。 */
#define VOC_POLL_MS     20

/* 超过这么久没收到一帧合法数据，就判定模块掉线，界面回落到占位符。
 *
 * 实测（2026-10-12）：模块 1 秒一帧（60 秒收到 60 帧）。
 * 取 5 秒 = 连续丢 5 帧才判掉线，既能容忍偶发丢帧，掉线判定也不会太迟钝。
 *
 * 改这个值之前先确认上报间隔 —— 它必须明显大于间隔，否则每收到一帧刚
 * 显示一下就被判掉线打回占位符，看起来就像"一直没数据"。
 * 测法：把 ui.c 的 VOC_DEBUG_UI 打开，CO2 卡会显示帧数，
 *       记 60 秒涨了多少，平均间隔 ≈ 60/N 秒。 */
#define VOC_TIMEOUT_MS  5000

typedef struct {
    float    temp_c;        /* 温度 ℃      */
    float    humi_rh;       /* 湿度 %RH    */
    uint16_t eco2_ppm;      /* eCO2 ppm    */
    uint16_t voc_ugm3;      /* VOC ug/m3   */
    uint16_t hcho_ugm3;     /* 甲醛 ug/m3  */
} voc_data_t;

/* 启动单字节中断接收。由 Task_Voc 在进循环前调一次。
   （收满一帧的后续处理在中断里不做，交给任务，见 voc_feed_byte 注释） */
bool voc_start(void);

/* ---- 下面三个是给 ISR / 任务分工用的，一般不用直接调 ---- */

/* ISR 上下文：喂一个字节进帧状态机。只做"攒字节"，不校验、不碰共享数据。 */
void voc_feed_byte(uint8_t b);

/* ISR 上下文：丢掉当前正在攒的半截帧，等下一帧重新对齐。
   串口出错（过载/帧错误）后由 uart_rx.c 调用。 */
void voc_feed_reset(void);

/* 任务上下文：若已收满一帧则复制出来并清标志，返回 true。
   内部用临界区保护，和 ISR 之间不会读到半截帧。 */
bool voc_take_frame(uint8_t *out);

/* 任务上下文：校验和 + 解析。校验不过返回 false。
   帧头不对、长度不对、校验和不对，都返回 false。 */
bool voc_parse(const uint8_t *frame, voc_data_t *out);

void voc_note_ok_frame(void);   /* 任务解析成功后调，累加诊断计数器 */

/* ---- 诊断计数器（判断链路通没通）----
 *   voc_rx_bytes()   ISR 里累加，只增不减
 *   voc_frames_ok()  每解析成功一帧 +1
 * 两个数的关系见 env_t 里的注释。 */
uint32_t voc_rx_bytes(void);
uint32_t voc_frames_ok(void);

#endif /* VOC_H */
