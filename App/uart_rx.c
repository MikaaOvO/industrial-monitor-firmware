#include "uart_rx.h"
#include "voc.h"
#include "host_proto.h"
#include "main.h"       /* huart2 / huart4 的类型 */

/* CubeMX 把两个句柄都定义在 Core/Src/main.c 里，main.h 没有 extern 声明 */
extern UART_HandleTypeDef huart4;
extern UART_HandleTypeDef huart2;

/* 每个串口一个接收暂存字节。HAL_UART_Receive_IT 一次收一个，
   地址必须稳定，所以不能放在栈上。 */
static uint8_t voc_byte;
static uint8_t host_byte;

bool uart_rx_start_voc(void)
{
    /* 先查 NVIC 有没有使能这个中断。
       HAL_UART_Receive_IT 只开 UART 内部的 RXNE 中断位，**不检查 NVIC** ——
       CubeMX 里忘了勾"全局中断"的话它会老老实实返回 HAL_OK，然后一个字节
       都收不到，属于静默失败。这里显式挡一下，把它变成开机就能发现的错误。 */
    if (NVIC_GetEnableIRQ(UART4_IRQn) == 0) return false;
    return HAL_UART_Receive_IT(&huart4, &voc_byte, 1) == HAL_OK;
}

bool uart_rx_start_host(void)
{
    if (NVIC_GetEnableIRQ(USART2_IRQn) == 0) return false;
    return HAL_UART_Receive_IT(&huart2, &host_byte, 1) == HAL_OK;
}

/* ================================================================
 * HAL 回调（ISR 上下文）—— 全工程唯一的两个定义
 * ================================================================ */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == UART4) {
        voc_feed_byte(voc_byte);
        /* 立刻重新武装，准备收下一个字节。
           这里到下次 RXNE 之间只有几微秒，而 9600 波特下字节间隔 1.04ms，
           不会漏。USART2 更宽（115200 下 87µs），也是同样的道理。 */
        HAL_UART_Receive_IT(&huart4, &voc_byte, 1);
    } else if (huart->Instance == USART2) {
        host_rx_byte(host_byte);
        HAL_UART_Receive_IT(&huart2, &host_byte, 1);
    }
}

/* 出错回调：F4 的串口在过载(ORE)/帧错误后会卡住不再接收，必须清标志并
   重新武装，否则模块一上电的杂波、或者插拔线的瞬态，就能把接收永久打死。
   顺手把半截帧丢掉，等下一帧重新对齐。 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == UART4) {
        __HAL_UART_CLEAR_OREFLAG(huart);
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        voc_feed_reset();
        HAL_UART_Receive_IT(&huart4, &voc_byte, 1);
    } else if (huart->Instance == USART2) {
        __HAL_UART_CLEAR_OREFLAG(huart);
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        host_rx_reset();
        HAL_UART_Receive_IT(&huart2, &host_byte, 1);
    }
}
