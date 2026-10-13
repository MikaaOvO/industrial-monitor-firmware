#include "host_proto.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 * 上位机协议实现
 *
 * 全程整数运算，不碰浮点 —— 原因见 host_proto.h 末尾的说明。
 * 温度湿度用"放大 10 倍的整数"(x10) 在内部传递，只在最后拼字符串时
 * 才拆成"整数部分.小数部分"。
 *
 * 两条链路（UART_LINK_DEBUG / UART_LINK_BUS）各有独立的一份接收状态，
 * 见下面的 host_link_t。
 * ================================================================ */

/* ---------------- 校验和 ---------------- */

/* '$' 和 '*' 之间所有字符的逐字节异或。
 *
 * 说明：异或校验能检出任意单字节错误，但检不出"两个字节互换位置"和
 * "同一字节重复偶数次" —— 协议文档里举过一个真实例子：
 * `$FAN,1,AUTO=1,MAN=0` 和 `$FAN,1,AUTO=0,MAN=1` 的校验和是同一个。
 * 对 115200 的点对点短接线够用；将来切 RS485 多节点、线更长的时候，
 * 建议换成 CRC16。 */
static uint8_t xor_bytes(const char *s, int n)
{
    uint8_t x = 0;
    for (int i = 0; i < n; i++) {
        x ^= (uint8_t)s[i];
    }
    return x;
}

/* ---------------- 定点小数的格式化与解析 ---------------- */

/* 把放大 10 倍的整数格式化成 "-10.5" 这种形式。
 * 自己拆而不用 printf 的 %f —— newlib-nano 不支持浮点格式化。 */
static void fmt_x10(char *out, int out_len, int32_t v10)
{
    int32_t whole = v10 / 10;
    int32_t frac  = v10 % 10;
    if (frac < 0) frac = -frac;      /* v10 为负时余数也是负的，取绝对值 */
    snprintf(out, out_len, "%ld.%ld", (long)whole, (long)frac);
}

/* 把浮点读数转成放大 10 倍的整数，顺便做四舍五入。 */
static int32_t to_x10(float v)
{
    return (int32_t)(v * 10.0f + (v >= 0.0f ? 0.5f : -0.5f));
}

/* 解析一个十进制小数，存成放大 10 倍的整数。
 * 自己解析而不用 strtof：少一层对库的浮点支持的依赖。
 * 只保留一位小数，多余的位数直接丢掉。
 * 格式不对（非法字符、超长、溢出）返回 false。 */
static bool parse_x10(const char *s, int32_t *out)
{
    if (s == NULL || out == NULL || *s == '\0') return false;

    bool neg = false;
    if (*s == '-')      { neg = true;  s++; }
    else if (*s == '+') { s++; }

    if (*s < '0' || *s > '9') return false;      /* 必须至少有整数位 */

    int32_t whole = 0;
    while (*s >= '0' && *s <= '9') {
        whole = whole * 10 + (*s - '0');
        if (whole > 1000000) return false;       /* 明显越界，防止溢出 */
        s++;
    }

    int32_t frac = 0;
    if (*s == '.') {
        s++;
        if (*s >= '0' && *s <= '9') {
            frac = *s - '0';
            s++;
        }
        while (*s >= '0' && *s <= '9') s++;      /* 多余的小数位丢掉 */
    }

    if (*s != '\0') return false;                /* 后面还有垃圾字符 */

    int32_t v = whole * 10 + frac;
    *out = neg ? -v : v;
    return true;
}

/* ---------------- 字段切分 ---------------- */

typedef struct {
    const char *p;      /* 下一个待切的位置 */
} fields_t;

static void fields_init(fields_t *f, const char *s) { f->p = s; }

/* 按逗号切出下一个字段。字段超长会被截断（保证 out 以 '\0' 结尾）。 */
static bool fields_next(fields_t *f, char *out, int out_len)
{
    if (f->p == NULL || *f->p == '\0') return false;

    int i = 0;
    while (f->p[i] != '\0' && f->p[i] != ',') {
        if (i < out_len - 1) out[i] = f->p[i];
        i++;
    }
    int n = (i < out_len - 1) ? i : out_len - 1;
    out[n] = '\0';

    f->p += i;
    if (*f->p == ',') f->p++;      /* 跳过逗号 */
    return true;
}

/* 把 "T=26.4" 切成 key 和 value。没有 '=' 返回 false。 */
static bool kv_split(const char *tok, char *key, int key_len,
                     char *val, int val_len)
{
    const char *eq = strchr(tok, '=');
    if (eq == NULL) return false;
    if (eq == tok)  return false;               /* "=xx" 没有 key，非法 */

    int kl = (int)(eq - tok);
    if (kl > key_len - 1) kl = key_len - 1;
    memcpy(key, tok, kl);
    key[kl] = '\0';

    const char *v = eq + 1;
    int vl = (int)strlen(v);
    if (vl > val_len - 1) vl = val_len - 1;
    memcpy(val, v, vl);
    val[vl] = '\0';

    return true;
}

/* ---------------- 每条链路的接收状态 ---------------- */

typedef struct {
    /* 正在攒的这一行（只由 ISR 写） */
    char     rx_line[HOST_RX_LINE_LEN];
    int      rx_len;
    /* 最近收满的一行（ISR 写、任务读） */
    char     done_line[HOST_RX_LINE_LEN];
    volatile bool line_ready;
    bool     rx_overflow;       /* 本行太长被丢掉了，等下一个终止符重新同步 */
    bool     rx_saw_cr;         /* 上一字节是 '\r'，用来吃掉 \r\n 里的 '\n' */

    /* 下行链路诊断计数器。都随遥测帧发给上位机（RXB/RXL/RXO）。
       32 位对齐量，Cortex-M4 上的单个读写是原子的，不用加保护。 */
    volatile uint32_t rx_bytes; /* ISR 累加：收到的字节总数 */
    volatile uint32_t rx_lines; /* ISR 累加：收满的行数（可能校验不过） */
    uint32_t          rx_ok;    /* 任务累加：校验通过并生效的命令数 */
} host_link_t;

static host_link_t lk_state[UART_LINK_COUNT];

/* 链路号合法性检查。放在这一层而不是让越界访问悄悄发生 ——
 * 越界写静态数组不会立刻崩，只会在别的地方冒出诡异现象。 */
static bool lk_valid(int lk)
{
    return (lk >= 0 && lk < UART_LINK_COUNT);
}

void host_proto_init(void)
{
    memset(lk_state, 0, sizeof(lk_state));
}

bool host_start(int lk)
{
    if (!lk_valid(lk)) return false;

    host_rx_reset(lk);
    return uart_rx_start_host(lk);
}

/* ISR 上下文：丢掉收到的半截行 */
void host_rx_reset(int lk)
{
    if (!lk_valid(lk)) return;

    lk_state[lk].rx_len      = 0;
    lk_state[lk].rx_overflow = false;
}

/* 把攒好的一行搬进"可取走"的槽位。
   不直接让任务去读 rx_line 的原因：rx_line 马上要从头攒下一行，
   任务读到半截就废了。 */
static bool line_finish(int lk)
{
    host_link_t *s = &lk_state[lk];

    if (s->rx_overflow) {
        /* 这一行太长已经作废，重新同步 */
        s->rx_overflow = false;
        s->rx_len      = 0;
        return false;
    }

    s->rx_line[s->rx_len] = '\0';
    memcpy(s->done_line, s->rx_line, (size_t)s->rx_len + 1);
    s->rx_len     = 0;
    s->line_ready = true;
    s->rx_lines++;
    return true;
}

bool host_rx_byte(int lk, uint8_t b)
{
    if (!lk_valid(lk)) return false;

    host_link_t *s = &lk_state[lk];

    s->rx_bytes++;

    /* 行终止符：'\r'、'\n'、"\r\n" 三种都接受。
       串口助手的"发送新行"有的加 \r\n、有的只加 \r、有的只加 \n ——
       只认其中一种的话，换个助手就变成"命令完全没反应"，而且从现象上
       根本看不出是行终止符的问题。这里三种都认。 */
    if (b == '\n' || b == '\r') {
        bool was_cr = s->rx_saw_cr;
        s->rx_saw_cr = (b == '\r');

        if (b == '\n' && was_cr) {
            return false;   /* 这是 \r\n 的第二个字符，上一字节已经结过行了 */
        }
        return line_finish(lk);
    }

    s->rx_saw_cr = false;

    if (s->rx_len >= HOST_RX_LINE_LEN - 1) {
        /* 溢出了。不能就地截断当一帧用 —— 那样只会喂给解析器一堆噪声。
           整行丢掉，等下一个终止符重新对齐。 */
        s->rx_overflow = true;
        return false;
    }

    s->rx_line[s->rx_len++] = (char)b;
    return false;
}

bool host_take_line(int lk, char *out, int out_len)
{
    if (!lk_valid(lk))               return false;
    if (out == NULL || out_len <= 0) return false;

    host_link_t *s = &lk_state[lk];
    if (!s->line_ready)              return false;

    int n = (int)strlen(s->done_line);
    if (n > out_len - 1) n = out_len - 1;
    memcpy(out, s->done_line, (size_t)n);
    out[n] = '\0';

    s->line_ready = false;
    return true;

    /* 这里没有关中断再抄。ISR 只在整行收满的那一刻写一次 done_line
       （约 2µs），而任务 5ms 才来取一次，撞上的概率极低；就算正好撞上，
       读到的也是残行，校验和会把它拦下来，不会把坏参数写进 cfg。
       用校验和兜底比在这里关中断简单，而且不用把 FreeRTOS 拉进这一层
       （拉进来 PC 上就没法单测了）。 */
}

/* ---------------- 上行：组帧 ---------------- */

int host_build_telemetry(char *buf, int buflen, uint8_t node, int lk,
                         const env_t *e, const cfg_t *c)
{
    if (buf == NULL || e == NULL || c == NULL) return 0;
    if (!lk_valid(lk))                         return 0;
    if (buflen < 32)                           return 0;

    char t[12], h[12];
    fmt_x10(t, sizeof t, to_x10(e->temp_c));
    fmt_x10(h, sizeof h, to_x10(e->humi_rh));

    /* 设备**当前实际在用**的阈值。上位机靠这几个字段能核对"我设的值到底
       生效了没"，也能用来诊断"为什么报警等级和我预期的不一样" ——
       屏幕显示的值和固件里真正用的值不一定是同一个。 */
    char ts[12], hs[12];
    fmt_x10(ts, sizeof ts, to_x10(c->temp_hi));
    fmt_x10(hs, sizeof hs, to_x10(c->humi_hi));

    /* 21VOC 掉线时 LINK=0，上位机据此知道温湿/CO2 那几路是过期值，
       别当成实时数据记进数据库。 */
    int link = e->voc_online ? 1 : 0;
    int alm  = (int)env_alarm_level(e, c);

    /* 下行链路诊断计数取**这条链路**的 —— 你从哪个口收到这帧，
       报的就是那个口的接收情况，上位机据此判断自己这边通不通。 */
    const host_link_t *s = &lk_state[lk];

    int n = snprintf(buf, (size_t)buflen,
                     "$ENV,%u,T=%s,H=%s,CO2=%u,VOC=%u,HCHO=%u,"
                     "SMK=%d,GAS=%d,LUX=%d,ALM=%d,LINK=%d,AUTO=%d,MAN=%d,"
                     "RXB=%u,RXL=%u,RXO=%u,"
                     "SPT=%s,SPH=%s,SPC=%u,SPS=%d,SPG=%d,SPL=%d",
                     (unsigned)node,
                     t, h,
                     (unsigned)e->eco2_ppm,
                     (unsigned)e->voc_ugm3,
                     (unsigned)e->hcho_ugm3,
                     (int)(e->smoke_idx + 0.5f),
                     (int)(e->co_idx    + 0.5f),
                     (int)(e->light_idx + 0.5f),
                     alm, link,
                     c->auto_fan   ? 1 : 0,
                     c->manual_fan ? 1 : 0,
                     (unsigned)s->rx_bytes,
                     (unsigned)s->rx_lines,
                     (unsigned)s->rx_ok,
                     ts, hs,
                     (unsigned)c->eco2_hi,
                     (int)(c->smoke_hi + 0.5f),
                     (int)(c->gas_hi   + 0.5f),
                     (int)(c->light_lo + 0.5f));

    /* snprintf 返回的是"本该写多少字符"，所以 n >= buflen 就说明被截断了 */
    if (n <= 0 || n + 6 > buflen) return 0;      /* 6 = "*XX\r\n" + '\0' */

    uint8_t cs = xor_bytes(buf + 1, n - 1);      /* '$' 和 '*' 之间 */

    int m = snprintf(buf + n, (size_t)(buflen - n), "*%02X\r\n", (unsigned)cs);
    if (m != 5) return 0;                        /* "*" + 2 位 + CRLF */

    return n + m;
}

/* ---------------- 下行：解析 ---------------- */

/* 钳制到 [lo, hi] */
static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint16_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return (uint16_t)lo;
    if (v > hi) return (uint16_t)hi;
    return (uint16_t)v;
}

/* 处理一个 "key=value"。认识的 key 就写进 cfg，不认识的忽略（向前兼容）。 */
static void apply_kv(const char *key, const char *val, cfg_t *c)
{
    int32_t x10;

    if (strcmp(key, "T") == 0 && parse_x10(val, &x10)) {
        c->temp_hi = clampf((float)x10 / 10.0f, CFG_TEMP_MIN, CFG_TEMP_MAX);
    } else if (strcmp(key, "H") == 0 && parse_x10(val, &x10)) {
        c->humi_hi = clampf((float)x10 / 10.0f, CFG_HUMI_MIN, CFG_HUMI_MAX);
    } else if (strcmp(key, "CO2") == 0 && parse_x10(val, &x10)) {
        c->eco2_hi = clampi(x10 / 10, CFG_ECO2_MIN, CFG_ECO2_MAX);
    } else if (strcmp(key, "SMK") == 0 && parse_x10(val, &x10)) {
        c->smoke_hi = clampf((float)x10 / 10.0f, CFG_SMOKE_MIN, CFG_SMOKE_MAX);
    } else if (strcmp(key, "GAS") == 0 && parse_x10(val, &x10)) {
        c->gas_hi = clampf((float)x10 / 10.0f, CFG_GAS_MIN, CFG_GAS_MAX);
    } else if (strcmp(key, "LUX") == 0 && parse_x10(val, &x10)) {
        c->light_lo = clampf((float)x10 / 10.0f, CFG_LIGHT_MIN, CFG_LIGHT_MAX);
    } else if (strcmp(key, "AUTO") == 0) {
        c->auto_fan = (val[0] == '1');
    } else if (strcmp(key, "MAN") == 0) {
        c->manual_fan = (val[0] == '1');
    }
    /* 其它 key 一律忽略 */
}

bool host_parse_cmd(const char *line, uint8_t my_node, cfg_t *c)
{
    if (line == NULL || c == NULL) return false;
    if (line[0] != '$')               return false;

    /* 校验段必须是 "*XX" 结尾（XX 是两位十六进制） */
    const char *star = strrchr(line, '*');
    if (star == NULL) return false;
    if (strlen(star) != 3) return false;

    unsigned got = 0;
    if (sscanf(star + 1, "%2X", &got) != 1) return false;

    /* 先验校验，通过了才动 cfg —— 绝不能"边解析边改"，
       否则一帧被干扰的命令会把参数改坏一半。 */
    uint8_t want = xor_bytes(line + 1, (int)(star - line) - 1);
    if ((uint8_t)got != want) return false;

    /* 把 '$' 之后、'*' 之前的正文拷出来单独切分。
       不能直接拿 line+1 去切 —— 那样最后一个字段会带着 "*XX" 尾巴
       （比如 "T=25*7A"），数值解析必然失败，表现为"每条命令的最后一个
       字段永远改不动"。 */
    char body[HOST_RX_LINE_LEN];
    int body_len = (int)(star - line) - 1;
    if (body_len <= 0 || body_len >= (int)sizeof body) return false;
    memcpy(body, line + 1, (size_t)body_len);
    body[body_len] = '\0';

    fields_t f;
    char tok[40], key[16], val[16];

    fields_init(&f, body);
    if (!fields_next(&f, tok, sizeof tok)) return false;

    /* 第一个字段应该是帧类型名（"SET" / "FAN"，不含 '='），
       紧接着是 node。 */
    if (strchr(tok, '=') == NULL) {

        if (!fields_next(&f, tok, sizeof tok)) return false;   /* node */

        /* ---- node 校验 ----
           0 表示广播，任何节点都执行；其它值必须等于本机编号。
           没有这一条的话，RS485 总线上的命令会被**所有节点一起执行** ——
           多节点组网时这是个致命的错。 */
        int32_t nd10;
        if (!parse_x10(tok, &nd10))            return false;
        if (nd10 % 10 != 0)                    return false;   /* node 必须是整数 */
        int32_t nd = nd10 / 10;
        if (nd != 0 && nd != (int32_t)my_node) return false;   /* 不是给我的 */
    }
    /* 没有帧类型名（第一个字段就带 '='）的话，当作没有 node 处理 ——
       这种帧不是本协议的格式，后面的字段同样会被尝试解析，
       但不匹配任何已知 key 就什么都不会发生。 */

    /* 再往后每个字段都是 key=value */
    while (fields_next(&f, tok, sizeof tok)) {
        if (kv_split(tok, key, sizeof key, val, sizeof val)) {
            apply_kv(key, val, c);
        }
    }

    return true;
}
