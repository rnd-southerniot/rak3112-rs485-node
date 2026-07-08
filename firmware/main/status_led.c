#include "status_led.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gpio_remap.h" /* PIN_WS2812_DIN (GPIO38) */
#include "led_strip.h"

static const char *TAG = "status_led";

#define TICK_MS 30 /* animation frame period */
#define PEAK 60    /* WS2812 is bright — cap the peak (0..255) */

static led_strip_handle_t s_strip = NULL;
static volatile status_led_state_t s_state = STATUS_LED_BOOT;
static volatile int s_flash_ms = 0; /* >0 while a one-shot uplink flash plays */

/* Triangle wave 0..peak..0 over `period_ms` — a soft breathe (no float, no LUT). */
static uint8_t breathe(uint32_t t_ms, uint32_t period_ms, uint8_t peak)
{
    const uint32_t x = t_ms % period_ms;
    const uint32_t half = period_ms / 2;
    const uint32_t v = (x < half) ? (x * peak / half) : ((period_ms - x) * peak / half);
    return (uint8_t)v;
}

static void put(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_strip) {
        led_strip_set_pixel(s_strip, 0, r, g, b);
        led_strip_refresh(s_strip);
    }
}

static void status_led_task(void *arg)
{
    (void)arg;
    uint32_t t = 0;
    for (;;) {
        uint8_t r = 0, g = 0, b = 0;
        if (s_flash_ms > 0) {
            r = 0;
            g = PEAK;
            b = 0; /* bright-green uplink flash overlay */
            s_flash_ms -= TICK_MS;
        } else {
            switch (s_state) {
            case STATUS_LED_BOOT:
                r = g = b = 8; /* dim white */
                break;
            case STATUS_LED_SENSOR_WAIT:
                r = breathe(t, 2000, PEAK); /* red slow breathe */
                break;
            case STATUS_LED_JOINING: {
                const uint8_t v = breathe(t, 800, PEAK); /* amber pulse (r + half-g) */
                r = v;
                g = (uint8_t)(v / 2);
                break;
            }
            case STATUS_LED_IDLE:
                g = breathe(t, 3000, 12); /* very dim green breathe = healthy heartbeat */
                break;
            case STATUS_LED_SENSOR_FAULT:
                r = ((t / 150) % 2) ? PEAK : 0; /* red fast blink (~3 Hz) */
                break;
            }
        }
        put(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        t += TICK_MS;
    }
}

esp_err_t status_led_init(void)
{
    const led_strip_config_t scfg = {
        .strip_gpio_num = PIN_WS2812_DIN,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags = {.invert_out = false},
    };
    const led_strip_rmt_config_t rcfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
        .flags = {.with_dma = false},
    };
    const esp_err_t err = led_strip_new_rmt_device(&scfg, &rcfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 init failed (%s) — status LED disabled", esp_err_to_name(err));
        return err;
    }
    xTaskCreate(status_led_task, "status_led", 2560, NULL, 3, NULL);
    ESP_LOGI(TAG, "status LED up on GPIO%d (WS2812)", (int)PIN_WS2812_DIN);
    return ESP_OK;
}

void status_led_set(status_led_state_t st)
{
    s_state = st;
}

void status_led_event_uplink(void)
{
    s_flash_ms = 400;
}
