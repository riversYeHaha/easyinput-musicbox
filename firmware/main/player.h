#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 播放模块：
 * songs 分区 MP3 → Helix 定点解码 → I2S1 MAX98357A（STD 16bit 立体声，
 * 采样率跟随 MP3 帧头，默认 48kHz；MAX98357A 硬件接法下左右声道送同样数据）。
 * 与 recorder 互斥：录音进行中拒绝播放；录音发起方负责先停播放。
 */

esp_err_t player_init(void);

/* 播放歌曲库中第 index 首（播放中切歌直接换；录音中拒绝） */
esp_err_t player_play_index(int index);

/* 播放/暂停切换（旋钮按压） */
esp_err_t player_pause_toggle(void);

/* 停止当前播放（等待音频任务退出，短阻塞） */
esp_err_t player_stop(void);

/* true = 正在出声（暂停为 false） */
bool player_is_playing(void);

/* 音量 0–100（软件增益，线性） */
esp_err_t player_set_volume(uint8_t vol);

/* ---- 供录音回放复用喇叭通道（仅播放器空闲时可调，recorder 内部使用） ---- */
esp_err_t player_spk_reconfig_clock(uint32_t sample_rate);
esp_err_t player_spk_write(const void *buf, size_t len, uint32_t timeout_ms);
