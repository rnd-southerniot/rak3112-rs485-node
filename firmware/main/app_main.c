#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "gpio_remap.h"
#include "lora.h"

static const char *TAG = "app";

/* GPIO9/GPIO40 held deliberate-floating on V1.1 (ADR-001 EC-4/EC-9; Phase 3 carry-forward). */
static void hold_reserved_pins_floating(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_I2C1_SDA_RESERVED) | (1ULL << PIN_I2C1_SCL_RESERVED),
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

void app_main(void)
{
    hold_reserved_pins_floating();
    ESP_ERROR_CHECK(lora_init());

    /* OTAA join with a few retries (do NOT abort/bootloop on RF failure during bring-up). */
    int joined = 0;
    for (int attempt = 1; attempt <= 5 && !joined; ++attempt) {
        ESP_LOGI(TAG, "join attempt %d/5", attempt);
        if (lora_join() == ESP_OK) {
            joined = 1;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }
    if (!joined) {
        ESP_LOGE(TAG, "OTAA join failed after retries — halting (check antenna/TCXO/keys)");
        for (;;)
            vTaskDelay(pdMS_TO_TICKS(10000));
    }

    /* Periodic uplink (60 s; AS923 dwell + fair-use friendly). */
    for (uint32_t n = 0;; ++n) {
        const uint8_t payload[4] = {0xA5, (uint8_t)(n >> 8), (uint8_t)n, 0x5A};
        lora_send(payload, sizeof(payload));
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
