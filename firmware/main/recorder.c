#include "recorder.h"

#include <string.h>

#include "board_pins.h"
#include "player.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "recorder";

/* ---- 介质布局 ---- */
#define PART_SUBTYPE   ((esp_partition_subtype_t)0x40)
#define REC_DATA_OFF   0x1000u /* PCM 从 4KB 边界开始，头独占首扇区 */
#define FLASH_SECTOR   0x1000u
#define REC_HDR_MAGIC  0x4D425243u /* "MBRC" */
#define REC_HDR_VER    1u

/* ---- PCM 格式 ---- */
#define REC_RATE_HZ    16000
#define REC_MAX_SEC    30
#define MIC_BLOCK      512 /* 每次读的帧数（32bit/帧 → 2KB raw） */

typedef struct {
    uint32_t magic;       /* REC_HDR_MAGIC */
    uint32_t ver;         /* REC_HDR_VER */
    uint32_t sample_rate; /* REC_RATE_HZ */
    uint32_t frames;      /* PCM 帧数（16bit/mono，字节数 = frames*2） */
    uint32_t crc32;       /* PCM 数据 CRC32（esp_rom_crc32_le，初值 0） */
    uint32_t reserved[11];
} rec_header_t;
_Static_assert(sizeof(rec_header_t) == 64, "rec header must stay 64B");

typedef enum {
    REC_IDLE = 0,
    REC_RECORDING,
    REC_FULL, /* 缓冲录满自动停，等 stop 落盘 */
} rec_state_t;

static const esp_partition_t *s_part;
static i2s_chan_handle_t s_rx;
static TaskHandle_t s_mic_task;
static SemaphoreHandle_t s_mic_done; /* mic 任务退出录音循环时给出 */

static volatile rec_state_t s_rec_state = REC_IDLE;
static int16_t *s_pcm;                 /* 录音期分配的 PSRAM 缓冲 */
static volatile uint32_t s_frames;     /* 已录帧数 */
static uint32_t s_max_frames;          /* 缓冲容量（帧） */

/* 最近一段录音的持久化信息（init 从头恢复） */
static uint32_t s_last_frames;
static uint32_t s_last_crc;

/* 录音回放状态 */
static volatile bool s_replay;
static volatile bool s_replay_stop;

/* ---------------- 麦克风采集 ---------------- */

static void mic_task(void *arg)
{
    static int32_t raw[MIC_BLOCK];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* 等 recorder_start 叫醒 */
        while (s_rec_state == REC_RECORDING) {
            size_t got = 0;
            esp_err_t err = i2s_channel_read(s_rx, raw, sizeof(raw), &got,
                                             pdMS_TO_TICKS(100));
            if (err != ESP_OK || got == 0) {
                continue; /* 超时重读，state 变化即退出 */
            }
            uint32_t n = (uint32_t)(got / sizeof(int32_t));
            uint32_t room = s_max_frames - s_frames;
            if (n > room) {
                n = room;
            }
            int16_t *dst = s_pcm + s_frames;
            for (uint32_t i = 0; i < n; i++) {
                dst[i] = (int16_t)(raw[i] >> 16); /* 32bit MSB 槽取高 16 位 */
            }
            s_frames += n;
            if (s_frames >= s_max_frames) {
                s_rec_state = REC_FULL;
                ESP_LOGW(TAG, "buffer full (%lu frames), auto stop",
                         (unsigned long)s_frames);
            }
        }
        xSemaphoreGive(s_mic_done);
    }
}

/* ---------------- 落盘 ---------------- */

static esp_err_t save_to_flash(void)
{
    uint32_t bytes = s_frames * sizeof(int16_t);
    if (bytes == 0) {
        ESP_LOGW(TAG, "nothing recorded, skip save");
        return ESP_ERR_INVALID_SIZE;
    }
    if (bytes > s_part->size - REC_DATA_OFF) {
        bytes = s_part->size - REC_DATA_OFF; /* 理论不可达（容量已按分区算），兜底 */
    }

    /* 擦头扇区 + 数据扇区（1MB 全量擦约数秒，属停录路径可接受） */
    uint32_t erase_len = REC_DATA_OFF + ((bytes + FLASH_SECTOR - 1) & ~(FLASH_SECTOR - 1));
    ESP_LOGI(TAG, "saving %lu B, erasing %lu B...", (unsigned long)bytes,
             (unsigned long)erase_len);
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_part, 0, erase_len), TAG, "erase");

    /* 顺序写：PSRAM → 4KB 内部 bounce → flash */
    static uint8_t bounce[FLASH_SECTOR];
    uint32_t crc = 0;
    uint32_t off = 0;
    while (off < bytes) {
        uint32_t n = bytes - off;
        if (n > sizeof(bounce)) {
            n = sizeof(bounce);
        }
        memcpy(bounce, (const uint8_t *)s_pcm + off, n);
        ESP_RETURN_ON_ERROR(esp_partition_write(s_part, REC_DATA_OFF + off, bounce, n),
                            TAG, "write @%lu", (unsigned long)off);
        crc = esp_rom_crc32_le(crc, bounce, n);
        off += n;
    }

    /* 头最后写：写数据期间掉电时旧头无效，不会把半段录音当有效 */
    rec_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = REC_HDR_MAGIC;
    h.ver = REC_HDR_VER;
    h.sample_rate = REC_RATE_HZ;
    h.frames = bytes / sizeof(int16_t);
    h.crc32 = crc;
    ESP_RETURN_ON_ERROR(esp_partition_write(s_part, 0, &h, sizeof(h)), TAG, "write hdr");

    s_last_frames = h.frames;
    s_last_crc = crc;
    ESP_LOGI(TAG, "saved: %lu frames (%lu.%01lu s), crc %08lx",
             (unsigned long)h.frames, (unsigned long)(h.frames / REC_RATE_HZ),
             (unsigned long)((h.frames % REC_RATE_HZ) * 10 / REC_RATE_HZ),
             (unsigned long)crc);
    return ESP_OK;
}

/* ---------------- 对外接口 ---------------- */

esp_err_t recorder_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PART_SUBTYPE, "rec");
    if (s_part == NULL) {
        ESP_LOGE(TAG, "rec partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t payload_frames = (s_part->size - REC_DATA_OFF) / sizeof(int16_t);
    s_max_frames = payload_frames < (uint32_t)(REC_RATE_HZ * REC_MAX_SEC)
                       ? payload_frames
                       : (uint32_t)(REC_RATE_HZ * REC_MAX_SEC);
    ESP_LOGI(TAG, "partition @0x%lx size 0x%lx, cap %lu frames (%lu s)",
             (unsigned long)s_part->address, (unsigned long)s_part->size,
             (unsigned long)s_max_frames, (unsigned long)(s_max_frames / REC_RATE_HZ));

    /* 读头恢复最近录音信息（CRC 留到 M3 需要时再校验） */
    rec_header_t h;
    if (esp_partition_read(s_part, 0, &h, sizeof(h)) == ESP_OK
        && h.magic == REC_HDR_MAGIC && h.ver == REC_HDR_VER
        && h.frames > 0 && h.frames <= payload_frames) {
        s_last_frames = h.frames;
        s_last_crc = h.crc32;
        ESP_LOGI(TAG, "last recording: %lu frames", (unsigned long)s_last_frames);
    } else {
        ESP_LOGI(TAG, "no valid recording yet");
    }

    /* 麦克风：I2S0 RX，STD MSB 32bit 单声道右槽，16kHz。GPIO11 仅作输入，永不驱动 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = MIC_BLOCK;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "rx new");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(REC_RATE_HZ),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                    I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_0,
            .ws = PIN_MIC_1,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_MIC_2,
            .invert_flags = { .mclk_inv = 0, .bclk_inv = 0, .ws_inv = 0 },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "rx std");
    /* 不 enable：recorder_start 才开，平时麦时钟静默 */

    s_mic_done = xSemaphoreCreateBinary();
    if (s_mic_done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 21,
                                            &s_mic_task, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t recorder_start(void)
{
    if (s_rec_state != REC_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 与出声互斥：先停歌曲播放与录音回放，防回授。
     * player_stop 幂等且能停暂停态；不能用 player_is_playing() 看门——
     * PL_PAUSED 会漏网，喇叭被 player_task 占着，之后借喇叭被判 busy */
    (void)player_stop();
    recorder_stop_play();

    if (s_pcm == NULL) {
        s_pcm = heap_caps_malloc(s_max_frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (s_pcm == NULL) {
            ESP_LOGE(TAG, "PSRAM alloc %lu B failed",
                     (unsigned long)(s_max_frames * sizeof(int16_t)));
            return ESP_ERR_NO_MEM;
        }
    }
    s_frames = 0;
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "rx enable");
    s_rec_state = REC_RECORDING;
    xTaskNotifyGive(s_mic_task);
    ESP_LOGI(TAG, "recording start (max %lu s)",
             (unsigned long)(s_max_frames / REC_RATE_HZ));
    return ESP_OK;
}

esp_err_t recorder_stop(void)
{
    if (s_rec_state == REC_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_rec_state = REC_IDLE;
    /* 等 mic 任务退出读循环（i2s 读超时 100ms，给足余量） */
    if (xSemaphoreTake(s_mic_done, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "mic task exit timeout");
    }
    (void)i2s_channel_disable(s_rx);

    esp_err_t err = save_to_flash();
    heap_caps_free(s_pcm);
    s_pcm = NULL;
    return err;
}

bool recorder_is_recording(void)
{
    return s_rec_state != REC_IDLE;
}

/* ---------------- 最近录音回放（借播放器 I2S1 通道） ---------------- */

static void replay_task(void *arg)
{
    static int16_t pcm_chunk[1024];
    static int16_t spk_chunk[2048]; /* 单声道复制成双声道 */

    if (player_spk_reconfig_clock(REC_RATE_HZ) == ESP_OK) {
        uint32_t total = s_last_frames * sizeof(int16_t);
        uint32_t off = 0;
        while (off < total && !s_replay_stop) {
            uint32_t n = total - off;
            if (n > sizeof(pcm_chunk)) {
                n = sizeof(pcm_chunk);
            }
            if (esp_partition_read(s_part, REC_DATA_OFF + off, pcm_chunk, n) != ESP_OK) {
                ESP_LOGE(TAG, "replay read failed @%lu", (unsigned long)off);
                break;
            }
            off += n;
            uint32_t frames = n / sizeof(int16_t);
            for (uint32_t i = 0; i < frames; i++) {
                spk_chunk[2 * i] = pcm_chunk[i];
                spk_chunk[2 * i + 1] = pcm_chunk[i];
            }
            if (player_spk_write(spk_chunk, frames * 2 * sizeof(int16_t), 1000)
                    != ESP_OK) {
                break;
            }
        }
        /* 恢复默认时钟（播放器起播时还会按 MP3 帧头再配，这里只是复位到已知态） */
        (void)player_spk_reconfig_clock(48000);
    } else {
        ESP_LOGW(TAG, "speaker busy, replay aborted");
    }
    s_replay = false;
    vTaskDelete(NULL);
}

esp_err_t recorder_play_last(void)
{
    if (s_rec_state != REC_IDLE || s_replay) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_last_frames == 0) {
        ESP_LOGW(TAG, "no recording to play");
        return ESP_ERR_NOT_FOUND;
    }
    (void)player_stop(); /* 喇叭独占；幂等，暂停态(PL_PAUSED)也能停 */
    s_replay_stop = false;
    s_replay = true;
    BaseType_t ok = xTaskCreatePinnedToCore(replay_task, "recplay", 4096, NULL, 20,
                                            NULL, 1);
    if (ok != pdPASS) {
        s_replay = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "replay last recording (%lu frames)", (unsigned long)s_last_frames);
    return ESP_OK;
}

bool recorder_is_playing(void)
{
    return s_replay;
}

void recorder_stop_play(void)
{
    if (!s_replay) {
        return;
    }
    s_replay_stop = true;
    for (int i = 0; i < 100 && s_replay; i++) {
        vTaskDelay(pdMS_TO_TICKS(10)); /* 等回放任务退出，≤1s */
    }
}

/* ---------------- M4 读侧（rec_pull 上传用） ---------------- */

esp_err_t recorder_last_meta(uint32_t *frames, uint32_t *rate)
{
    if (s_last_frames == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (frames) {
        *frames = s_last_frames;
    }
    if (rate) {
        *rate = REC_RATE_HZ;
    }
    return ESP_OK;
}

esp_err_t recorder_read(uint32_t offset, uint8_t *buf, uint32_t len, uint32_t *got)
{
    if (got) {
        *got = 0;
    }
    if (s_rec_state != REC_IDLE) {
        return ESP_ERR_INVALID_STATE; /* 录音进行中，分区数据尚不稳定 */
    }
    if (buf == NULL || s_last_frames == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t total = s_last_frames * sizeof(int16_t);
    if (offset >= total) {
        return ESP_OK; /* 已到尾，got=0 */
    }
    if (offset + len > total) {
        len = total - offset; /* 按实际录音字节数截断 */
    }
    esp_err_t err = esp_partition_read(s_part, REC_DATA_OFF + offset, buf, len);
    if (err == ESP_OK && got) {
        *got = len;
    }
    return err;
}
