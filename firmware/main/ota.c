#include "ota.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "ota";

static const char *state_str(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending-verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    default:
        return "undefined";
    }
}

void ota_log_boot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    ESP_LOGW(TAG, "running slot=%s state=%s tag='%s'", run->label, state_str(st),
             CONFIG_APP_BUILD_TAG);
}

void ota_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGW(TAG, "%s marked VALID (was pending-verify): %s", run->label, esp_err_to_name(e));
    }
}

static int cmd_ota_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    printf("ota: running=%s state=%s boot=%s next-slot=%s tag=%s\n", run->label, state_str(st),
           boot ? boot->label : "?", next ? next->label : "?", CONFIG_APP_BUILD_TAG);
    return 0;
}

/* ota-activate <0|1> — boot a slot whose image was already staged (e.g. parttool write_partition).
 * Uses the real esp_ota_set_boot_partition path → PENDING_VERIFY + rollback armed. */
static int cmd_ota_activate(int argc, char **argv)
{
    if (argc != 2) {
        printf("ERR usage: ota-activate <0|1>\n");
        return 1;
    }
    const int slot = atoi(argv[1]);
    const esp_partition_subtype_t sub =
        (slot == 1) ? ESP_PARTITION_SUBTYPE_APP_OTA_1 : ESP_PARTITION_SUBTYPE_APP_OTA_0;
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, NULL);
    if (!p) {
        printf("ERR no ota_%d partition\n", slot);
        return 1;
    }
    const esp_err_t e = esp_ota_set_boot_partition(p);
    if (e != ESP_OK) {
        printf("ERR set_boot %s: %s (is a valid app image staged in the slot?)\n", p->label,
               esp_err_to_name(e));
        return 1;
    }
    printf("OK boot -> %s (pending-verify, rollback armed) — restarting\n", p->label);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

void ota_register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "ota-status",
         .help = "show running/boot/next OTA slot + state",
         .func = cmd_ota_status},
        {.command = "ota-activate",
         .help = "<0|1> boot a pre-staged OTA slot (pending-verify; rollback armed)",
         .func = cmd_ota_activate},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); ++i) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}
