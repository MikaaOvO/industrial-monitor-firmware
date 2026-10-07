#include "ui.h"
#include "lvgl.h"
#include <stdbool.h>

/* ---------- 中文字体（bpp=2，稳定可用版） ---------- */
extern lv_font_t lv_font_zh_16;
extern lv_font_t lv_font_zh_20;

/* ---------- 前置声明 ---------- */
static void on_fan_click(lv_event_t *e);
static void on_tab(lv_event_t *e);
static void build_tabbar(void);
static void switch_page(lv_obj_t *active);
static lv_obj_t *tab_btns[2];
static lv_obj_t *tab_labels[2];
static lv_obj_t *fan_label = NULL;

/* ================================================================
 * 配色表：改下面的色值，屏幕上对应的地方就会变
 * 每个宏右边标注了它用在屏幕上的什么位置
 * ================================================================ */

/* ---- 全局基础色 ---- */
#define CLR_BG     lv_color_hex(0x000000)
  /* 屏幕整体背景色（ui_init 里设给 lv_scr_act），两页共用 */

#define CLR_CARD   lv_color_hex(0x1C1C1C)
  /* 深灰容器色：①底部 tabbar 背景 ②弹出 toast 的背景 */

#define CLR_INK    lv_color_hex(0xFFFFFF)
  /* 主文字色（白）：标题、磁贴卡上的名称+数值、设置页所有标签文字、
     手动通风按钮文字、toast 文字 */

#define CLR_SUB    lv_color_hex(0xB0B0B0)
  /* 次文字色（浅灰）：底部 tabbar 未选中的"概览"/"设置"文字 */

#define CLR_ACC    lv_color_hex(0x2196F3)
  /* 强调色（蓝）：①设置页数值文字 ②滑块已填充部分 ③开关打开时的滑块
     ④手动通风按钮"关"状态的背景 ⑤tabbar 选中文字+顶部蓝条 */

#define CLR_GOOD   lv_color_hex(0x4CAF50)
  /* 状态色（绿）：①概览页右上角状态灯 ②手动通风按钮"开"状态的背景 */

#define CLR_LINE   lv_color_hex(0x333333)
  /* 分割/轨道色：①设置页滑块的未填充轨道 ②tabbar 顶部那条细分隔线 */

/* ---- 6 张磁贴卡的整卡背景色（data_card 函数里用，每张卡一个色） ---- */
#define C_TEMP    lv_color_hex(0xFF5252)   /* 温度卡（第1行左） */
#define C_HUMI    lv_color_hex(0x2196F3)   /* 湿度卡（第1行右） */
#define C_SMOKE   lv_color_hex(0x26A69A)   /* 烟雾卡（第2行左） */
#define C_GAS     lv_color_hex(0xAB47BC)   /* 有毒气体卡（第2行右） */
#define C_LIGHT   lv_color_hex(0xFF9800)   /* 光照强度卡（第3行左） */
#define C_CO2     lv_color_hex(0x66BB6A)   /* CO2卡（第3行右） */

#define CONTENT_H  280

static lv_obj_t *pg_overview, *pg_setting;

/* ---------- 可滚动容器 ---------- */
static lv_obj_t *make_scroll(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 240, CONTENT_H);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLL_ELASTIC);
    return o;
}

/* ---------- 磁贴卡：整卡填色 + 白字 ---------- */
static lv_obj_t *data_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                           lv_coord_t w, lv_coord_t h,
                           const char *name, const char *value, lv_color_t accent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    /* 整卡填充传感器色 */
    lv_obj_set_style_bg_color(c, accent, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 4, 0);   /* Win10 磁贴是小圆角甚至直角 */
    lv_obj_set_style_pad_all(c, 0, 0);

    /* 名称：左上，白字 */
    lv_obj_t *n = lv_label_create(c);
    lv_label_set_text(n, name);
    lv_obj_set_style_text_color(n, CLR_INK, 0);
    lv_obj_set_pos(n, 12, 10);

    /* 数值：左下，白字大号 */
    lv_obj_t *v = lv_label_create(c);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, CLR_INK, 0);
    lv_obj_set_style_text_font(v, &lv_font_zh_20, 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    return c;
}

/* ---------- toast ---------- */
static void toast_del(lv_timer_t *t)
{
    lv_obj_del(t->user_data);
    lv_timer_del(t);
}
static void toast(const char *txt)
{
    lv_obj_t *o = lv_label_create(lv_scr_act());
    lv_label_set_text(o, txt);
    lv_obj_set_style_bg_color(o, CLR_CARD, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(o, CLR_INK, 0);
    lv_obj_set_style_radius(o, 4, 0);
    lv_obj_set_style_pad_hor(o, 14, 0);
    lv_obj_set_style_pad_ver(o, 8, 0);
    lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, -66);
    lv_timer_create(toast_del, 2000, o);
}

/* ---------- 概览页 ---------- */
static void build_overview(void)
{
    pg_overview = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(pg_overview);
    lv_obj_set_size(pg_overview, 240, CONTENT_H);
    lv_obj_set_pos(pg_overview, 0, 0);

    lv_obj_t *cont = make_scroll(pg_overview);

    /* 标题 */
    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "环境监测");
    lv_obj_set_style_text_color(title, CLR_INK, 0);
    lv_obj_set_style_text_font(title, &lv_font_zh_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 12);

    /* 状态灯 */
    lv_obj_t *led = lv_obj_create(cont);
    lv_obj_remove_style_all(led);
    lv_obj_set_size(led, 12, 12);
    lv_obj_align(led, LV_ALIGN_TOP_RIGHT, -22, 18);
    lv_obj_set_style_bg_color(led, CLR_GOOD, 0);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);

    /* 6 张磁贴：2 列 × 3 行 */
    const lv_coord_t GAP = 8;
    const lv_coord_t W = 107, H = 72;
    const lv_coord_t X0 = 12, X1 = X0 + W + GAP;
    const lv_coord_t Y0 = 48, ROW = H + GAP;
    data_card(cont, X0, Y0,            W, H, "温度",    "26.4°C",  C_TEMP);
    data_card(cont, X1, Y0,            W, H, "湿度",    "62%",     C_HUMI);
    data_card(cont, X0, Y0 + ROW,      W, H, "烟雾",    "0.12",    C_SMOKE);
    data_card(cont, X1, Y0 + ROW,      W, H, "有毒气体", "0.03",   C_GAS);
    data_card(cont, X0, Y0 + ROW * 2,  W, H, "光照强度", "320 lx", C_LIGHT);
    data_card(cont, X1, Y0 + ROW * 2,  W, H, "CO2",     "420 ppm", C_CO2);
}

/* ---------- 设置页 ---------- */
static void on_slider_deg(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    lv_obj_t *lab = lv_event_get_user_data(e);
    char buf[16];
    if (lab == NULL) return;
    lv_snprintf(buf, sizeof(buf), "%ld°C", (long)lv_slider_get_value(sl));
    lv_label_set_text(lab, buf);
}
static void on_slider_plain(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    lv_obj_t *lab = lv_event_get_user_data(e);
    char buf[16];
    if (lab == NULL) return;
    lv_snprintf(buf, sizeof(buf), "%ld", (long)lv_slider_get_value(sl));
    lv_label_set_text(lab, buf);
}
static void on_slider_co2(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    lv_obj_t *lab = lv_event_get_user_data(e);
    char buf[16];
    if (lab == NULL) return;
    lv_snprintf(buf, sizeof(buf), "%ldppm", (long)lv_slider_get_value(sl));
    lv_label_set_text(lab, buf);
}
static void on_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    toast(lv_obj_has_state(sw, LV_STATE_CHECKED) ? "自动通风已开启" : "自动通风已关闭");
}

static lv_obj_t *setting_slider_row(lv_obj_t *parent, lv_coord_t y,
                                    const char *name, const char *value,
                                    int32_t min, int32_t val, int32_t max,
                                    lv_event_cb_t cb, lv_obj_t **val_out)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 216, 46);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, name);
    lv_obj_set_style_text_color(t, CLR_INK, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_t *v = lv_label_create(r);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, CLR_ACC, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -2, 0);
    *val_out = v;

    lv_obj_t *s = lv_slider_create(r);
    lv_obj_set_size(s, 216, 14);
    lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, CLR_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, CLR_ACC, LV_PART_INDICATOR);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, v);
    return r;
}

static void build_setting(void)
{
    pg_setting = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(pg_setting);
    lv_obj_set_size(pg_setting, 240, CONTENT_H);
    lv_obj_set_pos(pg_setting, 0, 0);
    lv_obj_add_flag(pg_setting, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *cont = make_scroll(pg_setting);

    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_color(title, CLR_INK, 0);
    lv_obj_set_style_text_font(title, &lv_font_zh_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 12);

    lv_obj_t *v1 = NULL, *v2 = NULL, *v3 = NULL;
    setting_slider_row(cont, 44,  "温度报警阈值", "28°C",    20,  28,   40,   on_slider_deg,   &v1);
    setting_slider_row(cont, 100, "烟雾报警阈值", "15",      0,   15,   50,   on_slider_plain, &v2);
    setting_slider_row(cont, 156, "CO2报警阈值",  "1000ppm", 400, 1000, 2000, on_slider_co2,   &v3);

    /* 自动通风开关 */
    lv_obj_t *r3 = lv_obj_create(cont);
    lv_obj_remove_style_all(r3);
    lv_obj_set_size(r3, 216, 36);
    lv_obj_align(r3, LV_ALIGN_TOP_MID, 0, 214);
    lv_obj_t *t3 = lv_label_create(r3);
    lv_label_set_text(t3, "自动通风");
    lv_obj_set_style_text_color(t3, CLR_INK, 0);
    lv_obj_align(t3, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_t *sw1 = lv_switch_create(r3);
    lv_obj_align(sw1, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(sw1, CLR_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw1, CLR_ACC, LV_STATE_CHECKED | LV_PART_INDICATOR);
    lv_obj_add_event_cb(sw1, on_switch, LV_EVENT_VALUE_CHANGED, NULL);

    /* 报警提示音开关 */
    lv_obj_t *r4 = lv_obj_create(cont);
    lv_obj_remove_style_all(r4);
    lv_obj_set_size(r4, 216, 36);
    lv_obj_align(r4, LV_ALIGN_TOP_MID, 0, 258);
    lv_obj_t *t4 = lv_label_create(r4);
    lv_label_set_text(t4, "报警提示音");
    lv_obj_set_style_text_color(t4, CLR_INK, 0);
    lv_obj_align(t4, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_t *sw2 = lv_switch_create(r4);
    lv_obj_add_state(sw2, LV_STATE_CHECKED);
    lv_obj_align(sw2, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(sw2, CLR_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw2, CLR_ACC, LV_STATE_CHECKED | LV_PART_INDICATOR);

    /* 手动通风按钮 */
    lv_obj_t *btn_fan = lv_btn_create(cont);
    lv_obj_set_size(btn_fan, 216, 38);
    lv_obj_align(btn_fan, LV_ALIGN_TOP_MID, 0, 306);
    lv_obj_set_style_radius(btn_fan, 4, 0);
    lv_obj_set_style_bg_color(btn_fan, CLR_ACC, 0);
    lv_obj_set_style_bg_opa(btn_fan, LV_OPA_COVER, 0);
    lv_obj_t *lb = lv_label_create(btn_fan);
    lv_label_set_text(lb, "手动通风-关");
    lv_obj_set_style_text_color(lb, CLR_INK, 0);
    fan_label = lb;
    lv_obj_add_event_cb(btn_fan, on_fan_click, LV_EVENT_CLICKED, NULL);
}

/* ---------- 事件 ---------- */
static void on_fan_click(lv_event_t *e)
{
    static bool on = false;
    on = !on;
    lv_obj_t *btn = lv_event_get_target(e);
    if (on) {
        lv_label_set_text(fan_label, "手动通风-开");
        lv_obj_set_style_bg_color(btn, CLR_GOOD, 0);
        toast("通风已开启");
    } else {
        lv_label_set_text(fan_label, "手动通风-关");
        lv_obj_set_style_bg_color(btn, CLR_ACC, 0);
        toast("通风已关闭");
    }
}
static void switch_page(lv_obj_t *active)
{
    lv_obj_add_flag(pg_overview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pg_setting,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(active, LV_OBJ_FLAG_HIDDEN);
}
static void on_tab(lv_event_t *e)
{
    lv_obj_t *tab = lv_event_get_target(e);
    switch_page((lv_obj_t *)lv_event_get_user_data(e));
    for (int i = 0; i < 2; i++) {
        lv_obj_clear_state(tab_btns[i], LV_STATE_CHECKED);
        lv_obj_set_style_text_color(tab_labels[i], CLR_SUB, 0);
    }
    lv_obj_add_state(tab, LV_STATE_CHECKED);
    for (int i = 0; i < 2; i++) {
        if (tab_btns[i] == tab)
            lv_obj_set_style_text_color(tab_labels[i], CLR_ACC, 0);
    }
}

/* ---------- 底部 tabbar ---------- */
static void build_tabbar(void)
{
    lv_obj_t *bar = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, 40);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, CLR_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, CLR_LINE, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(bar, LV_DIR_NONE);

    const char *names[2] = {"概览", "设置"};
    for (int i = 0; i < 2; i++) {
        tab_btns[i] = lv_btn_create(bar);
        lv_obj_remove_style_all(tab_btns[i]);
        lv_obj_set_size(tab_btns[i], 120, 40);
        lv_obj_set_pos(tab_btns[i], i * 120, 0);
        lv_obj_set_style_radius(tab_btns[i], 0, 0);
        lv_obj_t *lb = lv_label_create(tab_btns[i]);
        lv_label_set_text(lb, names[i]);
        lv_obj_set_style_text_color(lb, CLR_SUB, 0);
        tab_labels[i] = lb;
        lv_obj_set_style_border_width(tab_btns[i], 3, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(tab_btns[i], LV_BORDER_SIDE_TOP, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(tab_btns[i], CLR_ACC, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(tab_btns[i], lv_color_hex(0x3A3A3A), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(tab_btns[i], LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_add_event_cb(tab_btns[i], on_tab, LV_EVENT_CLICKED,
                            i == 0 ? (void *)pg_overview : (void *)pg_setting);
    }
    lv_obj_add_state(tab_btns[0], LV_STATE_CHECKED);
    lv_obj_set_style_text_color(tab_labels[0], CLR_ACC, 0);
}

/* ---------- 入口 ---------- */
void ui_init(void)
{
    lv_obj_set_style_bg_color(lv_scr_act(), CLR_BG, 0);
    lv_obj_set_style_text_font(lv_scr_act(), &lv_font_zh_16, 0);
    build_overview();
    build_setting();
    build_tabbar();
    switch_page(pg_overview);
}
