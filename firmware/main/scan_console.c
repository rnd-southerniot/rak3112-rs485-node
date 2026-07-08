/*
 * scan_console.c — RS-485 register-discovery commands for the Pi scanner/profiling station.
 *
 * The Pi drives these over the `esp>` console to turn an unknown Modbus device into a Careflow
 * device-profile. Every command wraps the node's existing, proven Modbus master (modbus_master.h)
 * and the Phase-4 rs485 transport; nothing here writes NVS, flash, or restarts. Output is
 * machine-parseable: each command ends in exactly one `OK …` or `ERR …` line, register words are
 * printed as fixed-width uppercase hex.
 *
 * Bus config is held in a module-static rs485_config_t applied by `scan-cfg`; the other commands
 * require a prior successful scan-cfg (UART1 is not initialised in the idle branch until then).
 */
#include "scan_console.h"

#include "sdkconfig.h"

#if CONFIG_APP_SCAN_CONSOLE

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_console.h"
#include "esp_log.h"

#include "gpio_remap.h" /* PIN_RS485_TX / _RX / _DE_RE */
#include "modbus_master.h"
#include "modbus_rtu.h"
#include "rs485.h"

static const char *TAG = "scan";

/* Fixed per-transaction timeout. Generous enough for slow baud (1200) + long replies; the Pi paces
 * its own command cadence. Not operator-tunable to keep the command surface minimal. */
#define SCAN_TIMEOUT_MS 500u

static bool s_configured = false;
static uint32_t s_baud = 0;
static char s_parity = 'N';
static uint8_t s_stop = 1;

/* --- small parse helpers -------------------------------------------------------------------- */

static bool parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v > 0xFFFFFFFFul) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* FC arg must be 3 (holding) or 4 (input); returns the Modbus function code or 0 on error. */
static uint8_t parse_fc(const char *s)
{
    if (strcmp(s, "3") == 0) {
        return MODBUS_FC_READ_HOLDING_REGISTERS;
    }
    if (strcmp(s, "4") == 0) {
        return MODBUS_FC_READ_INPUT_REGISTERS;
    }
    return 0;
}

/* Map a completed modbus_master_read() status to a stable class token for the host parser. */
static const char *read_class(modbus_status_t st)
{
    switch (st) {
    case MODBUS_OK:
        return "DATA";
    case MODBUS_ERR_EXCEPTION:
        return "EXC";
    case MODBUS_ERR_SHORT:
        return "ABS"; /* no/short reply within the timeout = absent */
    default:
        return "BAD"; /* bytes seen but CRC/ID/func/bytecount mismatch = noise / wrong baud */
    }
}

static const char *probe_class(mb_probe_t p)
{
    switch (p) {
    case MB_PROBE_PRESENT_DATA:
        return "DATA";
    case MB_PROBE_PRESENT_EXCEPTION:
        return "EXCEPTION";
    case MB_PROBE_BADFRAME:
        return "BADFRAME";
    default:
        return "ABSENT";
    }
}

/* Re-init UART1 for the requested line settings using the exact run_modbus_scan() recipe. Stop-bit
 * count is recorded for the echo but the rs485 transport is 1-stop (an extra stop bit is only idle
 * time on RX, so it does not block discovery). */
static bool apply_cfg(uint32_t baud, char parity_c, uint8_t stop)
{
    uart_parity_t parity;
    switch (parity_c) {
    case 'N':
        parity = UART_PARITY_DISABLE;
        break;
    case 'E':
        parity = UART_PARITY_EVEN;
        break;
    case 'O':
        parity = UART_PARITY_ODD;
        break;
    default:
        return false;
    }
    if (uart_is_driver_installed(UART_NUM_1)) {
        uart_driver_delete(UART_NUM_1);
    }
    const rs485_config_t cfg = {
        .port = UART_NUM_1,
        .tx_gpio = PIN_RS485_TX,
        .rx_gpio = PIN_RS485_RX,
        .de_re_gpio = PIN_RS485_DE_RE,
        .baud_rate = (int)baud,
        .parity = parity,
        .rx_buffer_size = 512,
    };
    if (rs485_init(&cfg) != ESP_OK) {
        return false;
    }
    s_baud = baud;
    s_parity = parity_c;
    s_stop = stop;
    s_configured = true;
    return true;
}

/* --- commands ------------------------------------------------------------------------------- */

/* scan-cfg <baud> <N|E|O> <1|2> — (re)configure the RS-485 UART for the sweep. */
static int cmd_scan_cfg(int argc, char **argv)
{
    if (argc != 4) {
        printf("ERR usage: scan-cfg <baud> <N|E|O> <1|2>\n");
        return 1;
    }
    uint32_t baud = 0;
    if (!parse_u32(argv[1], &baud) || baud < 1200 || baud > 115200) {
        printf("ERR baud out of range (1200..115200)\n");
        return 1;
    }
    const char parity = (char)toupper((unsigned char)argv[2][0]);
    if (strlen(argv[2]) != 1 || (parity != 'N' && parity != 'E' && parity != 'O')) {
        printf("ERR parity must be N, E or O\n");
        return 1;
    }
    uint32_t stop = 0;
    if (!parse_u32(argv[3], &stop) || (stop != 1 && stop != 2)) {
        printf("ERR stop must be 1 or 2\n");
        return 1;
    }
    if (!apply_cfg(baud, parity, (uint8_t)stop)) {
        printf("ERR rs485_init failed at %lu %c%lu\n", (unsigned long)baud, parity,
               (unsigned long)stop);
        return 1;
    }
    printf("OK scan-cfg baud=%lu par=%c stop=%lu\n", (unsigned long)baud, parity,
           (unsigned long)stop);
    return 0;
}

/* scan-probe <unit> <fc> <addr> — one classified read of a single register (honours fc). */
static int cmd_scan_probe(int argc, char **argv)
{
    if (!s_configured) {
        printf("ERR not configured — run scan-cfg first\n");
        return 1;
    }
    uint32_t unit = 0, addr = 0;
    const uint8_t fc = (argc == 4) ? parse_fc(argv[2]) : 0;
    if (argc != 4 || !parse_u32(argv[1], &unit) || unit > 247 || fc == 0 ||
        !parse_u32(argv[3], &addr) || addr > 0xFFFF) {
        printf("ERR usage: scan-probe <unit 0-247> <fc 3|4> <addr 0-65535>\n");
        return 1;
    }
    uint16_t reg = 0;
    uint8_t exc = 0;
    const modbus_status_t st = modbus_master_read(UART_NUM_1, (uint8_t)unit, fc, (uint16_t)addr, 1,
                                                  SCAN_TIMEOUT_MS, &reg, &exc);
    printf("OK scan-probe unit=%lu fc=%u addr=%lu class=%s reg0=0x%04X exc=0x%02X\n",
           (unsigned long)unit, (unsigned)fc, (unsigned long)addr, read_class(st), (unsigned)reg,
           (unsigned)exc);
    return 0;
}

/* scan-ids <lo> <hi> [addr] — probe a unit-ID range (FC03 @ addr); exception still = present. */
static int cmd_scan_ids(int argc, char **argv)
{
    if (!s_configured) {
        printf("ERR not configured — run scan-cfg first\n");
        return 1;
    }
    uint32_t lo = 0, hi = 0, addr = 0;
    if (argc < 3 || argc > 4 || !parse_u32(argv[1], &lo) || !parse_u32(argv[2], &hi) || lo < 1 ||
        hi > 247 || lo > hi) {
        printf("ERR usage: scan-ids <lo 1-247> <hi 1-247> [addr]\n");
        return 1;
    }
    if (argc == 4 && (!parse_u32(argv[3], &addr) || addr > 0xFFFF)) {
        printf("ERR addr out of range\n");
        return 1;
    }
    int found = 0;
    for (uint32_t id = lo; id <= hi; ++id) {
        mb_probe_info_t info = {0};
        const mb_probe_t p =
            modbus_master_probe(UART_NUM_1, (uint8_t)id, (uint16_t)addr, SCAN_TIMEOUT_MS, &info);
        if (p == MB_PROBE_PRESENT_DATA || p == MB_PROBE_PRESENT_EXCEPTION) {
            ++found;
        }
        if (p != MB_PROBE_ABSENT) {
            printf("ID %lu %s reg0=0x%04X exc=0x%02X lat=%lums\n", (unsigned long)id,
                   probe_class(p), (unsigned)info.reg0, (unsigned)info.exception,
                   (unsigned long)info.latency_ms);
        }
    }
    printf("OK scan-ids found=%d\n", found);
    return 0;
}

/* scan-read <unit> <3|4> <addr> <qty> — one raw register read; prints the words as hex. */
static int cmd_scan_read(int argc, char **argv)
{
    if (!s_configured) {
        printf("ERR not configured — run scan-cfg first\n");
        return 1;
    }
    uint32_t unit = 0, addr = 0, qty = 0;
    const uint8_t fc = (argc == 5) ? parse_fc(argv[2]) : 0;
    if (argc != 5 || !parse_u32(argv[1], &unit) || unit > 247 || fc == 0 ||
        !parse_u32(argv[3], &addr) || addr > 0xFFFF || !parse_u32(argv[4], &qty) || qty < 1 ||
        qty > MODBUS_MAX_READ_REGS) {
        printf("ERR usage: scan-read <unit 0-247> <fc 3|4> <addr 0-65535> <qty 1-%u>\n",
               (unsigned)MODBUS_MAX_READ_REGS);
        return 1;
    }
    static uint16_t regs[MODBUS_MAX_READ_REGS];
    uint8_t exc = 0;
    const modbus_status_t st = modbus_master_read(UART_NUM_1, (uint8_t)unit, fc, (uint16_t)addr,
                                                  (uint16_t)qty, SCAN_TIMEOUT_MS, regs, &exc);
    if (st == MODBUS_ERR_EXCEPTION) {
        printf("ERR scan-read exc=0x%02X\n", (unsigned)exc);
        return 1;
    }
    if (st != MODBUS_OK) {
        printf("ERR scan-read class=%s st=%d\n", read_class(st), (int)st);
        return 1;
    }
    printf("OK scan-read unit=%lu fc=%u addr=%lu qty=%lu regs=", (unsigned long)unit, (unsigned)fc,
           (unsigned long)addr, (unsigned long)qty);
    for (uint32_t i = 0; i < qty; ++i) {
        printf("%04X%s", (unsigned)regs[i], (i + 1 < qty) ? " " : "");
    }
    printf("\n");
    return 0;
}

/* scan-sweep <unit> <fc> <start> <end> <stride> — block-read [start..end] in `stride`-sized chunks,
 * one SWEEP line per chunk (a chunk containing an absent register reports EXC/ABS; the host then
 * narrows by re-reading smaller blocks). */
static int cmd_scan_sweep(int argc, char **argv)
{
    if (!s_configured) {
        printf("ERR not configured — run scan-cfg first\n");
        return 1;
    }
    uint32_t unit = 0, start = 0, end = 0, stride = 0;
    const uint8_t fc = (argc == 6) ? parse_fc(argv[2]) : 0;
    if (argc != 6 || !parse_u32(argv[1], &unit) || unit > 247 || fc == 0 ||
        !parse_u32(argv[3], &start) || start > 0xFFFF || !parse_u32(argv[4], &end) ||
        end > 0xFFFF || end < start || !parse_u32(argv[5], &stride) || stride < 1 ||
        stride > MODBUS_MAX_READ_REGS) {
        printf("ERR usage: scan-sweep <unit> <fc 3|4> <start> <end> <stride 1-%u>\n",
               (unsigned)MODBUS_MAX_READ_REGS);
        return 1;
    }
    static uint16_t regs[MODBUS_MAX_READ_REGS];
    unsigned present = 0, exc_chunks = 0;
    for (uint32_t addr = start; addr <= end; addr += stride) {
        const uint32_t remain = end - addr + 1;
        const uint16_t qty = (uint16_t)((remain < stride) ? remain : stride);
        uint8_t exc = 0;
        const modbus_status_t st = modbus_master_read(UART_NUM_1, (uint8_t)unit, fc, (uint16_t)addr,
                                                      qty, SCAN_TIMEOUT_MS, regs, &exc);
        printf("SWEEP addr=%lu qty=%u %s", (unsigned long)addr, (unsigned)qty, read_class(st));
        if (st == MODBUS_OK) {
            ++present;
            printf(" regs=");
            for (uint16_t i = 0; i < qty; ++i) {
                printf("%04X%s", (unsigned)regs[i], (i + 1 < qty) ? " " : "");
            }
        } else if (st == MODBUS_ERR_EXCEPTION) {
            ++exc_chunks;
            printf(" exc=0x%02X", (unsigned)exc);
        } else {
            printf(" st=%d", (int)st);
        }
        printf("\n");
    }
    printf("OK scan-sweep present=%u exc=%u\n", present, exc_chunks);
    return 0;
}

void scan_console_register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "scan-cfg",
         .help =
             "<baud> <N|E|O> <1|2> — configure the RS-485 UART for a sweep (2-stop not enforced)",
         .func = cmd_scan_cfg},
        {.command = "scan-probe",
         .help = "<unit> <fc 3|4> <addr> — one classified register read",
         .func = cmd_scan_probe},
        {.command = "scan-ids",
         .help = "<lo> <hi> [addr] — probe a unit-ID range (exception still = present)",
         .func = cmd_scan_ids},
        {.command = "scan-read",
         .help = "<unit> <fc 3|4> <addr> <qty> — raw register read (hex words)",
         .func = cmd_scan_read},
        {.command = "scan-sweep",
         .help = "<unit> <fc 3|4> <start> <end> <stride> — block-sweep a register range",
         .func = cmd_scan_sweep},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); ++i) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    ESP_LOGI(TAG, "scan-* commands registered (scan-cfg/-probe/-ids/-read/-sweep) — "
                  "read-only Modbus discovery for the Pi scanner; no NVS/flash writes");
}

#endif /* CONFIG_APP_SCAN_CONSOLE */
