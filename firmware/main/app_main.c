#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "gpio_remap.h"
#include "lora.h"
#include "meter.h"
#include "payload.h"
#include "prov_console.h"
#if CONFIG_APP_MODBUS_SCAN_ON_BOOT || CONFIG_APP_MODBUS_POLL_ON_BOOT || !CONFIG_APP_FIELD_SIMULATE
#include "modbus_master.h"
#include "rs485.h"
#endif

static const char *TAG = "app";

/* -------------------------------------------------------------------------
 * Phase 7: Modbus config from NVS (written by "prov modbus").
 * Falls back to Kconfig defaults when NVS keys are absent.
 * ------------------------------------------------------------------------- */
#define MODBUS_NVS_NS "modbus"

typedef struct {
    int            baud;
    uart_parity_t  parity;
    int            stop_bits;
    uint8_t        slave_id;
    bool           from_nvs;
} modbus_nvs_cfg_t;

/* Read modbus config from NVS namespace "modbus". Kconfig defaults are used
 * for any key that is absent. Logs a "[modbus]" boot-verify marker. */
static modbus_nvs_cfg_t read_modbus_nvs_cfg(void)
{
    modbus_nvs_cfg_t cfg = {
        .baud      = CONFIG_APP_FIELD_BAUD,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = 1,
        .slave_id  = (uint8_t)CONFIG_APP_FIELD_UNIT,
        .from_nvs  = false,
    };

    nvs_handle_t h;
    if (nvs_open(MODBUS_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "[modbus] compiled defaults: baud=%d 8N1 slave=%u",
                 cfg.baud, (unsigned)cfg.slave_id);
        return cfg;
    }

    char buf[32];
    size_t len;

    len = sizeof(buf);
    if (nvs_get_str(h, "modbus_baud", buf, &len) == ESP_OK)
        cfg.baud = atoi(buf);

    len = sizeof(buf);
    if (nvs_get_str(h, "modbus_parity", buf, &len) == ESP_OK) {
        if (buf[0] == 'E' || buf[0] == 'e')      cfg.parity = UART_PARITY_EVEN;
        else if (buf[0] == 'O' || buf[0] == 'o') cfg.parity = UART_PARITY_ODD;
        else                                       cfg.parity = UART_PARITY_DISABLE;
    }

    len = sizeof(buf);
    if (nvs_get_str(h, "modbus_stop", buf, &len) == ESP_OK)
        cfg.stop_bits = atoi(buf);

    len = sizeof(buf);
    if (nvs_get_str(h, "modbus_slave", buf, &len) == ESP_OK)
        cfg.slave_id = (uint8_t)atoi(buf);

    nvs_close(h);
    cfg.from_nvs = true;

    /* Boot-verify marker: "modbus" (lowercase) searched by WebSerial boot-verify. */
    char parity_ch = (cfg.parity == UART_PARITY_EVEN) ? 'E'
                   : (cfg.parity == UART_PARITY_ODD)  ? 'O' : 'N';
    ESP_LOGI(TAG, "[modbus] NVS: baud=%d 8%c%d slave=%u",
             cfg.baud, parity_ch, cfg.stop_bits, (unsigned)cfg.slave_id);
    return cfg;
}

/* File-scope modbus config — populated in app_main() and used by run_field_app(). */
static modbus_nvs_cfg_t s_modbus_cfg;
/* ------------------------------------------------------------------------- */

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
    const uint16_t probe_addr = (uint16_t)CONFIG_APP_MODBUS_SCAN_PROBE_REG;

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
/* Bench bring-up: poll one Modbus register set at 1 Hz on CN1 and print the value (or
 * timeout/exception). Supports FC03/FC04 and, for the SELEC MFM384, a 32-bit float across 2
 * registers. Lets you swap A/B + verify wiring against a known device and watch it come alive
 * live. Fixed 8N1 (MFM384 + RS-FSJT both use it). Does not return. */
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
    const uint8_t func = (CONFIG_APP_MODBUS_POLL_FC == 4) ? MODBUS_FC_READ_INPUT_REGISTERS
                                                          : MODBUS_FC_READ_HOLDING_REGISTERS;
#if CONFIG_APP_MODBUS_POLL_FLOAT
    const uint16_t qty = 2;
#if CONFIG_APP_MODBUS_POLL_FLOAT_CDAB
    const modbus_word_order_t word_order = MODBUS_WORD_ORDER_CDAB;
    const char *order_label = "CDAB";
#else
    const modbus_word_order_t word_order = MODBUS_WORD_ORDER_ABCD;
    const char *order_label = "ABCD";
#endif
    ESP_LOGI(TAG, "Modbus poll: unit %u, FC%02u reg %u..%u @ %d 8N1, float32 %s, 1 Hz",
             (unsigned)unit, (unsigned)func, (unsigned)reg, (unsigned)(reg + 1),
             CONFIG_APP_MODBUS_POLL_BAUD, order_label);
#else
    const uint16_t qty = 1;
    ESP_LOGI(TAG, "Modbus poll: unit %u, FC%02u reg %u @ %d 8N1, uint16, 1 Hz — swap A/B if silent",
             (unsigned)unit, (unsigned)func, (unsigned)reg, CONFIG_APP_MODBUS_POLL_BAUD);
#endif
    for (uint32_t n = 0;; ++n) {
        uint16_t val[2] = {0, 0};
        uint8_t exc = 0;
        const modbus_status_t st =
            modbus_master_read(UART_NUM_1, unit, func, reg, qty, 500, val, &exc);
        if (st == MODBUS_OK) {
#if CONFIG_APP_MODBUS_POLL_FLOAT
            const float f = modbus_regs_to_f32(val, word_order);
            ESP_LOGI(TAG, "[%lu] reg%u..%u = 0x%04X%04X = %.3f", (unsigned long)n, (unsigned)reg,
                     (unsigned)(reg + 1), (unsigned)val[0], (unsigned)val[1], f);
#else
            ESP_LOGI(TAG, "[%lu] reg%u = %u (raw, 0x%04X)", (unsigned long)n, (unsigned)reg,
                     (unsigned)val[0], (unsigned)val[0]);
#endif
        } else if (st == MODBUS_ERR_EXCEPTION) {
            ESP_LOGW(TAG, "[%lu] exception 0x%02X (device answered — wrong reg/FC?)",
                     (unsigned long)n, (unsigned)exc);
        } else {
            ESP_LOGW(TAG, "[%lu] timeout (st=%d) — no reply; check A/B, GND, baud, unit ID",
                     (unsigned long)n, (int)st);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif /* CONFIG_APP_MODBUS_POLL_ON_BOOT */

/*
 * Field application: sample the configured device (real Modbus or simulated) then LoRaWAN-uplink a
 * compact ADR-005 payload, every CONFIG_APP_FIELD_SAMPLE_INTERVAL_S. Assumes lora is already
 * joined.
 */
static void run_field_app(void)
{
    /* Phase 7: use NVS modbus config when available, else Kconfig defaults. */
    const uint8_t unit = s_modbus_cfg.from_nvs ? s_modbus_cfg.slave_id
                                                : (uint8_t)CONFIG_APP_FIELD_UNIT;
    const TickType_t period = pdMS_TO_TICKS(1000u * (uint32_t)CONFIG_APP_FIELD_SAMPLE_INTERVAL_S);

#if !CONFIG_APP_FIELD_SIMULATE
    const rs485_config_t cfg = {
        .port = UART_NUM_1,
        .tx_gpio = PIN_RS485_TX,
        .rx_gpio = PIN_RS485_RX,
        .de_re_gpio = PIN_RS485_DE_RE,
        .baud_rate = s_modbus_cfg.from_nvs ? s_modbus_cfg.baud : CONFIG_APP_FIELD_BAUD,
        .parity    = s_modbus_cfg.from_nvs ? s_modbus_cfg.parity : UART_PARITY_DISABLE,
        .rx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(rs485_init(&cfg));
    ESP_LOGI(TAG, "field app: REAL modbus, unit %u @ %d %s %ds interval", (unsigned)unit,
             cfg.baud_rate, s_modbus_cfg.from_nvs ? "(NVS)" : "(compiled)",
             CONFIG_APP_FIELD_SAMPLE_INTERVAL_S);
#else
    ESP_LOGW(TAG, "field app: SIMULATED data (no Modbus), %ds interval — uplinks flagged simulated",
             CONFIG_APP_FIELD_SAMPLE_INTERVAL_S);
#endif

    for (uint32_t tick = 0;; ++tick) {
        uint8_t buf[PAYLOAD_MAX];
        size_t len = 0;
        uint8_t flags = 0;

#if CONFIG_APP_FIELD_DEVICE_MFM384
        mfm384_sample_t s = {0};
#if CONFIG_APP_FIELD_SIMULATE
        meter_sim_mfm384(tick, &s);
        flags |= TELEMETRY_FLAG_SIMULATED;
#else
        if (meter_read_mfm384(UART_NUM_1, unit, &s) != ESP_OK) {
            flags |= TELEMETRY_FLAG_STALE;
            ESP_LOGW(TAG, "[%lu] MFM384 read failed — uplinking stale-flagged",
                     (unsigned long)tick);
        }
#endif
        len = payload_encode_mfm384(&s, flags, buf, sizeof(buf));
        ESP_LOGI(TAG, "[%lu] MFM384 V=%.1f/%.1f/%.1f kW=%.2f kWh=%.2f f=%.2f pf=%.3f -> %u B",
                 (unsigned long)tick, s.v1n, s.v2n, s.v3n, s.total_kw, s.total_kwh, s.freq,
                 s.avg_pf, (unsigned)len);
#elif CONFIG_APP_FIELD_DEVICE_RSFSJT
        rsfsjt_sample_t s = {0};
#if CONFIG_APP_FIELD_SIMULATE
        meter_sim_rsfsjt(tick, &s);
        flags |= TELEMETRY_FLAG_SIMULATED;
#else
        if (meter_read_rsfsjt(UART_NUM_1, unit, &s) != ESP_OK) {
            flags |= TELEMETRY_FLAG_STALE;
            ESP_LOGW(TAG, "[%lu] RS-FSJT read failed — uplinking stale-flagged",
                     (unsigned long)tick);
        }
#endif
        len = payload_encode_rsfsjt(&s, flags, buf, sizeof(buf));
        ESP_LOGI(TAG, "[%lu] RS-FSJT wind=%.2f m/s -> %u B", (unsigned long)tick, s.wind_mps,
                 (unsigned)len);
#endif

        if (len > 0) {
            lora_send(buf, len);
        }
        vTaskDelay(period);
    }
}

void app_main(void)
{
    hold_reserved_pins_floating();

#if CONFIG_APP_MODBUS_POLL_ON_BOOT
    run_modbus_poll(); /* bench bring-up mode — does not return */
#elif CONFIG_APP_MODBUS_SCAN_ON_BOOT
    run_modbus_scan(); /* bench bring-up mode — does not return */
#endif

    /* Phase 7: initialise radio + NVS flash. NVS must be up before reading
     * modbus config or starting the provisioning console. */
    ESP_ERROR_CHECK(lora_init());

    /* Phase 7: read modbus config from NVS (populated by "prov modbus" command).
     * Falls back to Kconfig defaults when absent. Logs "[modbus]" boot marker. */
    s_modbus_cfg = read_modbus_nvs_cfg();

    /* Phase 7: start the NVS provisioning REPL as a FreeRTOS background task.
     * Runs concurrently with the join loop and field app.
     * lora_init() ensures nvs_flash_init() was called before this point. */
    ESP_ERROR_CHECK(prov_console_init());

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

    run_field_app(); /* sample -> encode -> uplink loop; does not return */
}
