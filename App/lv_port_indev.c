#include "lvgl.h"
#include "xpt2046.h"

static lv_indev_t *indev_touch = NULL;

/* LVGL 触控读取回调 */
static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    uint16_t x, y;
    if (XPT2046_GetPoint(&x, &y)) {
        data->state   = LV_INDEV_STATE_PR;   /* 按下 */
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_REL;    /* 松开 */
    }
}

/* 注册输入设备（在 lv_init() 之后、ui_init() 之前调用） */
void lv_port_indev_init(void)
{
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;   /* 指针类设备 = 触摸屏 */
    indev_drv.read_cb = touchpad_read;
    indev_touch = lv_indev_drv_register(&indev_drv);
}
