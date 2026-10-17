#include "encoder.h"

#include "board_pins.h"

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SHORT_PRESS_MAX_MS 800
#define ENC_RAW_PER_DETENT 4 /* 4x 正交解码，每档 4 个原始计数 */

static pcnt_unit_handle_t s_unit;
static int s_last_raw;
static int s_accum;
static volatile int s_detents;

static uint8_t s_sw_state;
static uint8_t s_sw_cnt;
static uint32_t s_sw_down_ms;
static volatile bool s_sw_short;

void encoder_init(void)
{
    pcnt_unit_config_t ucfg = {
        .low_limit = -1024,
        .high_limit = 1024,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&ucfg, &s_unit));

    pcnt_chan_config_t acfg = {
        .edge_gpio_num = PIN_ENC_A,
        .level_gpio_num = PIN_ENC_B,
    };
    pcnt_channel_handle_t cha;
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &acfg, &cha));

    pcnt_chan_config_t bcfg = {
        .edge_gpio_num = PIN_ENC_B,
        .level_gpio_num = PIN_ENC_A,
    };
    pcnt_channel_handle_t chb;
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &bcfg, &chb));

    /* 4x 正交：A/B 双边沿计数，方向由另一相电平决定 */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(cha,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(cha,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chb,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chb,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    pcnt_glitch_filter_config_t fcfg = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_unit, &fcfg));

    gpio_set_pull_mode(PIN_ENC_A, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_ENC_B, GPIO_PULLUP_ONLY);

    ESP_ERROR_CHECK(pcnt_unit_enable(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_unit));

    /* 按压脚：低有效，上拉输入 */
    gpio_config_t sw = {
        .pin_bit_mask = 1ULL << PIN_ENC_SW,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&sw);
}

void encoder_poll(void)
{
    int raw = 0;
    pcnt_unit_get_count(s_unit, &raw);
    int delta = raw - s_last_raw;
    s_last_raw = raw;

    s_accum += delta;
    while (s_accum >= ENC_RAW_PER_DETENT) {
        s_accum -= ENC_RAW_PER_DETENT;
        s_detents++;
    }
    while (s_accum <= -ENC_RAW_PER_DETENT) {
        s_accum += ENC_RAW_PER_DETENT;
        s_detents--;
    }

    int pressed = (gpio_get_level(PIN_ENC_SW) == 0);
    if (pressed != s_sw_state) {
        if (++s_sw_cnt >= 2) {
            s_sw_state = (uint8_t)pressed;
            s_sw_cnt = 0;
            if (pressed) {
                s_sw_down_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            } else {
                uint32_t held = xTaskGetTickCount() * portTICK_PERIOD_MS - s_sw_down_ms;
                if (held < SHORT_PRESS_MAX_MS) {
                    s_sw_short = true;
                }
            }
        }
    } else {
        s_sw_cnt = 0;
    }
}

bool encoder_short_press(void)
{
    bool e = s_sw_short;
    s_sw_short = false;
    return e;
}

int encoder_delta_detents(void)
{
    int d = s_detents;
    s_detents = 0;
    return d;
}
