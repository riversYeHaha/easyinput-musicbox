#pragma once

#include "esp_err.h"

/* GPIO8 拉高后等待共享电源稳定的项目常量（毫秒）。
 * 注意：此值是项目值，非板级已证明的硬件最小值，待实板测量确认。 */
#define POWER_STABLE_MS  50

/*
 * GPIO8 高有效共享电源（LED / 麦克风 / 扬声器）冷启动：
 *   1. GPIO8 与下游命令脚锁安全态（输出低）；麦克风脚只保持浮空，不初始化
 *   2. 拉高 GPIO8
 *   3. 等待 POWER_STABLE_MS 稳定（待实板测量确认）
 *   4. 返回成功
 * 本段不做 WS2812 / I2S / 麦克风的任何初始化。
 */
esp_err_t power_up(void);
