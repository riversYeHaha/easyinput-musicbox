#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * LED 状态层：5 颗 WS2812（GPIO12，RMT 驱动）。
 * 只允许 UI 任务调用；内部 20ms 帧率限制。
 * 灯效语义（musicbox 项目私有合同，见实现方案 §1）：
 *   录音中    = 全红常亮
 *   生成中    = 跑马灯旋转
 *   生成完毕  = 绿色闪烁 3 次后自动熄灭（回 IDLE）
 *   播放中    = 蓝色呼吸（占位色，可后调）
 *   空闲      = 全灭
 */
typedef enum {
    LED_MODE_IDLE = 0,   /* 全灭 */
    LED_MODE_RECORDING,  /* 录音中：全红常亮 */
    LED_MODE_GENERATING, /* 生成中：跑马灯旋转 */
    LED_MODE_DONE,       /* 生成完毕：绿闪 3 次后自动回 IDLE */
    LED_MODE_PLAYING,    /* 播放中：蓝色呼吸 */
} led_mode_t;

void led_status_init(void); /* 失败时内部置空句柄，poll 静默跳过 */

/* 切换灯效模式 */
void led_status_set_mode(led_mode_t mode);
led_mode_t led_status_mode(void);

/* UI 任务每圈调用；内部限帧 20ms，按当前模式渲染 */
void led_status_poll(void);
