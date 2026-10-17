#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * 录音模块：
 * I2S0 麦（GPIO9/10/11，STD MSB 32bit 单声道右声道）→ 16kHz/16bit/mono PCM
 * → 先进 PSRAM 缓冲（最长 30 秒），停止后落 rec 分区（0x1000 起数据区，
 * 64B 头 {magic/ver/sample_rate/frames/crc32} 最后写）。
 * S3 长按开始/松开停止；S4 回放最近录音（借播放器的 I2S1 喇叭通道，16kHz）。
 * 与 player 互斥：起录/回放前会先停播放；播放接口在录音中会拒绝。
 */

esp_err_t recorder_init(void);

/* 开始/停止录音（停止后落 rec 分区；缓冲录满自动停，仍需调 stop 落盘） */
esp_err_t recorder_start(void);
esp_err_t recorder_stop(void);

bool recorder_is_recording(void);

/* S4：回放最近一段录音（内部会先停歌曲播放） */
esp_err_t recorder_play_last(void);

/* true = 录音回放进行中 */
bool recorder_is_playing(void);

/* 停止录音回放（等回放任务退出，短阻塞） */
void recorder_stop_play(void);

/* ---- M4 读侧（rec_pull 上传用） ---- */

/* 最近一段录音的元信息：无录音返回 ESP_ERR_NOT_FOUND */
esp_err_t recorder_last_meta(uint32_t *frames, uint32_t *rate);

/* 从 REC_DATA_OFF + offset 读 PCM，长度按实际录音字节数截断；
 * 录音进行中拒绝（ESP_ERR_INVALID_STATE）；got 回实际读出字节数 */
esp_err_t recorder_read(uint32_t offset, uint8_t *buf, uint32_t len, uint32_t *got);
