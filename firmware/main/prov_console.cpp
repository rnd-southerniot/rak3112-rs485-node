/*
 * prov_console.cpp — NVS provisioning REPL console (Phase 7).
 *
 * Implements esp_console commands that match the frozen /v1/provisioning-protocol
 * contract (api/routers/builds.py lines 182, 194, 221):
 *
 *   prov modbus <baud> <parity> <stopBits> <slaveId>
 *   prov creds  <devEui> <joinEui> <appKey>
 *   prov show
 *
 * NVS namespaces:
 *   "lorawan"  — lorawan_deveui, lorawan_joineui, lorawan_appkey  (shared with lora.cpp)
 *   "modbus"   — modbus_baud, modbus_parity, modbus_stop, modbus_slave
 *
 * Security invariants:
 *   T-02-02 / guardrail §3 #4: appKey is NEVER echoed to console output or logs.
 *   Creds are injected at RUNTIME into NVS — never committed to the source tree.
 */
#include "prov_console.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_console.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "prov";

/* NVS namespace constants — "lorawan" shared with lora.cpp LORA_NVS_NS. */
#define LORA_NVS_NS   "lorawan"
#define MODBUS_NVS_NS "modbus"

/* -------------------------------------------------------------------------
 * NVS string helpers — open/set/commit/close (same open/commit/close pattern
 * as nvs_save / nvs_load in lora.cpp; reused here per the plan requirement).
 * ------------------------------------------------------------------------- */

static void nvs_put_str(const char *ns, const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool nvs_get_str_safe(const char *ns, const char *key, char *buf, size_t len)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        size_t n = len;
        ok = (nvs_get_str(h, key, buf, &n) == ESP_OK);
        nvs_close(h);
    }
    return ok;
}

/* -------------------------------------------------------------------------
 * Command handlers — sub-commands dispatched from cmd_prov_dispatch.
 *
 * Argument convention (after dispatch shift):
 *   handle_modbus: argc=5  argv={"modbus","baud","parity","stop","slave"}
 *   handle_creds:  argc=4  argv={"creds","devEui","joinEui","appKey"}
 *   handle_show:   argc>=1 argv={"show"}
 * ------------------------------------------------------------------------- */

/*
 * prov modbus <baud> <parity> <stopBits> <slaveId>
 * Writes NVS keys: modbus_baud, modbus_parity, modbus_stop, modbus_slave.
 */
static int handle_modbus(int argc, char **argv)
{
    if (argc != 5) {
        printf("Usage: prov modbus <baud> <parity> <stopBits> <slaveId>\n"
               "  Example: prov modbus 9600 N 1 1\n");
        return 1;
    }
    nvs_put_str(MODBUS_NVS_NS, "modbus_baud",   argv[1]);
    nvs_put_str(MODBUS_NVS_NS, "modbus_parity", argv[2]);
    nvs_put_str(MODBUS_NVS_NS, "modbus_stop",   argv[3]);
    nvs_put_str(MODBUS_NVS_NS, "modbus_slave",  argv[4]);
    printf("OK: modbus baud=%s parity=%s stop=%s slave=%s\n",
           argv[1], argv[2], argv[3], argv[4]);
    return 0;
}

/*
 * prov creds <devEui> <joinEui> <appKey>
 * Writes NVS keys: lorawan_deveui, lorawan_joineui, lorawan_appkey.
 * T-02-02: appKey is NEVER printed in any output from this function.
 */
static int handle_creds(int argc, char **argv)
{
    if (argc != 4) {
        printf("Usage: prov creds <devEui> <joinEui> <appKey>\n"
               "  devEui : 16 hex chars (EUI-64)\n"
               "  joinEui: 16 hex chars (all-zero for ChirpStack: 0000000000000000)\n"
               "  appKey : 32 hex chars (128-bit OTAA root key)\n");
        return 1;
    }
    if (strlen(argv[1]) != 16) {
        printf("ERROR: devEui must be exactly 16 hex chars (got %zu)\n", strlen(argv[1]));
        return 1;
    }
    if (strlen(argv[2]) != 16) {
        printf("ERROR: joinEui must be exactly 16 hex chars (got %zu)\n", strlen(argv[2]));
        return 1;
    }
    if (strlen(argv[3]) != 32) {
        printf("ERROR: appKey must be exactly 32 hex chars (got %zu)\n", strlen(argv[3]));
        return 1;
    }
    nvs_put_str(LORA_NVS_NS, "lorawan_deveui",  argv[1]);
    nvs_put_str(LORA_NVS_NS, "lorawan_joineui", argv[2]);
    nvs_put_str(LORA_NVS_NS, "lorawan_appkey",  argv[3]);
    /* T-02-02: appKey NEVER echoed — only devEui and joinEui are safe to print. */
    printf("OK: creds devEui=%s joinEui=%s appKey=<redacted %zu hex>\n",
           argv[1], argv[2], strlen(argv[3]));
    return 0;
}

/*
 * prov show — print current NVS provisioning state.
 * T-02-02: appKey NEVER printed; only its presence is indicated.
 */
static int handle_show(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char buf[64];

    printf("=== NVS Provisioning State ===\n");
    printf("[modbus]\n");

#define SHOW(ns, key, label)  \
    printf("  %-8s: %s\n", (label), \
           nvs_get_str_safe((ns), (key), buf, sizeof(buf)) ? buf : "<not set>")

    SHOW(MODBUS_NVS_NS, "modbus_baud",   "baud");
    SHOW(MODBUS_NVS_NS, "modbus_parity", "parity");
    SHOW(MODBUS_NVS_NS, "modbus_stop",   "stop");
    SHOW(MODBUS_NVS_NS, "modbus_slave",  "slave");

#undef SHOW

    printf("[lorawan]\n");
    printf("  %-8s: %s\n", "devEui",
           nvs_get_str_safe(LORA_NVS_NS, "lorawan_deveui", buf, sizeof(buf)) ? buf : "<not set>");
    printf("  %-8s: %s\n", "joinEui",
           nvs_get_str_safe(LORA_NVS_NS, "lorawan_joineui", buf, sizeof(buf)) ? buf : "<not set>");
    /* T-02-02: never print the appKey value — report presence only. */
    bool has_appkey = nvs_get_str_safe(LORA_NVS_NS, "lorawan_appkey", buf, sizeof(buf));
    printf("  %-8s: %s\n", "appKey", has_appkey ? "<redacted>" : "<not set>");

    printf("==============================\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * Top-level 'prov' command dispatcher.
 * esp_console passes the full command line: argv[0]="prov", argv[1]=sub-cmd.
 * We shift +1 before forwarding so each handler sees its own name at argv[0].
 * ------------------------------------------------------------------------- */
static int cmd_prov_dispatch(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: prov <modbus|creds|show>\n"
               "  prov modbus <baud> <parity> <stopBits> <slaveId>\n"
               "  prov creds  <devEui> <joinEui> <appKey>\n"
               "  prov show\n");
        return 1;
    }

    const char *sub = argv[1];

    if (strcmp(sub, "modbus") == 0) return handle_modbus(argc - 1, argv + 1);
    if (strcmp(sub, "creds")  == 0) return handle_creds(argc - 1, argv + 1);
    if (strcmp(sub, "show")   == 0) return handle_show(argc - 1, argv + 1);

    printf("ERROR: unknown sub-command '%s'. Use: prov modbus|creds|show\n", sub);
    return 1;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

extern "C" esp_err_t prov_console_init(void)
{
    /* REPL configuration — prompt "esp> " per the frozen protocol contract. */
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt             = "esp> ";
    repl_cfg.max_cmdline_length = 256;

    /* USB-Serial-JTAG hardware config (Phase 3 finding: RAK3112 native USB is
     * USB-Serial-JTAG; CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y in sdkconfig.defaults). */
    esp_console_dev_usb_serial_jtag_config_t hw_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_console_repl_t *repl = NULL;
    esp_err_t ret = esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to create USB-Serial-JTAG REPL: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Built-in 'help' lists all registered commands. */
    esp_console_register_help_command();

    /* Register 'prov' command. */
    const esp_console_cmd_t prov_cmd = {
        .command  = "prov",
        .help     = "NVS provisioning: prov modbus|creds|show",
        .hint     = NULL,
        .func     = &cmd_prov_dispatch,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&prov_cmd));

    /* Start the REPL FreeRTOS task — non-blocking for the caller. */
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "provisioning console ready (prompt: 'esp> ')");
    return ESP_OK;
}
