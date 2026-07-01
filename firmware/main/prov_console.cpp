/*
 * prov_console.cpp — provisioning REPL console (merge reconcile of the careflow CRM contract with
 * main's profile-driven provisioning, ADR-006).
 *
 * Owns the single USB-Serial-JTAG REPL (prompt "esp> ", 115200) that the CRM
 * /v1/provisioning-protocol contract + the WebSerial flasher speak:
 *
 *   prov modbus <baud> <parity> <stopBits> <slaveId>
 *   prov creds  <devEui> <joinEui> <appKey>
 *   prov show
 *
 * RECONCILE: the command NAMES (frozen contract) are preserved, but the handlers now write main's
 * canonical NVS namespace "prov" (u64 deveui/joineui, blob appkey, u8/u32 modbus config) — the same
 * store prov-lorawan/prov-modbus use and lora.cpp/load_field_cfg read. So there is ONE NVS schema
 * and ONE credential loader; the WebSerial flasher and the profile-driven runtime both work.
 *
 * This REPL also hosts main's prov-lorawan/-modbus/-profile/-show/-clear/-done commands
 * (registered via provisioning_register_commands) so the profile-driven path is available too.
 *
 * Security: T-02-02 / guardrail §3 #4 — the appKey is NEVER echoed to console output or logs.
 */
#include "prov_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "nvs.h"

#include "provisioning.h" /* provisioning_register_commands() — main's prov-* commands */

static const char *TAG = "prov";

/* main's canonical provisioning namespace (shared with provisioning.c + lora.cpp + app_main). */
#define PROV_NS "prov"

/* Parse `n` hex chars into `out` bytes. Returns true on exactly-`n`-hex input. */
static bool hex_to_bytes(const char *s, uint8_t *out, size_t nbytes)
{
    if (strlen(s) != nbytes * 2) {
        return false;
    }
    for (size_t i = 0; i < nbytes; ++i) {
        char b[3] = {s[2 * i], s[2 * i + 1], 0};
        char *end = NULL;
        long v = strtol(b, &end, 16);
        if (end != b + 2 || v < 0 || v > 0xff) {
            return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

/*
 * prov modbus <baud> <parity> <stopBits> <slaveId>  — writes NVS "prov" baud(u32)/par(u8)/unit(u8).
 * (stopBits is accepted for contract compatibility; main's transport is 1 stop bit.)
 */
static int handle_modbus(int argc, char **argv)
{
    if (argc != 5) {
        printf("Usage: prov modbus <baud> <parity> <stopBits> <slaveId>\n"
               "  Example: prov modbus 9600 N 1 1\n");
        return 1;
    }
    const uint32_t baud = (uint32_t)strtoul(argv[1], NULL, 10);
    const char p = argv[2][0];
    const uint8_t par = (p == 'E' || p == 'e') ? 1u : (p == 'O' || p == 'o') ? 2u : 0u;
    const uint8_t unit = (uint8_t)atoi(argv[4]);

    nvs_handle_t h;
    if (nvs_open(PROV_NS, NVS_READWRITE, &h) != ESP_OK) {
        printf("ERROR: nvs open\n");
        return 1;
    }
    nvs_set_u32(h, "baud", baud);
    nvs_set_u8(h, "par", par);
    nvs_set_u8(h, "unit", unit);
    const esp_err_t err = nvs_commit(h);
    nvs_close(h);
    printf(err == ESP_OK ? "OK: modbus baud=%lu parity=%s slave=%u (stop=%s ignored, 1 stop bit)\n"
                         : "ERROR: nvs commit\n",
           (unsigned long)baud, argv[2], (unsigned)unit, argv[3]);
    return err == ESP_OK ? 0 : 1;
}

/*
 * prov creds <devEui> <joinEui> <appKey>  — writes NVS "prov"
 * deveui(u64)/joineui(u64)/appkey(blob). T-02-02: the appKey is NEVER printed.
 */
static int handle_creds(int argc, char **argv)
{
    if (argc != 4 || strlen(argv[1]) != 16 || strlen(argv[2]) != 16 || strlen(argv[3]) != 32) {
        printf("Usage: prov creds <devEui:16hex> <joinEui:16hex> <appKey:32hex>\n");
        return 1;
    }
    uint8_t appkey[16];
    if (!hex_to_bytes(argv[3], appkey, sizeof(appkey))) {
        printf("ERROR: appKey is not 32 hex chars\n");
        return 1;
    }
    const uint64_t deveui = strtoull(argv[1], NULL, 16);
    const uint64_t joineui = strtoull(argv[2], NULL, 16);

    nvs_handle_t h;
    if (nvs_open(PROV_NS, NVS_READWRITE, &h) != ESP_OK) {
        printf("ERROR: nvs open\n");
        return 1;
    }
    nvs_set_u64(h, "deveui", deveui);
    nvs_set_u64(h, "joineui", joineui);
    nvs_set_blob(h, "appkey", appkey, sizeof(appkey));
    const esp_err_t err = nvs_commit(h);
    nvs_close(h);
    /* T-02-02: appKey redacted. */
    printf(err == ESP_OK ? "OK: creds devEui=%s joinEui=%s appKey=<redacted 32 hex>\n"
                         : "ERROR: nvs commit\n",
           argv[1], argv[2]);
    return err == ESP_OK ? 0 : 1;
}

/* prov show — print NVS "prov" state; appKey presence only (never the value). */
static int handle_show(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    nvs_handle_t h;
    if (nvs_open(PROV_NS, NVS_READONLY, &h) != ESP_OK) {
        printf("prov: (empty)\n");
        return 0;
    }
    uint64_t de = 0, je = 0;
    uint32_t baud = 0, intv = 0;
    uint8_t par = 0, unit = 0, dev = 0;
    uint8_t ak[16];
    size_t akn = sizeof(ak);
    nvs_get_u64(h, "deveui", &de);
    nvs_get_u64(h, "joineui", &je);
    const bool hasak = (nvs_get_blob(h, "appkey", ak, &akn) == ESP_OK && akn == sizeof(ak));
    nvs_get_u32(h, "baud", &baud);
    nvs_get_u32(h, "intv", &intv);
    nvs_get_u8(h, "par", &par);
    nvs_get_u8(h, "unit", &unit);
    nvs_get_u8(h, "dev", &dev);
    nvs_close(h);
    printf("=== NVS Provisioning State (ns 'prov') ===\n");
    printf("[lorawan] devEui=%016llx joinEui=%016llx appKey=%s\n", (unsigned long long)de,
           (unsigned long long)je, hasak ? "<redacted>" : "<not set>");
    printf("[modbus]  baud=%lu parity=%u slave=%u dev=%u interval=%lu\n", (unsigned long)baud,
           (unsigned)par, (unsigned)unit, (unsigned)dev, (unsigned long)intv);
    return 0;
}

/* 'prov' dispatcher: argv[0]="prov", argv[1]=sub-command. */
static int cmd_prov_dispatch(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: prov <modbus|creds|show>\n");
        return 1;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "modbus") == 0) {
        return handle_modbus(argc - 1, argv + 1);
    }
    if (strcmp(sub, "creds") == 0) {
        return handle_creds(argc - 1, argv + 1);
    }
    if (strcmp(sub, "show") == 0) {
        return handle_show(argc - 1, argv + 1);
    }
    printf("ERROR: unknown sub-command '%s'. Use: prov modbus|creds|show\n", sub);
    return 1;
}

extern "C" esp_err_t prov_console_init(void)
{
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "esp> ";          /* frozen CRM / WebSerial contract */
    repl_cfg.max_cmdline_length = 1100; /* a full prov-profile blob is ~900 hex chars */

    esp_console_dev_usb_serial_jtag_config_t hw_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t *repl = NULL;
    esp_err_t ret = esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to create USB-Serial-JTAG REPL: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_console_register_help_command();

    /* CRM-contract command: prov modbus|creds|show. */
    esp_console_cmd_t prov_cmd =
        {}; /* zero-init all fields (C++ warns on partial designated init) */
    prov_cmd.command = "prov";
    prov_cmd.help = "NVS provisioning (CRM contract): prov modbus|creds|show";
    prov_cmd.func = &cmd_prov_dispatch;
    ESP_ERROR_CHECK(esp_console_cmd_register(&prov_cmd));

    /* main's profile-driven commands on the same REPL: prov-lorawan/-modbus/-profile/-show/... */
    provisioning_register_commands();

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG,
             "provisioning console ready (esp> ) — 'prov modbus|creds|show' + prov-* commands");
    return ESP_OK;
}
