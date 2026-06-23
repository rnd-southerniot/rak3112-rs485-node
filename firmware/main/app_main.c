#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "gpio_remap.h"
#include "lora.h"
#if CONFIG_APP_MODBUS_SCAN_ON_BOOT || CONFIG_APP_MODBUS_POLL_ON_BOOT
#include "modbus_master.h"
#include "rs485.h"
#endif

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

#if CONFIG_APP_MODBUS_SCAN_ON_BOOT
/*
 * Bench bring-up: sweep common baud rates (starting at 9600 8N1) scanning Modbus unit IDs on the
 * node's RS-485 (CN1 / TP8485E). Stops at the first baud that finds a device. A valid reply OR a
 * Modbus exception both count as present; only a timeout is "absent" (unit ID 0 = broadcast is
 * skipped by the configured range). Parity sweep (8E1/8O1) needs an rs485 transport extension —
 * tracked for a follow-up; this covers 8N1, the common industrial default.
 */
static void run_modbus_scan(void)
{
    /* 4800 prioritised (operator hint); 8E1 first (common industrial default, top suspect). */
    static const int bauds[] = {9600, 4800, 19200, 38400, 57600, 115200};
    static const struct {
        uart_parity_t parity;
        const char *label;
    } framings[] = {
        {UART_PARITY_EVEN, "8E1"},
        {UART_PARITY_DISABLE, "8N1"},
        {UART_PARITY_ODD, "8O1"},
    };
    const uint8_t id_lo = (uint8_t)CONFIG_APP_MODBUS_SCAN_ID_LO;
    const uint8_t id_hi = (uint8_t)CONFIG_APP_MODBUS_SCAN_ID_HI;
    const uint32_t timeout_ms = (uint32_t)CONFIG_APP_MODBUS_SCAN_TIMEOUT_MS;
    const uint16_t probe_addr = 0x0000;

    ESP_LOGI(TAG,
             "=== Modbus RTU bus scan (CN1) — ids %u-%u x baud x {8N1,8E1,8O1}, FC03 @0x%04X ===",
             (unsigned)id_lo, (unsigned)id_hi, (unsigned)probe_addr);
    int total = 0;
    for (size_t f = 0; f < sizeof(framings) / sizeof(framings[0]) && total == 0; ++f) {
        for (size_t b = 0; b < sizeof(bauds) / sizeof(bauds[0]); ++b) {
            if (uart_is_driver_installed(UART_NUM_1)) {
                uart_driver_delete(UART_NUM_1);
            }
            const rs485_config_t cfg = {
                .port = UART_NUM_1,
                .tx_gpio = PIN_RS485_TX,
                .rx_gpio = PIN_RS485_RX,
                .de_re_gpio = PIN_RS485_DE_RE,
                .baud_rate = bauds[b],
                .parity = framings[f].parity,
                .rx_buffer_size = 256,
            };
            if (rs485_init(&cfg) != ESP_OK) {
                ESP_LOGE(TAG, "rs485_init @%d %s failed", bauds[b], framings[f].label);
                continue;
            }
            const int found = modbus_master_scan(UART_NUM_1, id_lo, id_hi, probe_addr, timeout_ms,
                                                 bauds[b], framings[f].label);
            total += found;
            if (found > 0) {
                ESP_LOGI(TAG, "*** device(s) at %d %s — stopping sweep ***", bauds[b],
                         framings[f].label);
                break;
            }
        }
    }
    ESP_LOGI(TAG, "=== scan complete: %d device(s) found ===", total);
    if (total == 0) {
        ESP_LOGW(TAG,
                 "no devices across baud x {8N1,8E1,8O1}, ids %u-%u — check A/B polarity, GND, "
                 "device power + RTU mode, 120R termination; widen ID range / try 1200,2400.",
                 (unsigned)id_lo, (unsigned)id_hi);
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000)); /* hold; scan result stays on console */
    }
}
#endif /* CONFIG_APP_MODBUS_SCAN_ON_BOOT */

#if CONFIG_APP_MODBUS_POLL_ON_BOOT
/* Bench bring-up: poll one FC03 holding register at 1 Hz on CN1 and print the value (or
 * timeout/exception). Lets you swap A/B + verify wiring against a known device and watch it come
 * alive live. Fixed 8N1 (matches RS-FSJT). Does not return. */
static void run_modbus_poll(void)
{
    const rs485_config_t cfg = {
        .port = UART_NUM_1,
        .tx_gpio = PIN_RS485_TX,
        .rx_gpio = PIN_RS485_RX,
        .de_re_gpio = PIN_RS485_DE_RE,
        .baud_rate = CONFIG_APP_MODBUS_POLL_BAUD,
        .parity = UART_PARITY_DISABLE,
        .rx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(rs485_init(&cfg));
    const uint8_t unit = (uint8_t)CONFIG_APP_MODBUS_POLL_UNIT;
    const uint16_t reg = (uint16_t)CONFIG_APP_MODBUS_POLL_REG;
    ESP_LOGI(TAG, "Modbus poll: unit %u, FC03 reg %u @ %d 8N1, 1 Hz — swap A/B if it times out",
             (unsigned)unit, (unsigned)reg, CONFIG_APP_MODBUS_POLL_BAUD);
    for (uint32_t n = 0;; ++n) {
        uint16_t val = 0;
        uint8_t exc = 0;
        const modbus_status_t st = modbus_master_read(
            UART_NUM_1, unit, MODBUS_FC_READ_HOLDING_REGISTERS, reg, 1, 500, &val, &exc);
        if (st == MODBUS_OK) {
            ESP_LOGI(TAG, "[%lu] reg%u = %u (raw) = %u.%u (x0.1)", (unsigned long)n, (unsigned)reg,
                     (unsigned)val, (unsigned)(val / 10), (unsigned)(val % 10));
        } else if (st == MODBUS_ERR_EXCEPTION) {
            ESP_LOGW(TAG, "[%lu] exception 0x%02X (device answered)", (unsigned long)n,
                     (unsigned)exc);
        } else {
            ESP_LOGW(TAG, "[%lu] timeout (st=%d) — no reply; try swapping A/B, check GND",
                     (unsigned long)n, (int)st);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif /* CONFIG_APP_MODBUS_POLL_ON_BOOT */

void app_main(void)
{
    hold_reserved_pins_floating();

#if CONFIG_APP_MODBUS_POLL_ON_BOOT
    run_modbus_poll(); /* bench bring-up mode — does not return */
#elif CONFIG_APP_MODBUS_SCAN_ON_BOOT
    run_modbus_scan(); /* bench bring-up mode — does not return */
#endif

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
