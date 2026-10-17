#include "buttons.h"

#include "board_pins.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    int pin;
    uint8_t state;          /* 0=松开 1=按下（消抖后） */
    uint8_t cnt;            /* 去抖计数 */
    uint32_t down_ms;       /* 按下时刻 */
    bool long_active;       /* 已进入长按态 */
    volatile bool ev_short; /* 短按事件 */
    volatile bool ev_long_start;
    volatile bool ev_long_end;
} key_t;

/* S1..S8 自然顺序 */
static key_t s_keys[BUTTONS_COUNT] = {
    { PIN_S1, 0, 0, 0, false, false, false, false },
    { PIN_S2, 0, 0, 0, false, false, false, false },
    { PIN_S3, 0, 0, 0, false, false, false, false },
    { PIN_S4, 0, 0, 0, false, false, false, false },
    { PIN_S5, 0, 0, 0, false, false, false, false },
    { PIN_S6, 0, 0, 0, false, false, false, false },
    { PIN_S7, 0, 0, 0, false, false, false, false },
    { PIN_S8, 0, 0, 0, false, false, false, false },
};

void buttons_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BUTTONS_COUNT; i++) {
        mask |= 1ULL << s_keys[i].pin;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

void buttons_poll(void)
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    for (int i = 0; i < BUTTONS_COUNT; i++) {
        key_t *k = &s_keys[i];
        int pressed = (gpio_get_level(k->pin) == 0);

        if (pressed != k->state) {
            if (++k->cnt >= 2) { /* 20ms 去抖 */
                k->state = (uint8_t)pressed;
                k->cnt = 0;
                if (pressed) {
                    k->down_ms = now;
                    k->long_active = false;
                    if (i != BTN_S3) {
                        /* 非 S3：按下沿即时给短按事件 */
                        k->ev_short = true;
                    }
                } else {
                    if (i == BTN_S3) {
                        if (k->long_active) {
                            k->ev_long_end = true;
                            k->long_active = false;
                        } else {
                            /* 未达长按时长的点按：也给一次短按（当前业务未用） */
                            k->ev_short = true;
                        }
                    }
                }
            }
        } else {
            k->cnt = 0;
        }

        /* S3 长按进入判定：按住达到阈值且尚未进入长按态 */
        if (i == BTN_S3 && k->state && !k->long_active &&
            now - k->down_ms >= BUTTONS_LONG_PRESS_MS) {
            k->long_active = true;
            k->ev_long_start = true;
        }
    }
}

bool buttons_level(uint8_t idx)
{
    if (idx >= BUTTONS_COUNT) {
        return false;
    }
    return s_keys[idx].state != 0;
}

static bool take(volatile bool *e)
{
    bool v = *e;
    *e = false;
    return v;
}

bool buttons_short_press(uint8_t idx)
{
    return idx < BUTTONS_COUNT && take(&s_keys[idx].ev_short);
}

bool buttons_long_start(uint8_t idx)
{
    return idx < BUTTONS_COUNT && take(&s_keys[idx].ev_long_start);
}

bool buttons_long_end(uint8_t idx)
{
    return idx < BUTTONS_COUNT && take(&s_keys[idx].ev_long_end);
}
