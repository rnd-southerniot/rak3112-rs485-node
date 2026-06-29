#include "provisioning.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "prov";
#define PROV_NS "prov"

static bool parse_hex_bytes(const char *s, uint8_t *out, size_t nbytes)
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

/* prov-lorawan <deveui16hex> <joineui16hex> <appkey32hex> */
static int cmd_lorawan(int argc, char **argv)
{
    if (argc != 4 || strlen(argv[1]) != 16 || strlen(argv[2]) != 16 || strlen(argv[3]) != 32) {
        printf("ERR usage: prov-lorawan <deveui16hex> <joineui16hex> <appkey32hex>\n");
        return 1;
    }
    uint8_t appkey[16];
    if (!parse_hex_bytes(argv[3], appkey, sizeof(appkey))) {
        printf("ERR appkey not 32 hex chars\n");
        return 1;
    }
    const uint64_t de = strtoull(argv[1], NULL, 16);
    const uint64_t je = strtoull(argv[2], NULL, 16);
    nvs_handle_t h;
    if (nvs_open(PROV_NS, NVS_READWRITE, &h) != ESP_OK) {
        printf("ERR nvs open\n");
        return 1;
    }
    nvs_set_u64(h, "deveui", de);
    nvs_set_u64(h, "joineui", je);
    nvs_set_blob(h, "appkey", appkey, sizeof(appkey));
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    printf(err == ESP_OK ? "OK lorawan deveui=%016llx joineui=%016llx appkey=<redacted>\n"
                         : "ERR nvs commit\n",
           (unsigned long long)de, (unsigned long long)je);
    return err == ESP_OK ? 0 : 1;
}

/* prov-modbus <dev 0|1> <baud> <parity 0|1|2> <unit> <interval_s> */
static int cmd_modbus(int argc, char **argv)
{
    if (argc != 6) {
        printf("ERR usage: prov-modbus <dev 0=MFM384|1=RSFSJT> <baud> <par 0=N|1=E|2=O> <unit> "
               "<interval_s>\n");
        return 1;
    }
    nvs_handle_t h;
    if (nvs_open(PROV_NS, NVS_READWRITE, &h) != ESP_OK) {
        printf("ERR nvs open\n");
        return 1;
    }
    nvs_set_u8(h, "dev", (uint8_t)atoi(argv[1]));
    nvs_set_u32(h, "baud", (uint32_t)strtoul(argv[2], NULL, 10));
    nvs_set_u8(h, "par", (uint8_t)atoi(argv[3]));
    nvs_set_u8(h, "unit", (uint8_t)atoi(argv[4]));
    nvs_set_u32(h, "intv", (uint32_t)strtoul(argv[5], NULL, 10));
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        printf("ERR nvs commit\n");
        return 1;
    }
    printf("OK modbus dev=%s baud=%s par=%s unit=%s intv=%s\n", argv[1], argv[2], argv[3], argv[4],
           argv[5]);
    return 0;
}

static int cmd_show(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    nvs_handle_t h;
    if (nvs_open(PROV_NS, NVS_READONLY, &h) != ESP_OK) {
        printf("prov: (empty)\n");
        return 0;
    }
    uint64_t de = 0, je = 0;
    nvs_get_u64(h, "deveui", &de);
    nvs_get_u64(h, "joineui", &je);
    uint8_t ak[16];
    size_t n = sizeof(ak);
    const bool hasak = (nvs_get_blob(h, "appkey", ak, &n) == ESP_OK && n == sizeof(ak));
    uint8_t dev = 0, par = 0, unit = 0;
    uint32_t baud = 0, intv = 0;
    nvs_get_u8(h, "dev", &dev);
    nvs_get_u32(h, "baud", &baud);
    nvs_get_u8(h, "par", &par);
    nvs_get_u8(h, "unit", &unit);
    nvs_get_u32(h, "intv", &intv);
    nvs_close(h);
    printf("prov: deveui=%016llx joineui=%016llx appkey=%s | dev=%u baud=%lu par=%u unit=%u "
           "intv=%lu\n",
           (unsigned long long)de, (unsigned long long)je, hasak ? "set" : "unset", (unsigned)dev,
           (unsigned long)baud, (unsigned)par, (unsigned)unit, (unsigned long)intv);
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    nvs_handle_t h;
    if (nvs_open(PROV_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    printf("OK cleared\n");
    return 0;
}

static int cmd_done(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("OK restarting into field mode\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

void provisioning_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "prov>";
    repl_cfg.max_cmdline_length = 200;
    esp_console_dev_usb_serial_jtag_config_t hw_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl));

    const esp_console_cmd_t cmds[] = {
        {.command = "prov-lorawan",
         .help = "<deveui16> <joineui16> <appkey32> — set OTAA credentials",
         .func = cmd_lorawan},
        {.command = "prov-modbus",
         .help = "<dev 0|1> <baud> <par 0|1|2> <unit> <interval_s> — set field config",
         .func = cmd_modbus},
        {.command = "prov-show",
         .help = "print current provisioning (appkey redacted)",
         .func = cmd_show},
        {.command = "prov-clear", .help = "erase the provisioning namespace", .func = cmd_clear},
        {.command = "prov-done", .help = "commit + restart into field mode", .func = cmd_done},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); ++i) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    esp_console_register_help_command();

    ESP_LOGI(TAG,
             "provisioning console ready — prov-lorawan / prov-modbus / prov-show / prov-clear "
             "/ prov-done (or tools/provision_nvs.py); nonces/session preserved");
    ESP_ERROR_CHECK(esp_console_start_repl(repl)); /* runs on its own task; we return */
}
