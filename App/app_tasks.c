#include "app_tasks.h"
#include "app_env.h"
#include "sensor.h"
#include "voc.h"
#include "host_proto.h"
#include "indicator.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"   /* configASSERT 在 FreeRTOSConfig.h 里 */
#include "lvgl.h"

/* ================================================================
 * 任务骨架
 *
 * 每个任务该干什么、数据往哪写，都写在函数开头的注释里。
 * 现在只有 Task_Lvgl 是完整的，另外三个是空转等待接外设。
 * ================================================================ */

/* ----------------------------------------------------------------
 * 栈大小。注意 CMSIS-RTOS2 里 osThreadAttr_t.stack_size 的单位是
 * **字节**，不是字 —— 384*4 才是 1.5KB。
 *
 * 这些栈都从 FreeRTOS 的堆里出（idle 和 timer 任务走 CMSIS-RTOS2 的
 * 静态内存，不占这个堆）。configTOTAL_HEAP_SIZE 现在是 32KB：
 *
 *     总堆                         32,768 B
 *       − LVGL 任务(8192 栈+TCB)    8,292
 *       − Sensor / Voc / Output     各 ~1.7KB
 *       − Host                       ~2.1KB
 *       = 剩                        ~ 16KB
 *
 * 余量很宽，所以栈不用抠得那么紧；真不够了 osThreadNew 只会返回 NULL
 * 且不报错，所以 app_tasks_create() 里用 configASSERT 兜底，
 * 另外 app_tasks_heap_left() 可以打印出来看实际剩多少。
 * ---------------------------------------------------------------- */
#define STACK_SENSOR    (384 * 4)    /* 1.5KB：ADC 读数 + 浮点换算      */
#define STACK_VOC       (384 * 4)    /* 1.5KB：12 字节帧解析            */
#define STACK_HOST      (640 * 4)    /* 2.5KB：要给两条链路各组一帧，
                                        snprintf 走 %s 路径吃栈较多 */
#define STACK_OUTPUT    (384 * 4)    /* 1.5KB：只算几个数、写寄存器     */

/* ----------------------------------------------------------------
 * 优先级。数值越大优先级越高。
 * LVGL 任务（CubeMX 的 defaultTask）现在是 osPriorityNormal，
 * 可以按上面的建议调成 osPriorityAboveNormal，界面会更跟手；
 * 不改也能正常工作。
 *
 * 这几个业务任务绝大部分时间都阻塞着，优先级差别影响很小。只有
 * Task_Host 给低一档 —— 它的阻塞发送一次要占 7ms 左右，排在最后
 * 免得去抢别人。
 * ---------------------------------------------------------------- */
#define PRIO_SENSOR     osPriorityNormal
#define PRIO_VOC        osPriorityNormal
#define PRIO_OUTPUT     osPriorityNormal
#define PRIO_HOST       osPriorityBelowNormal

/* 执行器任务的周期。风扇本身有斜率限制、界面 200ms 才刷一次，
   100ms 足够 —— 再快只是白唤醒。 */
#define OUTPUT_PERIOD_MS    100

/* VOC_TIMEOUT_MS 定义在 voc.h 里。这里曾经又定义了一份（值不一样），
   因为它在 #include "voc.h" 之后，把 voc.h 里的值悄悄覆盖掉了 —— 改一边
   不生效，而且只有编译器警告能发现。协议相关的常量一律归 voc.h 管。 */

static osThreadId_t sensorTaskHandle;
static osThreadId_t vocTaskHandle;
static osThreadId_t hostTaskHandle;
static osThreadId_t outputTaskHandle;

/* 建完任务后还剩多少 FreeRTOS 堆。堆不够时 osThreadNew 只是返回 NULL，
   不报错也不提示，是这类工程里最难查的坑之一 —— 所以留个可以打印
   出来看的数。等 Task_Host 通了，把它发到上位机看一眼。 */
static uint32_t s_heap_left = 0;

/* ================================================================
 * LVGL 引擎
 *
 * 这个循环就是 LVGL 的"心脏"：跑 LVGL 定时器、读触摸、判断哪些
 * 区域脏了要重绘。重绘时它会回调 lv_port_disp.c 里的 disp_flush，
 * 像素由 SPI1 的 DMA 搬走，DMA 传完在中断里调 lv_disp_flush_ready。
 *
 * 所以这里不是"刷新一帧"，而是"驱动引擎"，一帧要不要重绘由 LVGL
 * 自己决定。5ms 是实测可用的值（这块屏已经点亮验证过），别随便改大。
 * ================================================================ */
void Task_Lvgl(void *argument)
{
    (void)argument;

    for (;;) {
        lv_timer_handler();
        osDelay(5);
    }
}

/* ================================================================
 * 传感器采集
 *
 * 硬件：ADC1 三路，扫描 + DMA 循环搬运，硬件自己在转，CPU 不参与。
 *   Rank 1  PA0  MQ-2 烟雾
 *   Rank 2  PA1  MQ-7 有毒气体
 *   Rank 3  PC0  光敏
 *
 * 采集、滤波、两点标定全在 App/sensor.c 里，这个任务只负责按周期取一次
 * 读数写进共享数据。要调滤波参数或填标定端点，去改 sensor.c 的 cal[]
 * 和 EMA_ALPHA —— 那个文件里写了端点怎么实测。
 *
 * 顺便记一条容易踩的：模块上那个电位器对 AO 没用，它只接在 LM393
 * 比较器上决定 DO 的电平翻转。AO 是敏感元件的分压直出，公式里没有
 * 电位器那一项。想验证：盯着 ADC 读数拧电位器 —— AO 不动、板载 LED 翻转。
 * （走 DO 当阈值能用手拧，但那样上位机就改不了阈值了，和需求冲突。）
 * ================================================================ */
void Task_Sensor(void *argument)
{
    (void)argument;

    /* 启动 ADC1 的 DMA 循环搬运。
       注意别把 sensor_init() 直接塞进 configASSERT —— 万一以后有人把
       configASSERT 改成空宏，这个函数就永远不会被调用了，而且不报错。 */
    bool adc_ok = sensor_init();
    configASSERT(adc_ok);
    (void)adc_ok;

    for (;;) {
        float light, smoke, co;
        sensor_read(&light, &smoke, &co);
        env_set_adc(light, smoke, co);

        osDelay(100);
    }
}

/* ================================================================
 * 21VOC 模块（UART4，PC11=RX / PC10=TX，9600 8N1，模块主动上报）
 *
 * 协议、帧格式、手册里那两个坑（*28 其实是 *256、负温度编码差 1），
 * 以及 ISR 与任务的分工，全都写在 App/voc.c 里了。这个任务本身只负责：
 *
 *   进循环前：voc_start() 启动单字节中断接收（之后中断自维持）
 *   循环里：  每 20ms 看一次有没有收满一帧 ->
 *             有就校验+解析，成功写 env_set_voc()；
 *             连续 VOC_TIMEOUT_MS 没拿到合法帧就 env_set_voc_lost()，
 *             界面会回落到占位符，而不是继续显示过期数据。
 *
 * 为什么用轮询而不是在中断里通知任务：这样中断里完全不碰 FreeRTOS 的
 * API，和优先级约束彻底解耦。帧每秒才来一帧，20ms 的延迟无所谓。
 * ================================================================ */
void Task_Voc(void *argument)
{
    (void)argument;

    bool uart_ok = voc_start();
    configASSERT(uart_ok);
    (void)uart_ok;

    uint32_t silent_ms = 0;

    for (;;) {
        uint8_t frame[VOC_FRAME_LEN];
        bool got_good = false;

        if (voc_take_frame(frame)) {
            voc_data_t d;
            if (voc_parse(frame, &d)) {
                env_set_voc(d.temp_c, d.humi_rh,
                            d.eco2_ppm, d.voc_ugm3, d.hcho_ugm3);
                voc_note_ok_frame();
                got_good = true;
            }
            /* 校验不过就当作"没收到"处理，下面照常计时。
               注意别在这里提前 continue，否则一连串的错帧永远不会
               触发掉线判定，界面会一直挂着最后一次的好数据。 */
        }

        /* 把诊断计数器同步给界面（临时调试用，见 ui.c 的 VOC_DEBUG_UI） */
        env_set_voc_debug(voc_rx_bytes(), voc_frames_ok());

        if (got_good) {
            silent_ms = 0;
        } else {
            silent_ms += VOC_POLL_MS;
            if (silent_ms >= VOC_TIMEOUT_MS) {
                env_set_voc_lost();
            }
        }

        osDelay(VOC_POLL_MS);
    }
}

/* ================================================================
 * 上位机通信 —— 两条链路跑同一套协议
 *
 *   UART_LINK_DEBUG —— USART2 (PA2/PA3) + CH340     下位机调试用
 *   UART_LINK_BUS   —— USART6 (PC6/PC7) + MAX3485   上位机 RS485 总线
 *
 * 帧格式、字段含义、校验算法都在 App/host_proto.c 里。这个任务只负责：
 *
 *   进循环前：host_start() 给两条链路各启动一次接收中断
 *             （之后中断自维持，见 uart_rx.c）
 *   循环里：  ① 两条链路各取整行、解析，成功的命令写回共享 cfg
 *             ② 每 HOST_SEND_MS 在两条链路各发一帧遥测
 *
 * ---- 收发为什么不对称 ----
 *   收必须靠中断：115200 下一个字节只有 87µs，5ms 轮询一次中间会到
 *   50 多个字节，只能捞到 1 个，一帧都收不全。
 *   发是主动的、每 500ms 一次，用阻塞发送最简单 —— 一帧约 200 字节在
 *   115200 下约 17ms，相对于 500ms 周期可以接受。
 *   RS485 那条链路的 DE 方向控制就包在 uart_tx_send() 里，靠阻塞发送
 *   内部的 TC 等待保证时序（见 uart_rx.c 的注释）。
 *
 * ---- 边界 ----
 *   收到阈值后写 cfg_set() 就行，界面会在 200ms 内自己同步过来。
 *   这个任务**不要碰任何 lv_* 函数**（LVGL 不是线程安全的）。
 * ================================================================ */

/* 循环周期。取 5ms 而不是 20ms 的原因：每条链路的行缓冲只有一个槽位，
   如果两条命令在同一个轮询窗口内都收满，前一条会被后一条覆盖掉。
   115200 下一条 40 字节的命令约 3.5ms，两条之间至少隔 7ms，所以 5ms
   的窗口里塞不下两条 —— 这样"上位机连着发多条命令"就不会丢。
   （代价是每秒 200 次唤醒，每次只查几个标志位，可以忽略。） */
#define HOST_POLL_MS     5
#define HOST_SEND_MS     500    /* 遥测上报周期 */
#define HOST_NODE_ID     1      /* 节点号。现在恒为 1，给 RS485 组网预留 */

void Task_Host(void *argument)
{
    (void)argument;

    /* 两条链路的接收中断各启动一次。失败十有八九是 CubeMX 里那个串口的
       全局中断没勾 —— uart_rx.c 里显式查了 NVIC 才拦得住。 */
    for (int lk = 0; lk < UART_LINK_COUNT; lk++) {
        bool ok = host_start(lk);
        configASSERT(ok);
        (void)ok;
    }

    /* 帧缓冲放静态区，别放栈上 —— Task_Host 的栈只有 2.5KB，
       256 字节的帧加上 printf 系的内部占用会很紧张。 */
    static char txbuf[HOST_TX_BUF_LEN];

    uint32_t since_send = 0;

    for (;;) {
        /* ---- ① 收：两条链路都看 ---- */
        for (int lk = 0; lk < UART_LINK_COUNT; lk++) {

            char one[HOST_RX_LINE_LEN];
            while (host_take_line(lk, one, sizeof one)) {

                cfg_t c;
                cfg_get(&c);
                /* 校验通过的合法命令才写回去。
                   node 不匹配的帧这里就返回 false，什么都不会改。 */
                if (host_parse_cmd(one, HOST_NODE_ID, &c)) {
                    cfg_set(&c);
                }
                /* 解析失败（校验错、格式错、node 不对）就无声丢弃。
                   不要在失败时去改 cfg —— 一帧被干扰的命令绝不能改坏一半参数。 */
            }
        }

        /* ---- ② 发：每 HOST_SEND_MS 在两条链路各发一帧 ---- */
        since_send += HOST_POLL_MS;
        if (since_send >= HOST_SEND_MS) {
            since_send = 0;

            env_t e;
            cfg_t c;
            env_get(&e);
            cfg_get(&c);

            for (int lk = 0; lk < UART_LINK_COUNT; lk++) {
                /* 两条链路的收发是独立的，所以同一时刻组出来的两帧内容
                   不完全相同 —— RXB/RXL/RXO 各自报自己那条链路的接收情况。
                   这是有意的：你从哪个口收到，报的就是那个口的链路状态。 */
                int n = host_build_telemetry(txbuf, sizeof txbuf,
                                             HOST_NODE_ID, lk, &e, &c);
                if (n > 0) {
                    /* 发不出去（超时）不做重试：500ms 后本来就有下一帧，
                       重试只会把过期的数据补发一遍。 */
                    uart_tx_send(lk, (const uint8_t *)txbuf, (uint16_t)n);
                }
            }
        }

        osDelay(HOST_POLL_MS);
    }
}

/* ================================================================
 * 执行器输出（风扇，以后加灯光）
 *
 * 单独一个任务而不是塞进 Task_Sensor，是为了把"采样进"和"执行出"分开：
 * 两者出错的原因和排查方式完全不同，混在一起以后会越来越难读。
 *
 * 具体怎么算占空比、斜率限制多少，都在 App/indicator.c 里。这个任务
 * 只负责按周期取一次快照、调一次更新。
 *
 * 注意：它写的是 TIM1 的寄存器，不碰 LVGL —— 界面那边由 ui.c 的
 * 定时器自己刷，两者通过共享数据解耦。
 * ================================================================ */
void Task_Output(void *argument)
{
    (void)argument;

    indicator_init();

    for (;;) {
        env_t e;
        cfg_t c;
        env_get(&e);
        cfg_get(&c);

        indicator_update(&e, &c);

        osDelay(OUTPUT_PERIOD_MS);
    }
}

/* ================================================================
 * 任务创建
 * ================================================================ */
void app_tasks_create(void)
{
    const osThreadAttr_t sensorAttr = {
        .name       = "SensorTask",
        .stack_size = STACK_SENSOR,
        .priority   = (osPriority_t)PRIO_SENSOR,
    };
    const osThreadAttr_t vocAttr = {
        .name       = "VocTask",
        .stack_size = STACK_VOC,
        .priority   = (osPriority_t)PRIO_VOC,
    };
    const osThreadAttr_t hostAttr = {
        .name       = "HostTask",
        .stack_size = STACK_HOST,
        .priority   = (osPriority_t)PRIO_HOST,
    };
    const osThreadAttr_t outputAttr = {
        .name       = "OutputTask",
        .stack_size = STACK_OUTPUT,
        .priority   = (osPriority_t)PRIO_OUTPUT,
    };

    sensorTaskHandle = osThreadNew(Task_Sensor, NULL, &sensorAttr);
    vocTaskHandle    = osThreadNew(Task_Voc,    NULL, &vocAttr);
    hostTaskHandle   = osThreadNew(Task_Host,   NULL, &hostAttr);
    outputTaskHandle = osThreadNew(Task_Output, NULL, &outputAttr);

    s_heap_left = xPortGetFreeHeapSize();

    /* 堆不够时 osThreadNew 返回 NULL，不报错、不提示，后面想用这个
       句柄的地方才会莫名其妙地炸。所以在这里主动挡一下 ——
       卡在 configASSERT 里说明该去调大 TOTAL_HEAP_SIZE 了。 */
    configASSERT(sensorTaskHandle != NULL);
    configASSERT(vocTaskHandle    != NULL);
    configASSERT(hostTaskHandle   != NULL);
    configASSERT(outputTaskHandle != NULL);
}

/* 建完任务后剩余的 FreeRTOS 堆字节数（给 Task_Host 上报用） */
uint32_t app_tasks_heap_left(void)
{
    return s_heap_left;
}
