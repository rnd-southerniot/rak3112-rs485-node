#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "gpio_remap.h"
#include "rs485.h"
#include "ring_buffer.h"

static const char *TAG = "app";

/*
 * RS-485 echo baud (Phase 4 smoke gate runs 9600 first, then 115200).
 * Change here and re-flash for the second pass; ADR-002 documents the two-pass gate.
 */
#define RS485_ECHO_BAUD 9600

#define RS485_UART UART_NUM_1
#define RS485_RX_DRV_BUF 1024 /* ESP-IDF UART RX ring (>= 2*UART_HW_FIFO_LEN) */
#define ECHO_CHUNK 256
#define RX_STAGE_CAP 512

/*
 * Hold the V1.2-reserved I2C nets (GPIO9/GPIO40) as deliberate-floating on V1.1:
 * mode disabled (no drive), both internal pulls off → high-Z. Required by hardware
 * ADR-001 EC-4/EC-9 (Phase 2 carry-forward) to avoid undefined draw / spurious ULP wakeup.
 */
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
    ESP_LOGI(TAG, "reserved pins GPIO%d/GPIO%d held floating (V1.2 I2C placeholder)",
             PIN_I2C1_SDA_RESERVED, PIN_I2C1_SCL_RESERVED);
}

/*
 * Phase 4 echo: stage RX bytes through the host-tested ring buffer, then echo them
 * back onto the bus. Decoupling RX from TX via the ring buffer is the structure
 * Phase 6 (Modbus framing) builds on.
 */
static void rs485_echo_task(void *arg)
{
    (void)arg;
    static uint8_t rb_store[RX_STAGE_CAP];
    ring_buffer_t rx_rb;
    rb_init(&rx_rb, rb_store, sizeof(rb_store));

    uint8_t chunk[ECHO_CHUNK];
    uint32_t echoed = 0;
    uint32_t since_log = 0;

    for (;;) {
        int n = rs485_read(RS485_UART, chunk, sizeof(chunk), 20 /* ms */);
        if (n > 0) {
            rb_push(&rx_rb, chunk, (size_t)n);
            size_t m;
            while ((m = rb_pop(&rx_rb, chunk, sizeof(chunk))) > 0) {
                rs485_write(RS485_UART, chunk, m);
                echoed += (uint32_t)m;
            }
        }
        if (++since_log >= 250) { /* ~5 s at the 20 ms read timeout */
            since_log = 0;
            ESP_LOGI(TAG, "rs485 echo alive: %lu bytes echoed", (unsigned long)echoed);
        }
    }
}

void app_main(void)
{
    hold_reserved_pins_floating();

    const rs485_config_t cfg = {
        .port = RS485_UART,
        .tx_gpio = PIN_RS485_TX,
        .rx_gpio = PIN_RS485_RX,
        .de_re_gpio = PIN_RS485_DE_RE,
        .baud_rate = RS485_ECHO_BAUD,
        .rx_buffer_size = RS485_RX_DRV_BUF,
    };
    ESP_ERROR_CHECK(rs485_init(&cfg));

    xTaskCreate(rs485_echo_task, "rs485_echo", 4096, NULL, 10, NULL);

    for (uint32_t i = 0;; ++i) {
        printf("rak3112-rs485-node alive: tick=%lu\n", (unsigned long)i);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
