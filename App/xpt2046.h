#ifndef __XPT2046_H
#define __XPT2046_H

#include "main.h"

/* ===== 校准参数（第一次用需要实测替换，见文末说明） ===== */
#define XPT2046_X_MIN   300
#define XPT2046_X_MAX   3900
#define XPT2046_Y_MIN   300
#define XPT2046_Y_MAX   3900

#define LCD_WIDTH    240
#define LCD_HEIGHT   320

uint8_t  XPT2046_IsPressed(void);
uint8_t  XPT2046_ReadRaw(uint16_t *x, uint16_t *y);
uint8_t  XPT2046_GetPoint(uint16_t *x, uint16_t *y);

#endif
