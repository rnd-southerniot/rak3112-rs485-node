#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "gpio_remap.h"

static const char *TAG = "app";

/*
 * Hold the V1.2-reserved I2C nets (GPIO9/GPIO40) as deliberate-floating on V1.1:
 * mode disabled (no input/output drive), both internal pulls off → high-Z.
 *
 * Required by hardware ADR-001 EC-4/EC-9 (Phase 2 carry-forward): these pads are
 * routed-but-unterminated on V1.1; left at reset defaults they could draw
 * undefined current or cause spurious ULP wakeup. When V1.2 boards add the I2C
 * expansion header (#1), this becomes I2C peripheral init instead.
 */
static void hold_reserved_pins_floating(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_I2C1_SDA_RESERVED) | (1ULL << PIN_I2C1_SCL_RESERVED),
        .mode = GPIO_MODE_DISABLE,         /* no drive */
        .pull_up_en = GPIO_PULLUP_DISABLE, /* no internal pull */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_LOGI(TAG, "reserved pins GPIO%d/GPIO%d held floating (V1.2 I2C placeholder)",
             PIN_I2C1_SDA_RESERVED, PIN_I2C1_SCL_RESERVED);
}

void app_main(void)
{
    hold_reserved_pins_floating();

    for (uint32_t i = 0;; ++i) {
        printf("rak3112-rs485-node alive: tick=%lu\n", (unsigned long)i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
