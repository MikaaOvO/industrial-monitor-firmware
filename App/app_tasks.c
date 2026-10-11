#include "app_tasks.h"
#include "app_env.h"
#include "sensor.h"
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
 * 字节，不是字 —— 384*4 才是 1.5KB。
 *
 * 这些栈都是从 FreeRTOS 的堆里要的，而 configTOTAL_HEAP_SIZE 目前
 * 只有 15KB，所以要精打细算：
 *
 *     总堆                        15,360 B
 *       − LVGL 任务(8192 栈+TCB)   8,292
 *       = 剩                       7,068
 *       − 下面三个任务             5,420
 *       = 剩                       ~ 1,648 B
 *
 * （idle 和 timer 任务走 CMSIS-RTOS2 的静态内存，不占这个堆。）
 *
 * 1.6KB 余量够将来放一个 UART 的 StreamBuffer，但如果还要加东西，
 * 就得去 CubeMX 里把 FreeRTOS → Config parameters → TOTAL_HEAP_SIZE
 * 提到 32KB。注意那个 #define 在 FreeRTOSConfig.h 的 USER CODE 区
 * 之外，手改会被 CubeMX 重新生成时覆盖，必须在图形界面里改。
 * ---------------------------------------------------------------- */
#define STACK_SENSOR    (384 * 4)    /* 1.5KB：ADC 读数 + 浮点换算      */
#define STACK_VOC       (384 * 4)    /* 1.5KB：12 字节帧解析            */
#define STACK_HOST      (512 * 4)    /* 2.0KB：snprintf 组帧吃栈较多    */

/* ----------------------------------------------------------------
 * 优先级。数值越大优先级越高。
 * LVGL 任务（CubeMX 的 defaultTask）现在是 osPriorityNormal，
 * 可以按上面的建议调成 osPriorityAboveNormal，界面会更跟手；
 * 不改也能正常工作。
 * ---------------------------------------------------------------- */
#define PRIO_SENSOR     osPriorityNormal
#define PRIO_VOC        osPriorityNormal
#define PRIO_HOST       osPriorityBelowNormal

/* 21VOC 上报间隔约 1s，超过这个时间没收到合法帧就认为掉线 */
#define VOC_TIMEOUT_MS  3000

static osThreadId_t sensorTaskHandle;
static osThreadId_t vocTaskHandle;
static osThreadId_t hostTaskHandle;

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
 * 帧长固定 12 字节、帧头固定 0x2C，所以状态机很简单：
 *   找 0x2C -> 收满 12 字节 -> 校验和 -> 解析
 *
 * 接收不要用阻塞式 HAL_UART_Receive。正确做法：
 *   HAL_UARTEx_ReceiveToIdle_DMA() 启动接收，
 *   IDLE 中断里把整帧丢进 StreamBuffer / MessageQueue，
 *   这个任务阻塞在上面等。中断里只搬字节，不做解析。
 *
 * 帧结构（D[n] 是第 n 个字节）：
 *   D[0]      帧头 0x2C
 *   D[1..2]   VOC      ug/m3    = D[1]<<8 | D[2]
 *   D[3..4]   甲醛     ug/m3    = D[3]<<8 | D[4]
 *   D[5..6]   eCO2     ppm      = D[5]<<8 | D[6]
 *   D[7..8]   温度     0.1℃     = D[7]<<8 | D[8]
 *   D[9..10]  湿度     0.1%RH   = D[9]<<8 | D[10]
 *   D[11]     校验和 = 前 11 字节累加和取反 +1
 *
 * 两个坑：
 *   1) 手册印的是 *28，实际是 *256（PDF 把 2^8 的上标吃掉了）。
 *      用手册自己的例子验证：0x013D = 0x01*256+0x3D = 317 = 31.7℃，
 *      按 *28 算只有 89，对不上。
 *   2) 负温度编码和标准补码差 1：手册规则是"最高位为 1 表示零下，
 *      幅度 = 0xFFFF - 原始值"（-10 记作 0xFFF5）。
 *      按标准 int16 补码读 0xFFF5 会得到 -11。
 *      按手册规则实现，室内监测基本遇不到零下。
 * ================================================================ */
void Task_Voc(void *argument)
{
    (void)argument;

    for (;;) {
        /* TODO: 阻塞等一帧 -> 校验 -> 解析 -> env_set_voc(...)
                 超时 VOC_TIMEOUT_MS 没收到就 env_set_voc_lost() */

        osDelay(1000);      /* 骨架：先空转，接上 UART4 后删掉这行 */
    }
}

/* ================================================================
 * 上位机通信（USART2，PA2=TX / PA3=RX，115200 8N1，接 CH340）
 *
 * 一个串口既要发遥测又要收阈值命令，收发分两条路：
 *   发：每 500ms 取 env/cfg 快照，组帧，发出去
 *   收：字节进 StreamBuffer，这里跑状态机分帧 -> 校验 -> cfg_set()
 *
 * 组帧建议带 node 字段和 CRC —— 台账里 RS485 组网是预留项，
 * 现在就把格式定好，将来换物理层不用动上层代码。
 *
 * 注意：收到阈值后写 cfg_set() 就行，界面会在 200ms 内自己同步，
 *       这个任务不要碰任何 lv_* 函数。
 * ================================================================ */
void Task_Host(void *argument)
{
    (void)argument;

    for (;;) {
        /* TODO: 组帧上报 + 解析上位机下发的阈值命令 */

        osDelay(500);
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

    sensorTaskHandle = osThreadNew(Task_Sensor, NULL, &sensorAttr);
    vocTaskHandle    = osThreadNew(Task_Voc,    NULL, &vocAttr);
    hostTaskHandle   = osThreadNew(Task_Host,   NULL, &hostAttr);

    s_heap_left = xPortGetFreeHeapSize();

    /* 堆不够时 osThreadNew 返回 NULL，不报错、不提示，后面想用这个
       句柄的地方才会莫名其妙地炸。所以在这里主动挡一下 ——
       卡在 configASSERT 里说明该去调大 TOTAL_HEAP_SIZE 了。 */
    configASSERT(sensorTaskHandle != NULL);
    configASSERT(vocTaskHandle    != NULL);
    configASSERT(hostTaskHandle   != NULL);
}

/* 建完任务后剩余的 FreeRTOS 堆字节数（给 Task_Host 上报用） */
uint32_t app_tasks_heap_left(void)
{
    return s_heap_left;
}
