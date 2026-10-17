#pragma once

#include <stdbool.h>

/* 编码器：A/B 正交相位由 PCNT 硬件解码（不当普通按住键），按压低有效。UI 任务 10ms 轮询。 */
void encoder_init(void);
void encoder_poll(void);

/* 按压短按事件：读取后自动清除 */
bool encoder_short_press(void);

/* 旋转增量：每档 ±1（4x 解码后 ÷4），读取后清零 */
int encoder_delta_detents(void);
