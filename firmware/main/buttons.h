#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * S1–S8 八路独立低有效按键，UI 任务 10ms 轮询去抖（20ms）。
 * 语义（musicbox 项目私有合同）：
 *   - S1/S2/S4/S5/S6/S7/S8：短按（去抖后按下沿即时触发，读取后清除）
 *   - S3：长按检测（按住 >= BUTTONS_LONG_PRESS_MS 进长按，松开退出）；
 *         未达到长按时长的点按也会给一次 short_press（当前业务未用）
 */

#define BUTTONS_COUNT 8

/* 按键索引（按 S1..S8 自然顺序） */
enum {
    BTN_S1 = 0,
    BTN_S2,
    BTN_S3,
    BTN_S4,
    BTN_S5,
    BTN_S6,
    BTN_S7,
    BTN_S8,
};

#define BUTTONS_LONG_PRESS_MS 500

void buttons_init(void);
void buttons_poll(void);

/* 消抖后的当前电平：true=按下（keytest 用） */
bool buttons_level(uint8_t idx);

/* 短按事件：读取后自动清除 */
bool buttons_short_press(uint8_t idx);

/* S3 长按事件：进入/退出各触发一次，读取后自动清除 */
bool buttons_long_start(uint8_t idx);
bool buttons_long_end(uint8_t idx);
