#include "xpt2046.h"

/* hspi2 定义在 main.c，这里声明外部引用 */
extern SPI_HandleTypeDef hspi2;

/* XPT2046 命令字（差分模式，12 位 ADC） */
#define CMD_READ_X   0xD0
#define CMD_READ_Y   0x90

#define T_CS_LOW()   HAL_GPIO_WritePin(T_CS_GPIO_Port, T_CS_Pin, GPIO_PIN_RESET)
#define T_CS_HIGH()  HAL_GPIO_WritePin(T_CS_GPIO_Port, T_CS_Pin, GPIO_PIN_SET)

/* 读单个通道的原始 ADC 值 */
static uint16_t XPT2046_ReadChannel(uint8_t cmd)
{
    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0};

    T_CS_LOW();
    HAL_SPI_TransmitReceive(&hspi2, tx, rx, 3, 100);   /* 注意是 hspi2 */
    T_CS_HIGH();

    /* 12 位结果：rx[1] 的高 7 位 + rx[2] 的低 5 位 */
    return ((uint16_t)rx[1] << 5) | (rx[2] >> 3);
}

/* 是否按下（T_IRQ 低电平 = 按下） */
uint8_t XPT2046_IsPressed(void)
{
    return (HAL_GPIO_ReadPin(T_IRQ_GPIO_Port, T_IRQ_Pin) == GPIO_PIN_RESET);
}

/* 读原始坐标（5 次采样取平均，去抖动） */
uint8_t XPT2046_ReadRaw(uint16_t *x, uint16_t *y)
{
    if (!XPT2046_IsPressed()) return 0;

    uint32_t sum_x = 0, sum_y = 0;
    for (int i = 0; i < 5; i++) {
        sum_x += XPT2046_ReadChannel(CMD_READ_X);
        sum_y += XPT2046_ReadChannel(CMD_READ_Y);
    }
    *x = (uint16_t)(sum_x / 5);
    *y = (uint16_t)(sum_y / 5);
    return 1;
}

/* 转换成屏幕坐标（线性映射 + 钳位） */
uint8_t XPT2046_GetPoint(uint16_t *x, uint16_t *y)
{
    uint16_t raw_x, raw_y;
    if (!XPT2046_ReadRaw(&raw_x, &raw_y)) return 0;

    /* 关键：XPT2046 的 X 通道对应屏幕 Y 轴，Y 通道对应屏幕 X 轴 */
    *y = (uint16_t)((int32_t)(raw_x - XPT2046_X_MIN) * LCD_HEIGHT / (XPT2046_X_MAX - XPT2046_X_MIN));
    *x = (uint16_t)((int32_t)(raw_y - XPT2046_Y_MIN) * LCD_WIDTH  / (XPT2046_Y_MAX - XPT2046_Y_MIN));

    /* 钳位 */
    if (*x >= LCD_WIDTH)  *x = LCD_WIDTH  - 1;
    if (*y >= LCD_HEIGHT) *y = LCD_HEIGHT - 1;
    return 1;
}

