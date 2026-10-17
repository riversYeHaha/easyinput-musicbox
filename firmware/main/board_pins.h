#pragma once

/*
 * EasyInput 奇妙音乐盒（musicbox）—— 板级引脚表
 * 产品名 EasyInput V2.0 / 固件板型别名 v2 / PCB 丝印 AI Keyboard V2.1
 * SoC：ESP32-S3R8。三个名字是同一块板，不是三套电路。
 * 引脚事实以《EasyInput-V2.0-开发基线文档》§2 为准。
 */

/* GPIO8：高有效共享电源，同时给 LED、麦克风、扬声器供电 */
#define PIN_POWER_EN   8

/* 按键 S1-S8：八路独立低有效输入（非矩阵键盘）。GPIO0 只属于 BOOT。 */
#define PIN_S1   2
#define PIN_S2   47
#define PIN_S3   38
#define PIN_S4   41
#define PIN_S5   1
#define PIN_S6   6
#define PIN_S7   7
#define PIN_S8   48

/* 编码器：A/B 为正交相位，按压低有效 */
#define PIN_ENC_A    17
#define PIN_ENC_B    16
#define PIN_ENC_SW   18

/* 扬声器 MAX98357A（I2S） */
#define PIN_I2S_BCLK   14
#define PIN_I2S_WS     13
#define PIN_I2S_DOUT   15

/* 5 颗 WS2812 串联 */
#define PIN_LED_DATA   12
#define LED_COUNT      5

/* 麦克风：M1 阶段不接实时链路，不初始化，引脚保持浮空（GPIO11 硬性浮空） */
#define PIN_MIC_0    9
#define PIN_MIC_1    10
#define PIN_MIC_2    11

/* 原生 USB：GPIO19/20（由 USB Serial/JTAG 外设占用，无需 GPIO 配置） */
