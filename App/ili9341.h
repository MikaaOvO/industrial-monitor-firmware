#ifndef ILI9341_H
#define ILI9341_H
#include "stm32f4xx_hal.h"

#define ILI9341_WIDTH  240
#define ILI9341_HEIGHT 320

/* 引脚映射 */
#define DC_PORT     GPIOB
#define DC_PIN      GPIO_PIN_0
#define CS_PORT     GPIOB
#define CS_PIN      GPIO_PIN_1
#define RES_PORT    GPIOB
#define RES_PIN     GPIO_PIN_2
#define BL_PORT     GPIOB
#define BL_PIN      GPIO_PIN_10

/* 快捷操作 */
#define DC_H()  HAL_GPIO_WritePin(DC_PORT, DC_PIN, GPIO_PIN_SET)
#define DC_L()  HAL_GPIO_WritePin(DC_PORT, DC_PIN, GPIO_PIN_RESET)
#define CS_H()  HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET)
#define CS_L()  HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_RESET)
#define RES_H() HAL_GPIO_WritePin(RES_PORT, RES_PIN, GPIO_PIN_SET)
#define RES_L() HAL_GPIO_WritePin(RES_PORT, RES_PIN, GPIO_PIN_RESET)
#define BL_ON() HAL_GPIO_WritePin(BL_PORT, BL_PIN, GPIO_PIN_SET)
#define BL_OFF() HAL_GPIO_WritePin(BL_PORT, BL_PIN, GPIO_PIN_RESET)

void ILI9341_Init(void);
void ILI9341_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void ILI9341_WritePixels(uint16_t *color_buf, uint32_t len);

#endif
