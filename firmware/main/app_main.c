#include <stdio.h>
#include <stdlib.h>

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
#if CONFIG_APP_MODBUS_SCAN_ON_BOOT || CONFIG_APP_MODBUS_POLL_ON_BOOT ||                            \
    CONFIG_APP_MODBUS_JOG_CONSOLE || CONFIG_APP_MODBUS_DUMP_ON_BOOT || !CONFIG_APP_FIELD_SIMULATE
#include "modbus_master.h"
#include "rs485.h"
#endif
#if CONFIG_APP_MODBUS_JOG_CONSOLE
#include "esp_console.h"
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

#if CONFIG_APP_MODBUS_JOG_CONSOLE
/* -----------------------------------------------------------------------------------------------
 * Bench JOG console for a Modbus motor drive (LEESN IG28ET / Leadshine iEM-RS map, researched in
 * docs/RUNBOOK.md "Stepper side-quest"). Boots IDLE; the shaft turns only while a jog command runs,
 * and every jog is bounded (1-5 s) and auto-STOPs. FC06 writes here COMMAND MOTION — gated by the
 * hardware-safety checklist before this mode is ever flashed. mot-read is read-only.
 * --------------------------------------------------------------------------------------------- */
/* Verified against the LEESN "485 Communication Manual" V126 (manufacturer PDF). The earlier
 * Leadshine-iEM-RS 0x1xxx/CiA402 guesses were wrong — those addresses don't exist on this drive
 * (it echoed FC06 writes to them but the whole 0x1xxx range reads 0), which is why the jog was
 * inert. Real map: jog = 0x00CA (self-contained command word), enable = 0x00D4, feedback at 0x00xx.
 */
#define IG_REG_JOG 0x00CAu     /* Motor Jog command word (WriteWORD) — bit layout in jog_word() */
#define IG_REG_ENABLE 0x00D4u  /* Offline/Enable: low byte 0 = enable, 1 = release */
#define IG_REG_POS 0x0004u     /* Real-time position, INT32 @ 0x0004..5 (low word first) */
#define IG_REG_SPEED 0x0019u   /* Real-time speed, INT16 rpm (signed) */
#define IG_REG_STATUS 0x0006u  /* Operation+input status @ 0x0006..7 (op-state bits 8..9) */
#define IG_REG_ALARM 0x00A3u   /* Alarm status (read) */
#define IG_REG_RUN_ABS 0x00D0u /* Run to absolute position, INT32 (FC16, when stationary) */
#define IG_REG_RUNSPEED 0x00D8u  /* Running speed, INT32 0.01 rpm (FC16) */
#define IG_JOG_SPEED_RPM 60u     /* jog speed (bits 14..6 of 0x00CA), 0..511 rpm */
#define IG_GOTO_MAX_COUNTS 40000 /* bench safety cap on a single run-to-position move */
#define JOG_PORT UART_NUM_1
#define JOG_TIMEOUT_MS 500u

static uint8_t s_jog_unit;

/* Build the 0x00CA jog command word. bit15 = direction (0 CW / 1 CCW); bits14..6 = speed (rpm);
 * bit5 = stop method (0 decel / 1 immediate, only relevant on stop); bit0 = 1 run / 0 stop.
 * Manual example: jog 50 rpm CW running = 0x0C81. */
static uint16_t jog_word(int ccw, unsigned rpm, int run)
{
    if (rpm > 511u) {
        rpm = 511u;
    }
    uint16_t v = (uint16_t)(((rpm & 0x1FFu) << 6) | (run ? 1u : 0u));
    if (ccw) {
        v |= (uint16_t)(1u << 15);
    }
    return v;
}

/* FC06 write with a logged result. Returns the modbus status so callers can abort on failure. */
static modbus_status_t jog_write(uint16_t addr, uint16_t val)
{
    uint8_t exc = 0;
    const modbus_status_t st =
        modbus_master_write_single(JOG_PORT, s_jog_unit, addr, val, JOG_TIMEOUT_MS, &exc);
    if (st == MODBUS_OK) {
        ESP_LOGI(TAG, "  write 0x%04X = 0x%04X  OK", (unsigned)addr, (unsigned)val);
    } else if (st == MODBUS_ERR_EXCEPTION) {
        ESP_LOGW(TAG, "  write 0x%04X = 0x%04X  exception 0x%02X", (unsigned)addr, (unsigned)val,
                 (unsigned)exc);
    } else {
        ESP_LOGW(TAG, "  write 0x%04X = 0x%04X  FAILED st=%d (no reply / echo mismatch)",
                 (unsigned)addr, (unsigned)val, (int)st);
    }
    return st;
}

/* FC16 write of a 32-bit value across a register pair, LOW word at addr, HIGH word at addr+1. */
static modbus_status_t jog_write_s32(uint16_t addr, int32_t val)
{
    uint16_t regs[2];
    regs[0] = (uint16_t)((uint32_t)val & 0xFFFFu);         /* low word at addr */
    regs[1] = (uint16_t)(((uint32_t)val >> 16) & 0xFFFFu); /* high word at addr+1 */
    uint8_t exc = 0;
    const modbus_status_t st =
        modbus_master_write_multi(JOG_PORT, s_jog_unit, addr, 2, regs, JOG_TIMEOUT_MS, &exc);
    if (st == MODBUS_OK) {
        ESP_LOGI(TAG, "  write32 0x%04X = %ld  OK", (unsigned)addr, (long)val);
    } else {
        ESP_LOGW(TAG, "  write32 0x%04X = %ld  FAILED st=%d exc=0x%02X", (unsigned)addr, (long)val,
                 (int)st, (unsigned)exc);
    }
    return st;
}

/* Read a 32-bit value from a register pair, LOW word at addr, HIGH word at addr+1 (the word order
 * this drive uses, confirmed from the manual examples, e.g. 30000 reads back as 75 30 00 00). */
static int32_t jog_read_s32(uint16_t addr)
{
    uint16_t regs[2] = {0, 0};
    uint8_t exc = 0;
    const modbus_status_t st =
        modbus_master_read(JOG_PORT, s_jog_unit, MODBUS_FC_READ_HOLDING_REGISTERS, addr, 2,
                           JOG_TIMEOUT_MS, regs, &exc);
    if (st != MODBUS_OK) {
        return 0;
    }
    return (int32_t)(((uint32_t)regs[1] << 16) | (uint32_t)regs[0]);
}

static uint16_t jog_read_u16(uint16_t addr, modbus_status_t *st_out)
{
    uint16_t reg = 0;
    uint8_t exc = 0;
    const modbus_status_t st =
        modbus_master_read(JOG_PORT, s_jog_unit, MODBUS_FC_READ_HOLDING_REGISTERS, addr, 1,
                           JOG_TIMEOUT_MS, &reg, &exc);
    if (st_out != NULL) {
        *st_out = st;
    }
    return reg;
}

static int cmd_mot_read(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    modbus_status_t st = MODBUS_OK;
    const uint16_t status = jog_read_u16(IG_REG_STATUS, &st);
    if (st != MODBUS_OK) {
        ESP_LOGW(TAG, "mot-read: status read failed st=%d — check bus/baud/unit", (int)st);
        return 0;
    }
    const int32_t pos = jog_read_s32(IG_REG_POS);
    const int16_t vel = (int16_t)jog_read_u16(IG_REG_SPEED, NULL);
    modbus_status_t sa = MODBUS_OK;
    const uint16_t alarm = jog_read_u16(IG_REG_ALARM, &sa);
    const unsigned opstate = (unsigned)((status >> 8) & 0x3u); /* bits 8..9: 0 idle, 3 running */
    ESP_LOGI(TAG, "mot-read: status=0x%04X (op=%u) pos=%ld vel=%d rpm alarm=0x%04X",
             (unsigned)status, opstate, (long)pos, (int)vel, (unsigned)alarm);
    return 0;
}

static int cmd_mot_enable(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ESP_LOGI(TAG, "mot-enable (0x00D4 = 0)");
    jog_write(IG_REG_ENABLE, 0x0000u); /* low byte 0 = enable */
    return 0;
}

static int cmd_mot_disable(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ESP_LOGI(TAG, "mot-disable (jog stop + release 0x00D4 = 1)");
    jog_write(IG_REG_JOG, jog_word(0, 0, 0)); /* stop any jog */
    jog_write(IG_REG_ENABLE, 0x0001u);        /* low byte 1 = release motor */
    return 0;
}

static int cmd_jog_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ESP_LOGW(TAG, "jog-stop");
    jog_write(IG_REG_JOG, jog_word(0, 0, 0)); /* run bit clear -> decel stop */
    return 0;
}

/* Enable -> issue the 0x00CA jog word for a BOUNDED time, polling position/speed -> always STOP. */
static void jog_run(int ccw, const char *label, int secs)
{
    if (secs < 1) {
        secs = 1;
    }
    if (secs > 5) {
        secs = 5; /* hard cap: a bench jog is brief by design */
    }
    ESP_LOGW(TAG, ">>> JOG %s @ %u rpm for %ds — MOTOR WILL MOVE <<<", label,
             (unsigned)IG_JOG_SPEED_RPM, secs);
    jog_write(IG_REG_ENABLE,
              0x0000u); /* ensure enabled (0 = enable); harmless if already enabled */
    const int32_t pos0 = jog_read_s32(IG_REG_POS);
    if (jog_write(IG_REG_JOG, jog_word(ccw, IG_JOG_SPEED_RPM, 1)) != MODBUS_OK) {
        ESP_LOGW(TAG, "jog command failed — stopping");
        jog_write(IG_REG_JOG, jog_word(0, 0, 0));
        return;
    }
    for (int i = 0; i < secs * 5; ++i) {
        vTaskDelay(pdMS_TO_TICKS(200));
        const int32_t pos = jog_read_s32(IG_REG_POS);
        const int16_t vel = (int16_t)jog_read_u16(IG_REG_SPEED, NULL);
        ESP_LOGI(TAG, "  [%dms] pos=%ld vel=%d rpm", (i + 1) * 200, (long)pos, (int)vel);
    }
    jog_write(IG_REG_JOG, jog_word(0, 0, 0)); /* always stop, even if a poll failed mid-run */
    const int32_t pos1 = jog_read_s32(IG_REG_POS);
    ESP_LOGW(TAG, "jog %s done — STOPPED. position %ld -> %ld (delta %ld counts)", label,
             (long)pos0, (long)pos1, (long)(pos1 - pos0));
}

static int cmd_jog_fwd(int argc, char **argv)
{
    jog_run(0 /* CW */, "FWD", (argc > 1) ? atoi(argv[1]) : 2);
    return 0;
}

static int cmd_jog_rev(int argc, char **argv)
{
    jog_run(1 /* CCW */, "REV", (argc > 1) ? atoi(argv[1]) : 2);
    return 0;
}

/* Profile to an absolute target position (0x00D0, relative to origin) and auto-stop there. The
 * move magnitude is capped for bench safety; the drive ramps + stops itself at the target. */
static int cmd_goto(int argc, char **argv)
{
    if (argc < 2) {
        ESP_LOGW(TAG, "usage: goto <target_pulses>  (absolute position, relative to origin)");
        return 0;
    }
    const int32_t target = (int32_t)strtol(argv[1], NULL, 0);
    const int32_t pos0 = jog_read_s32(IG_REG_POS);
    const int32_t dist = (target > pos0) ? (target - pos0) : (pos0 - target);
    if (dist > IG_GOTO_MAX_COUNTS) {
        ESP_LOGW(TAG, "goto refused: move of %ld counts exceeds the %d bench cap (current pos %ld)",
                 (long)dist, IG_GOTO_MAX_COUNTS, (long)pos0);
        return 0;
    }
    ESP_LOGW(TAG, ">>> GOTO %ld (from %ld, %ld counts) @ %u rpm — MOTOR WILL MOVE <<<",
             (long)target, (long)pos0, (long)dist, (unsigned)IG_JOG_SPEED_RPM);
    jog_write(IG_REG_ENABLE, 0x0000u);                               /* enable */
    jog_write_s32(IG_REG_RUNSPEED, (int32_t)IG_JOG_SPEED_RPM * 100); /* 0.01 rpm units */
    if (jog_write_s32(IG_REG_RUN_ABS, target) != MODBUS_OK) {
        ESP_LOGW(TAG, "goto command failed");
        return 0;
    }
    int32_t last = pos0;
    int stable = 0;
    for (int i = 0; i < 40; ++i) { /* up to 8 s; the drive auto-stops at the target */
        vTaskDelay(pdMS_TO_TICKS(200));
        const int32_t pos = jog_read_s32(IG_REG_POS);
        ESP_LOGI(TAG, "  [%dms] pos=%ld", (i + 1) * 200, (long)pos);
        if (pos == last) {
            if (++stable >= 3) {
                break; /* settled at rest */
            }
        } else {
            stable = 0;
        }
        last = pos;
    }
    const int32_t pos1 = jog_read_s32(IG_REG_POS);
    ESP_LOGW(TAG, "goto done. position %ld -> %ld (target %ld, err %ld counts)", (long)pos0,
             (long)pos1, (long)target, (long)(target - pos1));
    return 0;
}

/* Bench JOG console: RS-485 up, motor idle, REPL on its own task. Does not return. */
static void run_modbus_jog_console(void)
{
    const rs485_config_t cfg = {
        .port = UART_NUM_1,
        .tx_gpio = PIN_RS485_TX,
        .rx_gpio = PIN_RS485_RX,
        .de_re_gpio = PIN_RS485_DE_RE,
        .baud_rate = CONFIG_APP_MODBUS_JOG_BAUD,
        .parity = UART_PARITY_DISABLE,
        .rx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(rs485_init(&cfg));
    s_jog_unit = (uint8_t)CONFIG_APP_MODBUS_JOG_UNIT;
    ESP_LOGW(TAG,
             "JOG console: unit %u @ %d 8N1. Motor is IDLE. Commands: mot-read | mot-enable | "
             "mot-disable | jog-fwd [s] | jog-rev [s] | jog-stop. SAFETY: motor must be "
             "secured/unloaded/current-limited.",
             (unsigned)s_jog_unit, CONFIG_APP_MODBUS_JOG_BAUD);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "jog>";
    repl_cfg.max_cmdline_length = 100;
    esp_console_dev_usb_serial_jtag_config_t hw_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl));

    const esp_console_cmd_t cmds[] = {
        {.command = "mot-read",
         .help = "read drive telemetry (status/pos/vel/alarm) — read-only",
         .func = cmd_mot_read},
        {.command = "mot-enable",
         .help = "enable the drive over RS-485 (FC06)",
         .func = cmd_mot_enable},
        {.command = "mot-disable", .help = "stop + de-energize the drive", .func = cmd_mot_disable},
        {.command = "jog-fwd",
         .help = "jog forward N sec (1-5, default 2) — MOVES THE MOTOR",
         .func = cmd_jog_fwd},
        {.command = "jog-rev",
         .help = "jog reverse N sec (1-5, default 2) — MOVES THE MOTOR",
         .func = cmd_jog_rev},
        {.command = "jog-stop", .help = "stop the motor immediately", .func = cmd_jog_stop},
        {.command = "goto",
         .help = "run to absolute position <pulses> (capped move) — MOVES THE MOTOR",
         .func = cmd_goto},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); ++i) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl)); /* runs on its own task */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000)); /* idle the main task; commands run on the REPL task */
    }
}
#endif /* CONFIG_APP_MODBUS_JOG_CONSOLE */

#if CONFIG_APP_MODBUS_DUMP_ON_BOOT
/* Read-only register-discovery sweep: FC03 every register in a block, log the non-zero ones plus a
 * readable/zero/exception tally. Reveals a drive's real map when the documented one doesn't match.
 * No writes, no motion. */
static void dump_range(uint8_t unit, uint16_t lo, uint16_t hi)
{
    ESP_LOGI(TAG, "-- range 0x%04X..0x%04X --", (unsigned)lo, (unsigned)hi);
    int nz = 0, zero = 0, exc = 0, to = 0;
    for (uint32_t a = lo; a <= (uint32_t)hi; ++a) { /* uint32 counter: no wrap if hi==0xFFFF */
        uint16_t reg = 0;
        uint8_t e = 0;
        const modbus_status_t st = modbus_master_read(
            UART_NUM_1, unit, MODBUS_FC_READ_HOLDING_REGISTERS, (uint16_t)a, 1, 200, &reg, &e);
        if (st == MODBUS_OK) {
            if (reg != 0) {
                ESP_LOGI(TAG, "  0x%04X = 0x%04X (%u)", (unsigned)a, (unsigned)reg, (unsigned)reg);
                ++nz;
            } else {
                ++zero;
            }
        } else if (st == MODBUS_ERR_EXCEPTION) {
            ++exc;
        } else {
            ++to;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGI(TAG, "  [range done] nonzero=%d zero=%d exception=%d timeout=%d", nz, zero, exc, to);
}

/* Bench register-discovery dump. Sweeps candidate blocks once, then idles. Does not return. */
static void run_modbus_dump(void)
{
    const rs485_config_t cfg = {
        .port = UART_NUM_1,
        .tx_gpio = PIN_RS485_TX,
        .rx_gpio = PIN_RS485_RX,
        .de_re_gpio = PIN_RS485_DE_RE,
        .baud_rate = CONFIG_APP_MODBUS_DUMP_BAUD,
        .parity = UART_PARITY_DISABLE,
        .rx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(rs485_init(&cfg));
    const uint8_t unit = (uint8_t)CONFIG_APP_MODBUS_DUMP_UNIT;
    ESP_LOGW(TAG, "=== Modbus register DISCOVERY DUMP: unit %u @ %d 8N1, FC03, read-only ===",
             (unsigned)unit, CONFIG_APP_MODBUS_DUMP_BAUD);

    /* Candidate blocks: Leadshine Pr-params, telemetry, control region, alarms, CiA402 objects. */
    static const uint16_t ranges[][2] = {
        {0x0000, 0x00FF}, {0x0180, 0x01FF}, {0x1000, 0x1050},
        {0x1800, 0x1810}, {0x2200, 0x2210}, {0x6040, 0x6080},
    };
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
        dump_range(unit, ranges[i][0], ranges[i][1]);
    }
    ESP_LOGW(TAG, "=== DUMP COMPLETE — non-zero registers above reveal the drive's real map ===");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif /* CONFIG_APP_MODBUS_DUMP_ON_BOOT */

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

#if CONFIG_APP_LED_HEARTBEAT || CONFIG_APP_FIELD_LED_BLINK
#include "led_strip.h"
static led_strip_handle_t led_strip_open(void); /* defined below */
#endif
#if CONFIG_APP_FIELD_LED_BLINK
static void led_blink_once(led_strip_handle_t strip); /* defined below */
#endif

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
#if CONFIG_APP_FIELD_LED_BLINK
    led_strip_handle_t field_led = led_strip_open();
    ESP_LOGI(TAG, "field LED blink ON — WS2812 flashes once per cycle (wake indicator)");
#endif

    for (uint32_t tick = 0;; ++tick) {
        esp_task_wdt_reset();
#if CONFIG_APP_FIELD_LED_BLINK
        led_blink_once(field_led); /* visible 'woke up' flash each cycle */
#endif
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

#if CONFIG_APP_LED_HEARTBEAT || CONFIG_APP_FIELD_LED_BLINK
#include "led_strip.h"
/* Open the onboard WS2812 (GPIO38) via RMT. */
static led_strip_handle_t led_strip_open(void)
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
    led_strip_handle_t strip = NULL;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&scfg, &rcfg, &strip));
    return strip;
}
#endif

#if CONFIG_APP_FIELD_LED_BLINK
/* One short green flash — per-cycle wake indicator in the field app. */
static void led_blink_once(led_strip_handle_t strip)
{
    if (!strip) {
        return;
    }
    led_strip_set_pixel(strip, 0, 0, 90, 0);
    led_strip_refresh(strip);
    vTaskDelay(pdMS_TO_TICKS(120));
    led_strip_set_pixel(strip, 0, 0, 0, 0);
    led_strip_refresh(strip);
}
#endif

#if CONFIG_APP_LED_HEARTBEAT
/* Bench "alive" indicator: breathe the WS2812 (GPIO38), rotating colour so it's clearly
 * firmware-driven. No LoRa / Modbus / provisioning / sleep — proves the board runs on any supply
 * (DC1 / USB-C / power bank) without a USB host. Does not return. */
static void run_led_heartbeat(void)
{
    led_strip_handle_t strip = led_strip_open();
    ESP_LOGW(TAG, "LED heartbeat — board ALIVE. Breathing WS2812 (GPIO38); no LoRa/Modbus/sleep.");

    static const uint8_t palette[][3] = {
        {0, 1, 0}, {0, 1, 1}, {0, 0, 1},
        {1, 0, 1}, {1, 1, 0}, /* green, cyan, blue, magenta, yellow */
    };
    const int n = sizeof(palette) / sizeof(palette[0]);
    const int peak = 80; /* WS2812 is bright — keep moderate */
    for (uint32_t i = 0;; ++i) {
        const uint8_t *c = palette[i % n];
        for (int b = 0; b <= peak; ++b) {
            led_strip_set_pixel(strip, 0, (uint32_t)(c[0] * b), (uint32_t)(c[1] * b),
                                (uint32_t)(c[2] * b));
            led_strip_refresh(strip);
            vTaskDelay(pdMS_TO_TICKS(9));
        }
        for (int b = peak; b >= 0; --b) {
            led_strip_set_pixel(strip, 0, (uint32_t)(c[0] * b), (uint32_t)(c[1] * b),
                                (uint32_t)(c[2] * b));
            led_strip_refresh(strip);
            vTaskDelay(pdMS_TO_TICKS(9));
        }
        vTaskDelay(pdMS_TO_TICKS(140));
    }
}
#endif /* CONFIG_APP_LED_HEARTBEAT */

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

#if CONFIG_APP_LED_HEARTBEAT
    run_led_heartbeat(); /* bench: breathe the WS2812 forever; does not return */
#endif

#if CONFIG_APP_MODBUS_POLL_ON_BOOT
    run_modbus_poll(); /* bench bring-up mode — does not return */
#elif CONFIG_APP_MODBUS_SCAN_ON_BOOT
    run_modbus_scan(); /* bench bring-up mode — does not return */
#elif CONFIG_APP_MODBUS_JOG_CONSOLE
    run_modbus_jog_console(); /* bench motor-jog console — does not return */
#elif CONFIG_APP_MODBUS_DUMP_ON_BOOT
    run_modbus_dump(); /* bench register-discovery dump — does not return */
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
