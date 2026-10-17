#include "led_status.h"

#include <string.h>

#include "board_pins.h"
#include "driver/rmt_tx.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define FRAME_MIN_US 20000 /* 20ms 帧限 */

/* 占位亮度（低亮度，实板再调） */
#define REC_R        80
#define CHASE_R      60
#define CHASE_G      45
#define DONE_G       90

#define DONE_FLASHES    3
#define DONE_BLINK_MS   220  /* 单次亮/灭时长 */
#define CHASE_STEP_MS   120  /* 跑马灯步进 */

/* 迪斯科：换色间隔随机 70~210ms，亮度随机 90~209（占位值，实板再调） */
#define DISCO_MIN_MS    70
#define DISCO_JITTER_MS 140
#define DISCO_MIN_LVL   90
#define DISCO_JITTER_LVL 120

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static uint8_t s_frame[LED_COUNT * 3]; /* GRB 顺序 */

static led_mode_t s_mode = LED_MODE_IDLE;
static int64_t s_mode_enter_ms;
static int64_t s_disco_next_ms; /* 迪斯科下次换色时刻（相对模式进入） */

void led_status_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = PIN_LED_DATA,
        .mem_block_symbols = 64,
        .resolution_hz = 10 * 1000 * 1000, /* 0.1us 分辨率 */
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&tx_cfg, &s_chan) != ESP_OK) {
        s_chan = NULL;
        return;
    }

    rmt_bytes_encoder_config_t bytes_cfg = {
        /* WS2812: T0H=0.3us T0L=0.9us / T1H=0.9us T1L=0.3us */
        .bit0 = { .duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0 },
        .bit1 = { .duration0 = 9, .level0 = 1, .duration1 = 3, .level1 = 0 },
        .flags.msb_first = 1,
    };
    if (rmt_new_bytes_encoder(&bytes_cfg, &s_enc) != ESP_OK) {
        s_enc = NULL;
        return;
    }

    rmt_enable(s_chan);
}

void led_status_set_mode(led_mode_t mode)
{
    s_mode = mode;
    s_mode_enter_ms = esp_timer_get_time() / 1000;
    s_disco_next_ms = 0; /* 重新进入任意模式时重置迪斯科计时，保证立刻换一次色 */
}

led_mode_t led_status_mode(void)
{
    return s_mode;
}

static void fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < LED_COUNT; i++) {
        s_frame[i * 3 + 0] = g;
        s_frame[i * 3 + 1] = r;
        s_frame[i * 3 + 2] = b;
    }
}

void led_status_poll(void)
{
    if (!s_chan || !s_enc) {
        return;
    }

    static int64_t last_us;
    int64_t now = esp_timer_get_time();
    if (now - last_us < FRAME_MIN_US) {
        return;
    }
    last_us = now;

    int64_t elapsed_ms = now / 1000 - s_mode_enter_ms;

    switch (s_mode) {
    case LED_MODE_RECORDING:
        fill(REC_R, 0, 0); /* 全红常亮 */
        break;

    case LED_MODE_GENERATING: {
        /* 跑马灯：单亮点循环旋转（占位暖黄色） */
        fill(0, 0, 0);
        int pos = (int)((elapsed_ms / CHASE_STEP_MS) % LED_COUNT);
        s_frame[pos * 3 + 0] = CHASE_G;
        s_frame[pos * 3 + 1] = CHASE_R;
        break;
    }

    case LED_MODE_DONE: {
        /* 绿闪 DONE_FLASHES 次后自动回 IDLE */
        int phase = (int)(elapsed_ms / DONE_BLINK_MS); /* 偶数=亮 奇数=灭 */
        if (phase >= DONE_FLASHES * 2) {
            s_mode = LED_MODE_IDLE;
            fill(0, 0, 0);
        } else if (phase % 2 == 0) {
            fill(0, DONE_G, 0);
        } else {
            fill(0, 0, 0);
        }
        break;
    }

    case LED_MODE_PLAYING: {
        /* 迪斯科风格：每颗灯随机取色、按随机节奏整体换色，偶发全黑闪断 */
        static const uint8_t palette[][3] = { /* {R,G,B} 高饱和色板 */
            { 255, 0, 0 },   { 0, 255, 0 },   { 0, 0, 255 },
            { 255, 255, 0 }, { 255, 0, 255 }, { 0, 255, 255 },
            { 255, 255, 255 },
        };
        static uint8_t grb[LED_COUNT * 3];

        if (elapsed_ms >= s_disco_next_ms) {
            bool blackout = (esp_random() & 7u) == 0u; /* 1/8 概率整拍全黑 */
            for (int i = 0; i < LED_COUNT; i++) {
                if (blackout) {
                    grb[i * 3] = grb[i * 3 + 1] = grb[i * 3 + 2] = 0;
                    continue;
                }
                const uint8_t *c = palette[esp_random() % (sizeof(palette) / sizeof(palette[0]))];
                uint32_t lvl = DISCO_MIN_LVL + esp_random() % DISCO_JITTER_LVL; /* 随机亮度 */
                grb[i * 3 + 0] = (uint8_t)(c[1] * lvl / 255); /* G */
                grb[i * 3 + 1] = (uint8_t)(c[0] * lvl / 255); /* R */
                grb[i * 3 + 2] = (uint8_t)(c[2] * lvl / 255); /* B */
            }
            s_disco_next_ms = elapsed_ms + DISCO_MIN_MS + esp_random() % DISCO_JITTER_MS;
        }
        memcpy(s_frame, grb, sizeof(grb));
        break;
    }

    case LED_MODE_IDLE:
    default:
        fill(0, 0, 0);
        break;
    }

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    if (rmt_transmit(s_chan, s_enc, s_frame, LED_COUNT * 3, &tx_cfg) == ESP_OK) {
        rmt_tx_wait_all_done(s_chan, -1);
    }
}
