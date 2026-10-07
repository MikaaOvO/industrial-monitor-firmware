#include "ili9341.h"

extern SPI_HandleTypeDef hspi1;  // CubeMX 生成的 SPI1 句柄

/* 写命令 */
static void ILI9341_WriteCmd(uint8_t cmd)
{
    DC_L();
    CS_L();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, 100);
    CS_H();
}

/* 写 1 字节数据 */
static void ILI9341_WriteData8(uint8_t data)
{
    DC_H();
    CS_L();
    HAL_SPI_Transmit(&hspi1, &data, 1, 100);
    CS_H();
}

/* 批量写像素（DMA 方式，启动后立即返回；完成中断在 lv_port_disp.c） */
void ILI9341_WritePixels(uint16_t *color_buf, uint32_t len)
{
    DC_H();
    CS_L();
    HAL_SPI_Transmit_DMA(&hspi1, (uint8_t *)color_buf, (uint16_t)(len * 2));
}

/* 设置显示窗口 */
void ILI9341_SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    ILI9341_WriteCmd(0x2A); // 列地址
    ILI9341_WriteData8(x1 >> 8);
    ILI9341_WriteData8(x1 & 0xFF);
    ILI9341_WriteData8(x2 >> 8);
    ILI9341_WriteData8(x2 & 0xFF);

    ILI9341_WriteCmd(0x2B); // 页地址
    ILI9341_WriteData8(y1 >> 8);
    ILI9341_WriteData8(y1 & 0xFF);
    ILI9341_WriteData8(y2 >> 8);
    ILI9341_WriteData8(y2 & 0xFF);

    ILI9341_WriteCmd(0x2C); // 显存写入
}

/* ILI9341 初始化序列 */
void ILI9341_Init(void)
{
    /* 硬件复位 */
    RES_L();
    HAL_Delay(20);
    RES_H();
    HAL_Delay(120);

    ILI9341_WriteCmd(0xCF);
    ILI9341_WriteData8(0x00);
    ILI9341_WriteData8(0xC1);
    ILI9341_WriteData8(0X30);

    ILI9341_WriteCmd(0xED);
    ILI9341_WriteData8(0x64);
    ILI9341_WriteData8(0x03);
    ILI9341_WriteData8(0X12);
    ILI9341_WriteData8(0X81);

    ILI9341_WriteCmd(0xE8);
    ILI9341_WriteData8(0x85);
    ILI9341_WriteData8(0x00);
    ILI9341_WriteData8(0x78);

    ILI9341_WriteCmd(0xCB);
    ILI9341_WriteData8(0x39);
    ILI9341_WriteData8(0x2C);
    ILI9341_WriteData8(0x00);
    ILI9341_WriteData8(0x34);
    ILI9341_WriteData8(0x05);

    ILI9341_WriteCmd(0xF7);
    ILI9341_WriteData8(0x20);

    ILI9341_WriteCmd(0xEA);
    ILI9341_WriteData8(0x00);
    ILI9341_WriteData8(0x00);

    ILI9341_WriteCmd(0xC0);    // 电源控制1
    ILI9341_WriteData8(0x23);

    ILI9341_WriteCmd(0xC1);    // 电源控制2
    ILI9341_WriteData8(0x10);

    ILI9341_WriteCmd(0xC5);    // VCOM 控制1
    ILI9341_WriteData8(0x3e);
    ILI9341_WriteData8(0x28);

    ILI9341_WriteCmd(0xC7);    // VCOM 控制2
    ILI9341_WriteData8(0x86);

    ILI9341_WriteCmd(0x36);    // 内存访问控制
    ILI9341_WriteData8(0x48);  // 竖屏 RGB 顺序

    ILI9341_WriteCmd(0x3A);    // 像素格式
    ILI9341_WriteData8(0x55);  // 16位 RGB565

    ILI9341_WriteCmd(0xB1);    // 帧速率控制
    ILI9341_WriteData8(0x00);
    ILI9341_WriteData8(0x18);

    ILI9341_WriteCmd(0xB6);    // 显示功能控制
    ILI9341_WriteData8(0x08);
    ILI9341_WriteData8(0x82);
    ILI9341_WriteData8(0x27);

    ILI9341_WriteCmd(0xF2);    // 3伽马关闭
    ILI9341_WriteData8(0x00);

    ILI9341_WriteCmd(0x26);    // 伽马曲线
    ILI9341_WriteData8(0x01);

    ILI9341_WriteCmd(0xE0);    // 正伽马校准
    uint8_t gamma_p[] = {0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00};
    for(uint8_t i=0;i<15;i++) ILI9341_WriteData8(gamma_p[i]);

    ILI9341_WriteCmd(0xE1);    // 负伽马校准
    uint8_t gamma_n[] = {0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F};
    for(uint8_t i=0;i<15;i++) ILI9341_WriteData8(gamma_n[i]);

    ILI9341_WriteCmd(0x11);    // 退出睡眠
    HAL_Delay(120);
    ILI9341_WriteCmd(0x29);    // 开启显示
    BL_ON();                   // 点亮背光
}
