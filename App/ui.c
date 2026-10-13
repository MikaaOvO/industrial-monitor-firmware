#include "ui.h"
#include "app_env.h"
#include "lvgl.h"
#include <stdbool.h>

/* ---------- 中文字体（bpp=2，稳定可用版） ---------- */
extern lv_font_t lv_font_zh_16;
extern lv_font_t lv_font_zh_20;

/* ================================================================
 * 线程安全约定（很重要）
 *
 * LVGL 不是线程安全的，本文件里所有 lv_* 调用都只能发生在 LVGL 任务
 * 的上下文里。所以这里的做法是：界面自己去"拉"数据，而不是让别的
 * 任务"推"进来 ——
 *
 *   ui_refresh_cb() 是一个 200ms 的 LVGL 定时器，跑在 lv_timer_handler
 *   里面，也就是在 LVGL 任务里。它主动调 env_get()/cfg_get() 取快照。
 *
 * 这样 SensorTask / VocTask / HostTask 根本不需要、也不允许碰 LVGL。
 *
 * 反面例子：不要在别的任务里直接调 lv_label_set_text()，会花屏或死机。
 * ================================================================ */

/* ================================================================
 * 【临时调试开关】测 21VOC 的上报间隔 —— 排完设回 0 就恢复原样
 *
 * 打开后，"CO2"卡改显示通过校验的帧数（温度和湿度卡照常显示真值）。
 *
 * 用法：记下 CO2 卡当前的数，等 60 秒，再看一次。涨幅 N 就说明平均
 * 间隔约 60/N 秒。然后把 voc.h 的 VOC_TIMEOUT_MS 定成间隔的 3 倍以上。
 *
 * 顺便也能判断超时改对了没：
 *   温度/湿度卡连续显示真值     → 超时够长，正常
 *   真值和横杠之间反复闪        → 间隔比 VOC_TIMEOUT_MS 还长，继续加大
 * ================================================================ */
#define VOC_DEBUG_UI   0

/* ---------- 前置声明 ---------- */
static void on_fan_click(lv_event_t *e);
static void on_tab(lv_event_t *e);
static void build_tabbar(void);
static void switch_page(lv_obj_t *active);
static void ui_refresh_cb(lv_timer_t *t);
static void ui_sync_cfg(const cfg_t *c);
static lv_obj_t *tab_btns[2];
static lv_obj_t *tab_labels[2];
static lv_obj_t *fan_label = NULL;
/* 手动通风按钮当前显示的状态。用变量记住，别去比较 label 的文字 ——
   那种做法一改文案就失效。 */
static bool fan_btn_on = false;

/* ---------- 概览页：会随数据变化的控件 ---------- */
static lv_obj_t *v_temp, *v_humi, *v_smoke, *v_gas, *v_light, *v_co2;  /* 磁贴数值 */
static lv_obj_t *led_status;                                            /* 状态灯   */
static lv_obj_t *sw_fan;                                                /* 自动通风 */

/* ================================================================
 * 设置页的 6 个阈值滑块
 *
 * 6 个滑块的逻辑完全一样（改一个数值、格式化一个 label、写回 cfg 的
 * 某个字段），只是字段和格式不同，所以用一张表驱动，别写 6 个近似
 * 的回调函数。
 * ================================================================ */
enum {
    CTL_TEMP = 0,   /* 温度报警阈值   */
    CTL_HUMI,       /* 湿度报警阈值   */
    CTL_SMOKE,      /* 烟雾报警阈值   */
    CTL_GAS,        /* 有毒气体报警阈值 */
    CTL_LIGHT,      /* 开灯阈值（方向相反） */
    CTL_ECO2,       /* CO2 报警阈值   */
    CTL_COUNT
};

typedef struct {
    lv_obj_t   *slider;
    lv_obj_t   *label;
    const char *fmt;                /* 数值 label 的显示格式 */
    void      (*apply)(int32_t);    /* 写回 cfg 的哪个字段   */
} ctl_t;

static ctl_t ctl[CTL_COUNT];

/* 界面刷新周期。200ms 够用：人眼看不出跟不上的感觉，
   比 5ms 的 LVGL 主循环省很多重绘。 */
#define UI_REFRESH_MS  200

/* ================================================================
 * 配色表：改下面的色值，屏幕上对应的地方就会变
 * 每个宏右边标注了它用在屏幕上的什么位置
 * ================================================================ */

/* ---- 全局基础色 ---- */
#define CLR_BG     lv_color_hex(0xFFFFFF)
  /* 屏幕整体背景色（ui_init 里设给 lv_scr_act），两页共用 */

#define CLR_CARD   lv_color_hex(0xC0C0C0)
  /* 深灰容器色：①底部 tabbar 背景 ②弹出 toast 的背景 */

#define CLR_INK    lv_color_hex(0x000000)
  /* 主文字色：标题、磁贴卡上的名称+数值、设置页所有标签文字、
     手动通风按钮文字、toast 文字 */

#define CLR_SUB    lv_color_hex(0x000000)
  /* 次文字色（浅灰）：底部 tabbar 未选中的"概览"/"设置"文字 */

#define CLR_ACC    lv_color_hex(0x3498DB)
  /* 强调色（蓝）：①设置页数值文字 ②滑块已填充部分 ③开关打开时的滑块
     ④手动通风按钮"关"状态的背景 ⑤tabbar 选中文字+顶部蓝条 */

#define CLR_GOOD   lv_color_hex(0x45DB34)
  /* 状态色（绿）：①概览页右上角状态灯（正常）②手动通风按钮"开"状态的背景 */
#define CLR_WARN   lv_color_hex(0xF1C40F)
  /* 状态色（黄）：概览页状态灯"警告"（接近阈值但还没超） */
#define CLR_BAD    lv_color_hex(0xE74C3C)
  /* 状态色（红）：概览页状态灯"报警"（有指标超阈值） */
#define CLR_FAN_OFF lv_color_hex(0xDB346E)
  /* 手动通风按钮"关"状态的背景 */

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

/* ---------- 磁贴卡：整卡填色 + 白字 ----------
 * val_out 把"数值"那个 label 的句柄递出来，后面刷新要用。
 * （原版这里丢掉了句柄，所以数值没法动态更新） */
static lv_obj_t *data_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                           lv_coord_t w, lv_coord_t h,
                           const char *name, const char *value, lv_color_t accent,
                           lv_obj_t **val_out)
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
    if (val_out) *val_out = v;
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
    led_status = lv_obj_create(cont);
    lv_obj_remove_style_all(led_status);
    lv_obj_set_size(led_status, 12, 12);
    lv_obj_align(led_status, LV_ALIGN_TOP_RIGHT, -22, 18);
    lv_obj_set_style_bg_color(led_status, CLR_GOOD, 0);
    lv_obj_set_style_bg_opa(led_status, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(led_status, LV_RADIUS_CIRCLE, 0);

    /* 6 张磁贴：2 列 × 3 行，顺序和设置页的滑块一一对应。
       初始文字给的是占位符，别放假数据 —— 真值在 200ms 后由
       ui_refresh_cb 填进来。 */
    const lv_coord_t GAP = 8;
    const lv_coord_t W = 107, H = 72;
    const lv_coord_t X0 = 12, X1 = X0 + W + GAP;
    const lv_coord_t Y0 = 48, ROW = H + GAP;
    data_card(cont, X0, Y0,            W, H, "温度",    "---.-°C",  C_TEMP,  &v_temp);
    data_card(cont, X1, Y0,            W, H, "湿度",    "--%",      C_HUMI,  &v_humi);
    data_card(cont, X0, Y0 + ROW,      W, H, "烟雾",    "--",       C_SMOKE, &v_smoke);
    data_card(cont, X1, Y0 + ROW,      W, H, "有毒气体", "--",      C_GAS,   &v_gas);
    data_card(cont, X0, Y0 + ROW * 2,  W, H, "光照强度", "---",      C_LIGHT, &v_light);
    data_card(cont, X1, Y0 + ROW * 2,  W, H, "CO2",     "---- ppm", C_CO2,   &v_co2);
}

/* ================================================================
 * 阈值回写：界面拖滑块 -> 更新共享 cfg
 * 上位机改阈值走的也是同一份 cfg，两边不会各说各话。
 * ================================================================ */
static void apply_temp (int32_t v) { cfg_t c; cfg_get(&c); c.temp_hi  = (float)v;    cfg_set(&c); }
static void apply_humi (int32_t v) { cfg_t c; cfg_get(&c); c.humi_hi  = (float)v;    cfg_set(&c); }
static void apply_smoke(int32_t v) { cfg_t c; cfg_get(&c); c.smoke_hi = (float)v;    cfg_set(&c); }
static void apply_gas  (int32_t v) { cfg_t c; cfg_get(&c); c.gas_hi   = (float)v;    cfg_set(&c); }
static void apply_light(int32_t v) { cfg_t c; cfg_get(&c); c.light_lo = (float)v;    cfg_set(&c); }
static void apply_eco2 (int32_t v) { cfg_t c; cfg_get(&c); c.eco2_hi  = (uint16_t)v; cfg_set(&c); }

/* 6 个滑块共用的回调，靠 user_data 里的 ctl_t 区分是哪一个 */
static void on_slider(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    ctl_t *cc = (ctl_t *)lv_event_get_user_data(e);
    if (cc == NULL || cc->label == NULL) return;

    int32_t v = lv_slider_get_value(sl);
    lv_label_set_text_fmt(cc->label, cc->fmt, (long)v);
    if (cc->apply) cc->apply(v);
}

/* 自动通风开关 */
static void on_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    cfg_t c;
    cfg_get(&c);
    c.auto_fan = on;
    cfg_set(&c);
    toast(on ? "自动通风已开启" : "自动通风已关闭");
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

    /* ---- 6 个阈值滑块 ----
       顺序刻意和概览页的磁贴一致，方便对照。
       初始值必须和 app_env.c 里的 DEF_* 保持一致 —— ui_init() 跑在
       调度器启动之前，那时还没法取锁回读 cfg，只能靠两边默认值对齐。

       第 3 列（value）只是开机时显示的占位文字，200ms 后会被
       ui_sync_cfg 用 cfg 的真值覆盖。 */
    struct {
        int         idx;
        const char *name;
        const char *fmt;
        const char *value;
        int32_t     min, def, max;
        void      (*apply)(int32_t);
    } rows[CTL_COUNT] = {
        /* 滑块的上下限取自 app_env.h 的 CFG_*_MIN/MAX —— 那是唯一的一处
           定义，上位机下发阈值时的钳制也用同一份（见 docs 里的 P-12：
           同一个范围在两处各写一遍，改一边不生效且只有警告）。
           中间的数是开机默认值，必须和 app_env.c 的 DEF_* 一致。 */
        { CTL_TEMP,  "温度报警阈值",     "%ld°C",  "28°C",
          (int32_t)CFG_TEMP_MIN,   28,   (int32_t)CFG_TEMP_MAX,  apply_temp  },
        { CTL_HUMI,  "湿度报警阈值",     "%ld%%",  "70%",
          (int32_t)CFG_HUMI_MIN,   70,   (int32_t)CFG_HUMI_MAX,  apply_humi  },
        { CTL_SMOKE, "烟雾报警阈值",     "%ld",    "15",
          (int32_t)CFG_SMOKE_MIN,  15,   (int32_t)CFG_SMOKE_MAX, apply_smoke },
        { CTL_GAS,   "有毒气体报警阈值", "%ld",    "15",
          (int32_t)CFG_GAS_MIN,    15,   (int32_t)CFG_GAS_MAX,   apply_gas   },
        { CTL_LIGHT, "开灯阈值",         "%ld",    "30",
          (int32_t)CFG_LIGHT_MIN,  30,   (int32_t)CFG_LIGHT_MAX, apply_light },
        { CTL_ECO2,  "CO2报警阈值",      "%ldppm", "1000ppm",
          (int32_t)CFG_ECO2_MIN,  1000,  (int32_t)CFG_ECO2_MAX,  apply_eco2  },
    };

    const lv_coord_t Y0 = 44, PITCH = 52;

    for (int i = 0; i < CTL_COUNT; i++) {
        ctl_t *cc = &ctl[rows[i].idx];

        lv_obj_t *r = lv_obj_create(cont);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, 216, 46);
        lv_obj_align(r, LV_ALIGN_TOP_MID, 0, Y0 + PITCH * i);

        lv_obj_t *t = lv_label_create(r);
        lv_label_set_text(t, rows[i].name);
        lv_obj_set_style_text_color(t, CLR_INK, 0);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 2, 0);

        cc->label = lv_label_create(r);
        lv_label_set_text(cc->label, rows[i].value);
        lv_obj_set_style_text_color(cc->label, CLR_ACC, 0);
        lv_obj_align(cc->label, LV_ALIGN_TOP_RIGHT, -2, 0);

        cc->fmt   = rows[i].fmt;
        cc->apply = rows[i].apply;

        cc->slider = lv_slider_create(r);
        lv_obj_set_size(cc->slider, 216, 14);
        lv_obj_align(cc->slider, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_slider_set_range(cc->slider, rows[i].min, rows[i].max);
        lv_slider_set_value(cc->slider, rows[i].def, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(cc->slider, CLR_LINE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(cc->slider, CLR_ACC, LV_PART_INDICATOR);
        lv_obj_add_event_cb(cc->slider, on_slider, LV_EVENT_VALUE_CHANGED, cc);
    }

    /* ---- 自动通风开关 ----
       （原设计还有个"报警提示音"开关，但台账里没有任何蜂鸣器/喇叭，
         报警指示只有红/黄/绿 LED，那个开关控制不了硬件，已去掉。） */
    lv_obj_t *rs = lv_obj_create(cont);
    lv_obj_remove_style_all(rs);
    lv_obj_set_size(rs, 216, 36);
    lv_obj_align(rs, LV_ALIGN_TOP_MID, 0, Y0 + PITCH * CTL_COUNT + 6);
    lv_obj_t *ts = lv_label_create(rs);
    lv_label_set_text(ts, "自动通风");
    lv_obj_set_style_text_color(ts, CLR_INK, 0);
    lv_obj_align(ts, LV_ALIGN_LEFT_MID, 2, 0);
    sw_fan = lv_switch_create(rs);
    lv_obj_align(sw_fan, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(sw_fan, CLR_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw_fan, CLR_ACC, LV_STATE_CHECKED | LV_PART_INDICATOR);
    lv_obj_add_event_cb(sw_fan, on_switch, LV_EVENT_VALUE_CHANGED, NULL);

    /* ---- 手动通风按钮 ---- */
    lv_obj_t *btn_fan = lv_btn_create(cont);
    lv_obj_remove_style_all(btn_fan);   /* 清除主题默认样式，否则按下时主题PRESSED色会覆盖 */
    lv_obj_set_size(btn_fan, 216, 38);
    lv_obj_align(btn_fan, LV_ALIGN_TOP_MID, 0, Y0 + PITCH * CTL_COUNT + 50);
    lv_obj_set_style_radius(btn_fan, 4, 0);
    lv_obj_set_style_bg_color(btn_fan, CLR_FAN_OFF, 0);
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
    lv_obj_t *btn = lv_event_get_target(e);
    cfg_t c;
    cfg_get(&c);
    c.manual_fan = !c.manual_fan;
    cfg_set(&c);

    fan_btn_on = c.manual_fan;
    if (c.manual_fan) {
        lv_label_set_text(fan_label, "手动通风-开");
        lv_obj_set_style_bg_color(btn, CLR_GOOD, 0);
        toast("通风已开启");
    } else {
        lv_label_set_text(fan_label, "手动通风-关");
        lv_obj_set_style_bg_color(btn, CLR_FAN_OFF, 0);
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
        lv_obj_set_style_bg_color(tab_btns[i], lv_color_hex(0x969696), LV_STATE_CHECKED);  /* 选中背景深灰 */
        lv_obj_set_style_bg_opa(tab_btns[i], LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_add_event_cb(tab_btns[i], on_tab, LV_EVENT_CLICKED,
                            i == 0 ? (void *)pg_overview : (void *)pg_setting);
    }
    lv_obj_add_state(tab_btns[0], LV_STATE_CHECKED);
    lv_obj_set_style_text_color(tab_labels[0], CLR_ACC, 0);
}

/* ================================================================
 * 定时刷新（跑在 LVGL 任务里）
 * ================================================================ */
static void ui_refresh_cb(lv_timer_t *t)
{
    (void)t;
    env_t e;
    cfg_t c;
    env_get(&e);
    cfg_get(&c);

    /* ---- 三张 21VOC 的卡：模块掉线就别显示陈旧值，会误导 ---- */
    if (e.voc_online) {
        lv_label_set_text_fmt(v_temp, "%.1f°C", e.temp_c);
        lv_label_set_text_fmt(v_humi, "%.0f%%",  e.humi_rh);
        lv_label_set_text_fmt(v_co2,  "%u ppm",  (unsigned)e.eco2_ppm);
    } else {
        lv_label_set_text(v_temp, "---.-°C");
        lv_label_set_text(v_humi, "--%");
        lv_label_set_text(v_co2,  "---- ppm");
    }

#if VOC_DEBUG_UI
    /* 【临时调试】只顶掉 CO2 这一张，改成显示"通过校验的帧数"，用来测
       模块的上报间隔。温度和湿度卡照常显示真值 —— 这样一次烧录能同时
       回答两个问题：超时改对了没（看温度/湿度连不连续）、间隔是多少
       （看这个帧数的涨幅）。排完把 VOC_DEBUG_UI 设回 0。 */
    lv_label_set_text_fmt(v_co2, "%lu", (unsigned long)e.voc_frames_ok);
#endif

    /* ---- 三张 ADC 的卡：显示 0~100 相对指数 ---- */
    lv_label_set_text_fmt(v_smoke, "%.0f", e.smoke_idx);
    lv_label_set_text_fmt(v_gas,   "%.0f", e.co_idx);
    lv_label_set_text_fmt(v_light, "%.0f", e.light_idx);

    /* ---- 状态灯：绿=正常 黄=接近阈值 红=超阈值 ---- */
    alarm_t a = env_alarm_level(&e, &c);
    lv_color_t col = (a == ALARM_ALERT) ? CLR_BAD
                   : (a == ALARM_WARN)  ? CLR_WARN
                                        : CLR_GOOD;
    lv_obj_set_style_bg_color(led_status, col, 0);

    /* ---- 把配置同步回控件（上位机改了阈值这里要跟着动） ---- */
    ui_sync_cfg(&c);
}

/* 拖动中的滑块不要被刷新的值改写，否则手感会打架 */
static bool slider_busy(lv_obj_t *sl)
{
    return lv_obj_has_state(sl, LV_STATE_PRESSED);
}

static void sync_one(int idx, int32_t val)
{
    ctl_t *cc = &ctl[idx];
    if (cc->slider == NULL || cc->label == NULL) return;
    if (slider_busy(cc->slider))                             return;  /* 用户正在拖 */
    if (lv_slider_get_value(cc->slider) == val)              return;  /* 没变就别动 */
    lv_slider_set_value(cc->slider, val, LV_ANIM_OFF);
    lv_label_set_text_fmt(cc->label, cc->fmt, (long)val);
}

static void sync_switch(lv_obj_t *sw, bool on)
{
    if (sw == NULL) return;
    if (lv_obj_has_state(sw, LV_STATE_CHECKED) == on) return;
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_clear_state(sw, LV_STATE_CHECKED);
}

static void ui_sync_cfg(const cfg_t *c)
{
    /* 四舍五入到整数，和滑块的整数刻度对齐 */
    sync_one(CTL_TEMP,  (int32_t)(c->temp_hi  + 0.5f));
    sync_one(CTL_HUMI,  (int32_t)(c->humi_hi  + 0.5f));
    sync_one(CTL_SMOKE, (int32_t)(c->smoke_hi + 0.5f));
    sync_one(CTL_GAS,   (int32_t)(c->gas_hi   + 0.5f));
    sync_one(CTL_LIGHT, (int32_t)(c->light_lo + 0.5f));
    sync_one(CTL_ECO2,  (int32_t)c->eco2_hi);

    sync_switch(sw_fan, c->auto_fan);

    /* 手动通风按钮：状态只有 cfg.manual_fan 一个来源。
       这样上位机也能改这个开关，界面会跟着变。 */
    if (fan_label != NULL && fan_btn_on != c->manual_fan) {
        fan_btn_on = c->manual_fan;
        lv_label_set_text(fan_label, fan_btn_on ? "手动通风-开" : "手动通风-关");
        lv_obj_t *btn = lv_obj_get_parent(fan_label);
        lv_obj_set_style_bg_color(btn, fan_btn_on ? CLR_GOOD : CLR_FAN_OFF, 0);
    }
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

    /* 数据刷新定时器。它跑在 lv_timer_handler 里，也就是 LVGL 任务
       的上下文，所以里面可以放心调 lv_* —— 这正是选这个方案的
       原因：其他任务永远不需要碰 LVGL。 */
    lv_timer_create(ui_refresh_cb, UI_REFRESH_MS, NULL);
}
