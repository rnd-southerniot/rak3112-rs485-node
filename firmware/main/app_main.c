#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "gpio_remap.h"
#include "lora.h"
#include "meter.h"
#include "ota.h"
#include "payload.h"
#include "provisioning.h"
#if CONFIG_APP_MODBUS_SCAN_ON_BOOT || CONFIG_APP_MODBUS_POLL_ON_BOOT || !CONFIG_APP_FIELD_SIMULATE
#include "modbus_master.h"
#include "rs485.h"
#endif

static const char *TAG = "app";

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic/exception";
    case ESP_RST_INT_WDT:
        return "interrupt-WDT";
    case ESP_RST_TASK_WDT:
        return "task-WDT";
    case ESP_RST_WDT:
        return "other-WDT";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep-wake";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "SDIO";
    case ESP_RST_USB:
        return "USB";
    case ESP_RST_JTAG:
        return "JTAG";
    default:
        return "unknown";
    }
}

/* Reset-cause + persistent boot counter, logged once at startup (7a; seed of the firmware §8 fault
 * log). */
static void log_boot_diagnostics(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();

    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nv = nvs_flash_init();
    }
    uint32_t boot = 0;
    nvs_handle_t h;
    if (nv == ESP_OK && nvs_open("faultlog", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "bootcnt", &boot); /* leaves boot=0 if the key is unset */
        boot += 1;
        nvs_set_u32(h, "bootcnt", boot);
        nvs_commit(h);
        nvs_close(h);
    }

    ESP_LOGW(TAG, "=== boot #%lu — reset reason: %s (%d) ===", (unsigned long)boot,
             reset_reason_str(reason), (int)reason);
    if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT) {
        ESP_LOGW(TAG, "previous boot ended in a WATCHDOG reset");
    } else if (reason == ESP_RST_BROWNOUT) {
        ESP_LOGW(TAG, "previous boot ended in a BROWNOUT — inspect the 3V3 rail / RT6160");
    }
}

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

/* Arm the Task Watchdog for the field-app task; fed once per cycle + during the sleep (7a). */
static void field_wdt_arm(void)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = 1000u * (uint32_t)CONFIG_APP_TASK_WDT_TIMEOUT_S,
/* Light-sleep (7b) freezes the idle tasks by design, so don't watch them — only the field task. */
#if CONFIG_APP_LIGHT_SLEEP
        .idle_core_mask = 0,
#else
        .idle_core_mask = (1u << portNUM_PROCESSORS) - 1u,
#endif
        .trigger_panic = true,
    };
    /* IDF already init'd the TWDT (CONFIG_ESP_TASK_WDT_INIT) — reconfigure to our timeout. */
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(esp_task_wdt_init(&cfg));
    } else {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL)); /* watch this (field-app) task */
    ESP_LOGI(TAG, "task watchdog armed: %ds timeout, panic-on-starve",
             CONFIG_APP_TASK_WDT_TIMEOUT_S);
}

/* Inter-sample wait split into <=2 s chunks, feeding the WDT each chunk so the long sleep never
 * starves the watchdog (a hang in the work section above still trips it). With light-sleep (7b)
 * each chunk is a manual light-sleep (CPU + peripherals gated, RTC timer wake; RAM retained so the
 * LoRaWAN session survives); otherwise a plain vTaskDelay. */
static void wdt_fed_delay(TickType_t total)
{
    const TickType_t chunk = pdMS_TO_TICKS(2000);
    while (total > 0) {
        const TickType_t d = total < chunk ? total : chunk;
#if CONFIG_APP_LIGHT_SLEEP
        ESP_ERROR_CHECK(
            esp_sleep_enable_timer_wakeup((uint64_t)(d * portTICK_PERIOD_MS) * 1000ULL));
        esp_light_sleep_start();
#else
        vTaskDelay(d);
#endif
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        total -= d;
    }
}

/* Runtime field configuration (7d). NVS namespace 'prov' overrides the Kconfig build defaults so a
 * node can be re-pointed at a different meter / baud / interval without a rebuild. */
typedef enum { FIELD_DEV_MFM384 = 0, FIELD_DEV_RSFSJT = 1 } field_device_t;
typedef struct {
    uint8_t device; /* field_device_t */
    uint32_t baud;
    uint8_t parity; /* 0=N, 1=E, 2=O */
    uint8_t unit;
    uint32_t interval_s;
    bool from_nvs; /* any field came from NVS */
} field_cfg_t;

static uart_parity_t parity_enum(uint8_t p)
{
    return p == 1 ? UART_PARITY_EVEN : (p == 2 ? UART_PARITY_ODD : UART_PARITY_DISABLE);
}
static const char *parity_label(uint8_t p)
{
    return p == 1 ? "8E1" : (p == 2 ? "8O1" : "8N1");
}

static void load_field_cfg(field_cfg_t *c)
{
#if CONFIG_APP_FIELD_DEVICE_RSFSJT
    c->device = FIELD_DEV_RSFSJT;
#else
    c->device = FIELD_DEV_MFM384;
#endif
    c->baud = (uint32_t)CONFIG_APP_FIELD_BAUD;
    c->parity = 0;
    c->unit = (uint8_t)CONFIG_APP_FIELD_UNIT;
    c->interval_s = (uint32_t)CONFIG_APP_FIELD_SAMPLE_INTERVAL_S;
    c->from_nvs = false;

    nvs_handle_t h;
    if (nvs_open("prov", NVS_READONLY, &h) == ESP_OK) {
        uint8_t u8;
        uint32_t u32;
        if (nvs_get_u8(h, "dev", &u8) == ESP_OK) {
            c->device = u8;
            c->from_nvs = true;
        }
        if (nvs_get_u32(h, "baud", &u32) == ESP_OK) {
            c->baud = u32;
            c->from_nvs = true;
        }
        if (nvs_get_u8(h, "par", &u8) == ESP_OK) {
            c->parity = u8;
            c->from_nvs = true;
        }
        if (nvs_get_u8(h, "unit", &u8) == ESP_OK) {
            c->unit = u8;
            c->from_nvs = true;
        }
        if (nvs_get_u32(h, "intv", &u32) == ESP_OK) {
            c->interval_s = u32;
            c->from_nvs = true;
        }
        nvs_close(h);
    }
}

/*
 * Field application: sample the configured device (real Modbus or simulated) then LoRaWAN-uplink a
 * compact ADR-005 payload, every interval. Config from NVS 'prov' (else Kconfig). Assumes lora is
 * already joined.
 */
static void run_field_app(void)
{
    field_cfg_t cfg;
    load_field_cfg(&cfg);
    const TickType_t period = pdMS_TO_TICKS(1000u * cfg.interval_s);
    const char *dev_name = (cfg.device == FIELD_DEV_RSFSJT) ? "RS-FSJT" : "MFM384";

#if !CONFIG_APP_FIELD_SIMULATE
    const rs485_config_t rcfg = {
        .port = UART_NUM_1,
        .tx_gpio = PIN_RS485_TX,
        .rx_gpio = PIN_RS485_RX,
        .de_re_gpio = PIN_RS485_DE_RE,
        .baud_rate = (int)cfg.baud,
        .parity = parity_enum(cfg.parity),
        .rx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(rs485_init(&rcfg));
    ESP_LOGI(TAG, "field app: REAL Modbus %s, unit %u @ %lu %s, %lus interval [cfg:%s]", dev_name,
             (unsigned)cfg.unit, (unsigned long)cfg.baud, parity_label(cfg.parity),
             (unsigned long)cfg.interval_s, cfg.from_nvs ? "NVS" : "build-default");
#else
    ESP_LOGW(TAG,
             "field app: SIMULATED %s (no Modbus), %lus interval [cfg:%s] — uplinks flagged "
             "simulated",
             dev_name, (unsigned long)cfg.interval_s, cfg.from_nvs ? "NVS" : "build-default");
#endif

    field_wdt_arm();
#if CONFIG_APP_LIGHT_SLEEP
    ESP_LOGI(TAG, "light-sleep duty-cycle ON — CPU sleeps between samples (session retained)");
#else
    ESP_LOGI(TAG, "light-sleep OFF — busy-wait between samples");
#endif

    for (uint32_t tick = 0;; ++tick) {
        esp_task_wdt_reset();
        uint8_t buf[PAYLOAD_MAX];
        size_t len = 0;
        uint8_t flags = 0;

        if (cfg.device == FIELD_DEV_MFM384) {
            mfm384_sample_t s = {0};
#if CONFIG_APP_FIELD_SIMULATE
            meter_sim_mfm384(tick, &s);
            flags |= TELEMETRY_FLAG_SIMULATED;
#else
            if (meter_read_mfm384(UART_NUM_1, cfg.unit, &s) != ESP_OK) {
                flags |= TELEMETRY_FLAG_STALE;
                ESP_LOGW(TAG, "[%lu] MFM384 read failed — uplinking stale-flagged",
                         (unsigned long)tick);
            }
#endif
            len = payload_encode_mfm384(&s, flags, buf, sizeof(buf));
            ESP_LOGI(TAG, "[%lu] MFM384 V=%.1f/%.1f/%.1f kW=%.2f kWh=%.2f f=%.2f pf=%.3f -> %u B",
                     (unsigned long)tick, s.v1n, s.v2n, s.v3n, s.total_kw, s.total_kwh, s.freq,
                     s.avg_pf, (unsigned)len);
        } else {
            rsfsjt_sample_t s = {0};
#if CONFIG_APP_FIELD_SIMULATE
            meter_sim_rsfsjt(tick, &s);
            flags |= TELEMETRY_FLAG_SIMULATED;
#else
            if (meter_read_rsfsjt(UART_NUM_1, cfg.unit, &s) != ESP_OK) {
                flags |= TELEMETRY_FLAG_STALE;
                ESP_LOGW(TAG, "[%lu] RS-FSJT read failed — uplinking stale-flagged",
                         (unsigned long)tick);
            }
#endif
            len = payload_encode_rsfsjt(&s, flags, buf, sizeof(buf));
            ESP_LOGI(TAG, "[%lu] RS-FSJT wind=%.2f m/s -> %u B", (unsigned long)tick, s.wind_mps,
                     (unsigned)len);
        }

        if (len > 0) {
            lora_send(buf, len);
        }

#if CONFIG_APP_DEBUG_WDT_STARVE
        if (tick + 1 >= (uint32_t)CONFIG_APP_DEBUG_WDT_STARVE_AFTER) {
            ESP_LOGE(TAG,
                     "DEBUG: starving the task watchdog after %lu cycle(s) — expect a TWDT reset "
                     "in ~%ds (7a gate)",
                     (unsigned long)(tick + 1), CONFIG_APP_TASK_WDT_TIMEOUT_S);
            for (;;) {
                vTaskDelay(pdMS_TO_TICKS(1000)); /* deliberately NOT feeding the watchdog */
            }
        }
#endif
        wdt_fed_delay(period);
    }
}

void app_main(void)
{
#if CONFIG_APP_DEBUG_OTA_BAD
    /* 7c rollback gate: crash before mark-valid so an OTA into this image rolls back. */
    ESP_LOGE(TAG, "DEBUG: bad OTA image — aborting before mark-valid (7c rollback test)");
    abort();
#endif
    hold_reserved_pins_floating();
    log_boot_diagnostics();
    ota_log_boot();
    ota_mark_valid(); /* booted into app code → healthy; cancel any pending rollback */

#if CONFIG_APP_MODBUS_POLL_ON_BOOT
    run_modbus_poll(); /* bench bring-up mode — does not return */
#elif CONFIG_APP_MODBUS_SCAN_ON_BOOT
    run_modbus_scan(); /* bench bring-up mode — does not return */
#endif

#if CONFIG_APP_PROVISIONING_CONSOLE
    provisioning_console_start(); /* prov-* REPL on its own task; available in field mode too */
    ota_register_commands();      /* ota-status / ota-activate on the same console */
#endif

    /* 7d: with no OTAA credentials (empty NVS 'prov' + placeholder compiled key), don't bogus-join
     * — idle until the provisioning console writes creds (prov-done restarts into field mode). */
    if (!lora_is_provisioned()) {
        for (uint32_t i = 0;; ++i) {
            if (i % 6 == 0) {
                ESP_LOGW(TAG, "AWAITING PROVISIONING — no LoRaWAN credentials. Run "
                              "tools/provision_nvs.py -p <port> (or the prov-* console).");
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

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

    run_field_app(); /* sample -> encode -> uplink loop; does not return */
}
