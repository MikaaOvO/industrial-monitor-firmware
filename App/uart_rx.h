#ifndef UART_RX_H
#define UART_RX_H

#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * 串口传输层
 *
 * 这个文件只干三件事：把字节从串口捞出来、重新武装接收中断、按需
 * 处理 RS485 的方向控制。**不碰协议** —— 字节怎么组成帧是 host_proto.c
 * 和 voc.c 的事。
 *
 * 三路串口：
 *
 *   UART4   —— 21VOC 模块，只收（模块主动上报）
 *   USART2  —— CH340，下位机调试链路
 *   USART6  —— MAX3485，上位机 RS485 链路
 *
 * ---- 上位机链路的编号 ----
 * 两条上位机链路跑同一套协议，所以协议层要知道字节是从哪条来的 ——
 * 否则两条链路的半截命令会混在同一个行缓冲里。协议层用下面这个编号
 * 区分，各自维护自己的一份接收状态。
 * ================================================================ */
enum {
    UART_LINK_DEBUG = 0,    /* USART2 + CH340   —— 下位机调试 */
    UART_LINK_BUS   = 1,    /* USART6 + MAX3485 —— 上位机 RS485 总线 */
    UART_LINK_COUNT
};

/* ---- 收 ---- */

/* 启动 21VOC（UART4）的接收中断。Task_Voc 进循环前调一次。 */
bool uart_rx_start_voc(void);

/* 启动某条上位机链路的接收中断。lk 取 UART_LINK_*。
   返回 false 说明 HAL 没接受 —— 最常见的原因是 CubeMX 里忘了勾
   这个串口的全局中断（HAL_UART_Receive_IT 自己不检查 NVIC，
   会返回 HAL_OK 然后一个字节都收不到，所以这里显式查了 NVIC）。 */
bool uart_rx_start_host(int lk);

/* ---- 发 ---- */

/* 在某条上位机链路上发一帧。
 *
 * lk   —— 取 UART_LINK_*
 * data —— 要发的字节，**不需要**以 '\0' 结尾
 * len  —— 要发的字节数
 *
 * 内部处理 RS485 的方向控制（DE 拉高 → 发送 → DE 拉低）。
 * 发送是阻塞的，一帧约 200 字节在 115200 下约 17ms。
 */
bool uart_tx_send(int lk, const uint8_t *data, uint16_t len);

#endif /* UART_RX_H */
