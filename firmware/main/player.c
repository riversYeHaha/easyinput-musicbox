#include "player.h"

#include <string.h>

#include "board_pins.h"
#include "recorder.h"
#include "songbook.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mp3dec.h"

static const char *TAG = "player";

/* MP3 输入滑窗（一帧 1152 样本 @128kbps/44.1k 约 418B，4KB 足够多帧缓冲） */
#define INBUF_SIZE        4096
/* Helix 单次解码最大输出：MAX_NGRAN(2) * MAX_NSAMP(576) * MAX_NCHAN(2) 个 int16 */
#define OUT_SAMPLES_MAX   2304
/* 默认时钟：首帧解码出来后按帧头采样率重配 */
#define DEFAULT_RATE_HZ   48000

typedef enum {
    PL_IDLE = 0,
    PL_PLAYING,
    PL_PAUSED,
} pl_state_t;

static i2s_chan_handle_t s_tx;
static HMP3Decoder s_dec;
static SemaphoreHandle_t s_play_sem; /* 二进制：唤醒播放任务 */

static volatile pl_state_t s_state = PL_IDLE;
static volatile int s_req_index = -1; /* 请求播放的槽索引；-1 = 无 */
static volatile bool s_req_stop;

static uint8_t s_volume = 50;
static int32_t s_vol_q15 = 50 * 32767 / 100; /* 线性增益 Q15 */

/* 播放任务私有大缓冲（BSS，不占栈） */
static uint8_t s_inbuf[INBUF_SIZE];
static int16_t s_outbuf[OUT_SAMPLES_MAX];
static int16_t s_i2sbuf[OUT_SAMPLES_MAX]; /* 单声道扩双声道用 */

static inline int16_t vol_scale(int16_t s)
{
    int32_t v = ((int32_t)s * s_vol_q15) >> 15;
    if (v > 32767) {
        v = 32767;
    } else if (v < -32768) {
        v = -32768;
    }
    return (int16_t)v;
}

/* 切采样率须先关通道；MAX98357A 无 mute 脚，auto_clear 保证切换期间输出静音 */
static esp_err_t spk_set_clock(uint32_t rate)
{
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx), TAG, "tx disable");
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &clk);
    if (err != ESP_OK) {
        (void)i2s_channel_enable(s_tx); /* 尽力恢复原时钟 */
        ESP_RETURN_ON_ERROR(err, TAG, "tx reconfig clock %lu", (unsigned long)rate);
    }
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx re-enable");
    ESP_LOGI(TAG, "spk clock -> %lu Hz", (unsigned long)rate);
    return ESP_OK;
}

/* 一帧 PCM 出喇叭：音量缩放；单声道复制成双声道（MAX98357A 两声道同数据） */
static void output_frame(const MP3FrameInfo *fi)
{
    size_t bytes;
    const void *buf;
    if (fi->nChans == 2) {
        for (int i = 0; i < fi->outputSamps; i++) {
            s_outbuf[i] = vol_scale(s_outbuf[i]);
        }
        buf = s_outbuf;
        bytes = (size_t)fi->outputSamps * sizeof(int16_t);
    } else {
        for (int i = 0; i < fi->outputSamps; i++) {
            int16_t v = vol_scale(s_outbuf[i]);
            s_i2sbuf[2 * i] = v;
            s_i2sbuf[2 * i + 1] = v;
        }
        buf = s_i2sbuf;
        bytes = (size_t)fi->outputSamps * 2 * sizeof(int16_t);
    }
    size_t written = 0;
    i2s_channel_write(s_tx, buf, bytes, &written, portMAX_DELAY); /* 阻塞写即解码节拍 */
}

/* 播一首歌：songbook 流式读 → Helix 解码 → I2S1。
 * 返回条件：自然播完 / s_req_stop / s_req_index 被改（切歌） */
static void run_song(int index)
{
    uint32_t file_off = 0;
    uint8_t *read_ptr = s_inbuf;
    int bytes_left = 0;
    bool eof = false;
    bool clock_set = false;
    MP3FrameInfo fi;

    ESP_LOGI(TAG, "play slot %d \"%s\"", index, songbook_get_title(index));

    while (!s_req_stop && s_req_index == index) {
        if (s_state == PL_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(20)); /* 暂停：不写 I2S，auto_clear 出静音 */
            continue;
        }

        /* 余量不足一半时移齐并填满滑窗（ID3/垃圾字节由 FindSyncWord 跳过） */
        if (bytes_left < INBUF_SIZE / 2 && !eof) {
            memmove(s_inbuf, read_ptr, (size_t)bytes_left);
            read_ptr = s_inbuf;
            size_t got = 0;
            if (songbook_read(index, file_off, s_inbuf + bytes_left,
                              INBUF_SIZE - (size_t)bytes_left, &got) != ESP_OK) {
                ESP_LOGE(TAG, "songbook_read failed @%lu", (unsigned long)file_off);
                break;
            }
            file_off += got;
            bytes_left += (int)got;
            if (got == 0) {
                eof = true;
            }
        }
        if (bytes_left == 0) {
            break; /* eof 且耗尽 */
        }

        int offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (offset < 0) {
            if (eof) {
                break;
            }
            /* 同步字可能跨读边界，保留尾部 3 字节 */
            int keep = bytes_left >= 3 ? 3 : bytes_left;
            memmove(s_inbuf, read_ptr + bytes_left - keep, (size_t)keep);
            read_ptr = s_inbuf;
            bytes_left = keep;
            continue;
        }
        read_ptr += offset;
        bytes_left -= offset;

        int err = MP3Decode(s_dec, &read_ptr, &bytes_left, s_outbuf, 0);
        if (err == ERR_MP3_NONE) {
            MP3GetLastFrameInfo(s_dec, &fi);
            if (!clock_set && fi.samprate > 0) {
                if (spk_set_clock((uint32_t)fi.samprate) == ESP_OK) {
                    clock_set = true;
                }
            }
            if (fi.outputSamps > 0) {
                output_frame(&fi);
            }
        } else if (err == ERR_MP3_INDATA_UNDERFLOW) {
            if (eof) {
                break; /* 尾帧数据不足 */
            } /* else 下轮 refill */
        } else {
            /* 坏帧：丢 1 字节重新找同步 */
            if (bytes_left > 0) {
                read_ptr++;
                bytes_left--;
            } else if (eof) {
                break;
            }
        }
    }

    if (!s_req_stop && s_req_index != index) {
        ESP_LOGI(TAG, "slot %d interrupted by switch", index);
    }
}

static void player_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_play_sem, portMAX_DELAY);
        while (s_req_index >= 0) {
            int idx = s_req_index;
            s_state = PL_PLAYING;
            run_song(idx);
            s_state = PL_IDLE;
            if (s_req_stop) {
                s_req_stop = false;
                s_req_index = -1;
            } else if (s_req_index == idx) {
                s_req_index = -1; /* 自然播完 */
            }
            /* s_req_index 换成别的歌：循环继续，直接接下一首 */
        }
    }
}

esp_err_t player_init(void)
{
    s_dec = MP3InitDecoder();
    if (s_dec == NULL) {
        ESP_LOGE(TAG, "MP3InitDecoder failed");
        return ESP_ERR_NO_MEM;
    }

    /* 扬声器 MAX98357A：I2S1 TX，飞利浦 16bit 立体声，默认 48kHz */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 512;
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "tx new");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = 0, .bclk_inv = 0, .ws_inv = 0 },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "tx std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx enable");

    s_play_sem = xSemaphoreCreateBinary();
    if (s_play_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(player_task, "player", 8192, NULL, 20, NULL, 1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready (Helix decoder + I2S1 @%d Hz)", DEFAULT_RATE_HZ);
    return ESP_OK;
}

esp_err_t player_play_index(int index)
{
    if (recorder_is_recording()) {
        ESP_LOGW(TAG, "recording in progress, play rejected");
        return ESP_ERR_INVALID_STATE;
    }
    if (songbook_get_title(index) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_req_stop = false;
    s_req_index = index;
    xSemaphoreGive(s_play_sem);
    return ESP_OK;
}

esp_err_t player_pause_toggle(void)
{
    if (s_state == PL_PLAYING) {
        s_state = PL_PAUSED;
    } else if (s_state == PL_PAUSED) {
        s_state = PL_PLAYING;
    } else {
        /* 空闲：按压即开始播放当前曲目（方案：旋钮按压 = 播放/暂停歌曲）。
         * 之前此处直接返回 ESP_ERR_INVALID_STATE，导致开机后按压旋钮毫无反应 */
        int idx = songbook_current();
        if (idx < 0) {
            ESP_LOGW(TAG, "idle press: songbook empty, nothing to play");
            return ESP_ERR_INVALID_STATE;
        }
        return player_play_index(idx);
    }
    return ESP_OK;
}

esp_err_t player_stop(void)
{
    if (s_state == PL_IDLE && s_req_index < 0) {
        return ESP_OK;
    }
    s_req_stop = true;
    xSemaphoreGive(s_play_sem); /* 防空闲等待时叫不醒 */
    for (int i = 0; i < 50 && s_state != PL_IDLE; i++) {
        vTaskDelay(pdMS_TO_TICKS(10)); /* 等解码任务退出，≤500ms */
    }
    return s_state == PL_IDLE ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool player_is_playing(void)
{
    return s_state == PL_PLAYING;
}

esp_err_t player_set_volume(uint8_t vol)
{
    if (vol > 100) {
        vol = 100;
    }
    s_volume = vol;
    s_vol_q15 = (int32_t)vol * 32767 / 100;
    return ESP_OK;
}

esp_err_t player_spk_reconfig_clock(uint32_t sample_rate)
{
    if (s_state != PL_IDLE) {
        return ESP_ERR_INVALID_STATE; /* 仅播放器空闲时可借喇叭 */
    }
    return spk_set_clock(sample_rate);
}

esp_err_t player_spk_write(const void *buf, size_t len, uint32_t timeout_ms)
{
    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_tx, buf, len, &written, pdMS_TO_TICKS(timeout_ms));
    return err;
}
