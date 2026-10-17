#include "power.h"

#include "board_pins.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 下游命令脚：ESP32 侧驱动、指向被供电外设的输出脚 */
static const int s_downstream_cmd_pins[] = {
    PIN_LED_DATA,
    PIN_I2S_BCLK,
    PIN_I2S_WS,
    PIN_I2S_DOUT,
};
#define NUM_DOWNSTREAM  (sizeof(s_downstream_cmd_pins) / sizeof(s_downstream_cmd_pins[0]))

esp_err_t power_up(void)
{
    esp_err_t err;

    /* 步骤 1：GPIO8 与下游命令脚全部锁安全态（输出低） */
    uint64_t mask = 1ULL << PIN_POWER_EN;
    for (unsigned i = 0; i < NUM_DOWNSTREAM; i++) {
        mask |= 1ULL << s_downstream_cmd_pins[i];
    }
    gpio_config_t out = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&out);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(PIN_POWER_EN, 0);
    for (unsigned i = 0; i < NUM_DOWNSTREAM; i++) {
        gpio_set_level(s_downstream_cmd_pins[i], 0);
    }

    /* 麦克风脚不初始化，仅显式浮空、不上下拉（GPIO11 硬性要求） */
    gpio_config_t mic = {
        .pin_bit_mask = (1ULL << PIN_MIC_0) | (1ULL << PIN_MIC_1) | (1ULL << PIN_MIC_2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&mic);
    if (err != ESP_OK) {
        return err;
    }

    /* 步骤 2：拉高 GPIO8 */
    gpio_set_level(PIN_POWER_EN, 1);

    /* 步骤 3：等待稳定（POWER_STABLE_MS，项目值，待实板测量确认）。初始化路径，允许 vTaskDelay */
    vTaskDelay(pdMS_TO_TICKS(POWER_STABLE_MS));

    /* 步骤 4：返回成功 */
    return ESP_OK;
}
