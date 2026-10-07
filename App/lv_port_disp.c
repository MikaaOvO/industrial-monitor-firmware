#include "lv_port_disp.h"
#include "ili9341.h"

// 缓冲行数
#define BUF_LINES 40

#define USE_DOUBLE_BUFFER 1

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;

#if USE_DOUBLE_BUFFER
static lv_color_t buf1[ILI9341_WIDTH * BUF_LINES];
static lv_color_t buf2[ILI9341_WIDTH * BUF_LINES];
#else
static lv_color_t buf[ILI9341_WIDTH * BUF_LINES];
#endif

/* SPI1 DMA 发送完成中断：DMA 一传完立刻通知 LVGL */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1) {
        CS_H();
        lv_disp_flush_ready(&disp_drv);
    }
}

static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    ILI9341_SetWindow(area->x1, area->y1, area->x2, area->y2);
    uint32_t pixel_cnt = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    ILI9341_WritePixels((uint16_t *)color_p, pixel_cnt);
    /* 不调 flush_ready，等 DMA 中断里调 */
}

void lv_port_disp_init(void)
{
#if USE_DOUBLE_BUFFER
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, ILI9341_WIDTH * BUF_LINES);
#else
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, ILI9341_WIDTH * BUF_LINES);
#endif

    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.hor_res = ILI9341_WIDTH;
    disp_drv.ver_res = ILI9341_HEIGHT;
    lv_disp_drv_register(&disp_drv);
}
