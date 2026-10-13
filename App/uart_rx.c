#include "uart_rx.h"
#include "voc.h"
#include "host_proto.h"
#include "main.h"       /* huartX / RS485_DE_Pin 等 */

/* CubeMX 把三个句柄都定义在 Core/Src/main.c 里，main.h 没有 extern 声明 */
extern UART_HandleTypeDef huart4;
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart6;

/* 每路一个接收暂存字节。HAL_UART_Receive_IT 一次收一个，
   地址必须稳定，所以不能放在栈上。 */
static uint8_t voc_byte;
static uint8_t host_byte[UART_LINK_COUNT];

/* 发送超时（ms）。一帧约 200 字节在 115200 下约 17ms，给足余量。
   超时太长的话，万一硬件有问题会把 Task_Host 卡住。 */
#define UART_TX_TIMEOUT_MS  100

/* 链路号 → 串口句柄 */
static UART_HandleTypeDef *host_uart(int lk)
{
    return (lk == UART_LINK_BUS) ? &huart6 : &huart2;
}

/* 链路号 → NVIC 中断号（用来查 CubeMX 到底勾没勾） */
static IRQn_Type host_irqn(int lk)
{
    return (lk == UART_LINK_BUS) ? USART6_IRQn : USART2_IRQn;
}

/* ================================================================ */

bool uart_rx_start_voc(void)
{
    /* 先查 NVIC。HAL_UART_Receive_IT 只开 UART 内部的 RXNE 中断位，
       **不检查 NVIC** —— CubeMX 里忘了勾"全局中断"的话它会老老实实
       返回 HAL_OK，然后一个字节都收不到，属于静默失败。
       这里显式挡一下，把它变成开机就能发现的错误。 */
    if (NVIC_GetEnableIRQ(UART4_IRQn) == 0) return false;
    return HAL_UART_Receive_IT(&huart4, &voc_byte, 1) == HAL_OK;
}

bool uart_rx_start_host(int lk)
{
    if (lk < 0 || lk >= UART_LINK_COUNT) return false;
    if (NVIC_GetEnableIRQ(host_irqn(lk)) == 0) return false;

    return HAL_UART_Receive_IT(host_uart(lk), &host_byte[lk], 1) == HAL_OK;
}

bool uart_tx_send(int lk, const uint8_t *data, uint16_t len)
{
    if (lk < 0 || lk >= UART_LINK_COUNT) return false;
    if (data == NULL || len == 0)        return false;

    UART_HandleTypeDef *h = host_uart(lk);

    if (lk == UART_LINK_BUS) {
        /* ---- RS485 半双工：先把 DE 拉高切到发送 ----
           （模块内部 DE 和 /RE 短接，所以拉高 = 驱动器开、接收器关。）*/
        HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_SET);
    }

    HAL_StatusTypeDef st = HAL_UART_Transmit(h, (uint8_t *)data, len,
                                             UART_TX_TIMEOUT_MS);

    if (lk == UART_LINK_BUS) {
        /* 发完拉低切回接收。
         *
         * **这里能直接切，是因为 HAL_UART_Transmit 是阻塞发送，
         * 而且它在返回前会等 TC（Transmission Complete）标志** ——
         * TC 置位代表最后一个字节真的移出了移位寄存器，不是只写进了
         * 数据寄存器。
         *
         * !! 如果以后把发送改成 DMA，这段话必须重新处理 !! DMA 的
         * 传输完成中断只代表"最后一个字节写进了 DR"，此时它还在移位
         * 寄存器里往外发。那时候提前拉低 DE 会把最后一个字节截断，
         * 而且现象很隐蔽（只是偶尔少一个字节、校验和不过）。
         * 正确做法是等 USART_SR 的 TC 位，或改用 HAL_UART_TxCpltCallback。 */
        HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
    }

    return st == HAL_OK;
}

/* ================================================================
 * HAL 回调（ISR 上下文）—— 全工程唯一的两个定义
 *
 * 一个工程只能有一个 HAL_UART_RxCpltCallback（HAL 里是 __weak，
 * 重定义会链接冲突），所以三路串口都在这里按 Instance 分发。
 * ================================================================ */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == UART4) {
        voc_feed_byte(voc_byte);
        /* 立刻重新武装，准备收下一个字节。
           这里到下次 RXNE 之间只有几微秒，而 9600 波特下字节间隔
           1.04ms，不会漏。 */
        HAL_UART_Receive_IT(&huart4, &voc_byte, 1);
    } else if (huart->Instance == USART2) {
        host_rx_byte(UART_LINK_DEBUG, host_byte[UART_LINK_DEBUG]);
        HAL_UART_Receive_IT(&huart2, &host_byte[UART_LINK_DEBUG], 1);
    } else if (huart->Instance == USART6) {
        host_rx_byte(UART_LINK_BUS, host_byte[UART_LINK_BUS]);
        HAL_UART_Receive_IT(&huart6, &host_byte[UART_LINK_BUS], 1);
    }
}

/* 出错回调：F4 的串口在过载(ORE)/帧错误后会卡住不再接收，必须清标志
   并重新武装，否则模块一上电的杂波、或者插拔线的瞬态，就能把接收
   永久打死。顺手把半截帧丢掉，等下一帧重新对齐。 */
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
        host_rx_reset(UART_LINK_DEBUG);
        HAL_UART_Receive_IT(&huart2, &host_byte[UART_LINK_DEBUG], 1);
    } else if (huart->Instance == USART6) {
        __HAL_UART_CLEAR_OREFLAG(huart);
        huart->ErrorCode = HAL_UART_ERROR_NONE;
        host_rx_reset(UART_LINK_BUS);
        HAL_UART_Receive_IT(&huart6, &host_byte[UART_LINK_BUS], 1);
    }
}
