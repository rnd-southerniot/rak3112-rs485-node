#include "lora.h"

#include <RadioLib.h>

#include <string.h>
#include <stdint.h>

#include "EspHalS3.h"
#include "esp_log.h"
#include "gpio_remap.h"
/* Real credentials header is gitignored (holds the AppKey secret, generated from firmware/.env).
 * Fall back to the committed placeholder template when it's absent (clean clone / CI build-check)
 * so the project always builds; placeholder creds cannot join a network.
 *
 * Phase 7 NVS override: lora_join() checks NVS keys lorawan_deveui / lorawan_joineui /
 * lorawan_appkey FIRST (written by the prov_console "prov creds" command). Compiled creds
 * are only used when NVS keys are absent (first boot before provisioning). */
#if defined(__has_include) && __has_include("lora_credentials.h")
#include "lora_credentials.h"
#else
#pragma message(                                                                                   \
    "lora_credentials.h not found — using PLACEHOLDER OTAA creds (build-check only; copy lora_credentials.h.example to lora_credentials.h and fill real keys to join)")
#include "lora_credentials.h.example"
#endif
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "lora";
#define LORA_NVS_NS "lorawan"

/* SPI(SCK=5, MISO=3, MOSI=6); Module(NSS=7, DIO1=47, RST=8, BUSY=48) — ADR-001 (bench-confirmed).
 */
static EspHalS3 *s_hal = new EspHalS3(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI);
static SX1262 s_radio =
    new Module(s_hal, PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RESET, PIN_LORA_BUSY);
static const LoRaWANBand_t s_band = AS923; /* AS923-1, freq offset 0 (ADR-004) */
static LoRaWANNode s_node(&s_radio, &s_band, 0);

static void nvs_save(const char *key, const uint8_t *buf, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(LORA_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, key, buf, len);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool nvs_load(const char *key, uint8_t *buf, size_t len)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(LORA_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = len;
        ok = (nvs_get_blob(h, key, buf, &n) == ESP_OK && n == len);
        nvs_close(h);
    }
    return ok;
}

/* Load a NVS string key from LORA_NVS_NS (written by prov_console "prov creds"). */
static bool nvs_load_str(const char *key, char *buf, size_t len)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(LORA_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = len;
        ok = (nvs_get_str(h, key, buf, &n) == ESP_OK);
        nvs_close(h);
    }
    return ok;
}

/* Parse a hex string (e.g. "3CDC75FFFE6F8924") into a uint64_t MSB-first. */
static uint64_t hex_str_to_u64(const char *s)
{
    return (uint64_t)strtoull(s, nullptr, 16);
}

/* Parse a hex string of 2*len chars into len bytes (MSB-first). */
static bool hex_str_to_bytes(const char *s, uint8_t *out, size_t len)
{
    if (strlen(s) != len * 2) return false;
    for (size_t i = 0; i < len; ++i) {
        char pair[3] = { s[i * 2], s[i * 2 + 1], '\0' };
        out[i] = (uint8_t)strtoul(pair, nullptr, 16);
    }
    return true;
}

extern "C" esp_err_t lora_init(void)
{
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* TCXO 1.8 V via begin (8th arg); DC-DC regulator (useRegulatorLDO=false). */
    int16_t st =
        s_radio.begin(923.2, 125.0, 9, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 8, 1.8f, false);
    if (st != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "radio.begin @1.8V failed (%d); retrying @1.6V", st);
        st = s_radio.begin(923.2, 125.0, 9, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 8, 1.6f,
                           false);
        if (st != RADIOLIB_ERR_NONE) {
            ESP_LOGE(TAG, "radio.begin failed (%d)", st);
            return ESP_FAIL;
        }
    }
    s_radio.setDio2AsRfSwitch(true);
    s_node.setDwellTime(true, 400); /* AS923 mandatory 400 ms uplink dwell */
    ESP_LOGI(TAG, "SX1262 up (AS923, TCXO, DIO2 RF-switch)");
    return ESP_OK;
}

extern "C" esp_err_t lora_join(void)
{
    /* --- Phase 7: load OTAA creds from NVS (written by "prov creds"), or fall back
     * to compiled-in placeholders from lora_credentials.h.  Log the source so that
     * the boot-verify step can detect "[creds:NVS]" in the boot log. */
    uint64_t deveui  = LORA_DEVEUI;
    uint64_t joineui = LORA_JOINEUI;
    uint8_t  appkey[16];
    memcpy(appkey, LORA_APPKEY, sizeof(appkey));

    char s_deveui[32], s_joineui[32], s_appkey[64];
    bool nvs_ok = nvs_load_str("lorawan_deveui",  s_deveui,  sizeof(s_deveui))  &&
                  nvs_load_str("lorawan_joineui", s_joineui, sizeof(s_joineui)) &&
                  nvs_load_str("lorawan_appkey",  s_appkey,  sizeof(s_appkey));

    if (nvs_ok &&
        strlen(s_deveui) == 16 && strlen(s_joineui) == 16 && strlen(s_appkey) == 32) {
        deveui  = hex_str_to_u64(s_deveui);
        joineui = hex_str_to_u64(s_joineui);
        hex_str_to_bytes(s_appkey, appkey, 16);
        /* Boot-verify marker: "[creds:NVS]" — searched for by the WebSerial spike. */
        ESP_LOGI(TAG, "[creds:NVS] DevEUI=%016llx JoinEUI=%016llx",
                 (unsigned long long)deveui, (unsigned long long)joineui);
    } else {
        ESP_LOGW(TAG, "[creds:compiled] DevEUI=%016llx JoinEUI=%016llx "
                 "(no NVS creds — run 'prov creds' to provision)",
                 (unsigned long long)deveui, (unsigned long long)joineui);
    }
    /* --- end Phase 7 cred load --- */

    int16_t st = s_node.beginOTAA(joineui, deveui, appkey, appkey);
    if (st != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "beginOTAA failed (%d)", st);
        return ESP_FAIL;
    }

    /* Restore persisted DevNonce/JoinNonce and any prior session (survives reboots →
     * DevNonce climbs monotonically, which ChirpStack requires; restored session skips re-join). */
    uint8_t nonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
    if (nvs_load("nonces", nonces, sizeof(nonces)))
        s_node.setBufferNonces(nonces);
    uint8_t session[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
    if (nvs_load("session", session, sizeof(session)))
        s_node.setBufferSession(session);

    st = s_node.activateOTAA();
    /* Persist nonces after EVERY attempt — DevNonce was incremented even on failure, so it
     * keeps climbing across retries/reboots until it exceeds ChirpStack's stored value. */
    nvs_save("nonces", s_node.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);

    if (st == RADIOLIB_LORAWAN_NEW_SESSION || st == RADIOLIB_LORAWAN_SESSION_RESTORED) {
        nvs_save("session", s_node.getBufferSession(), RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
        /* Pin a dwell-legal, short-ToA uplink DR (AS923 DR3 = SF9BW125). Default low DR (SF12,
         * ~1.2 s ToA) overruns the TxDone wait → -5 TX_TIMEOUT. Also disable ADR for determinism.
         */
        s_node.setDatarate(3);
        s_node.setADR(false);
        ESP_LOGI(TAG, "%s; uplink DR3 (SF9)",
                 st == RADIOLIB_LORAWAN_NEW_SESSION ? "JOINED AS923 (new session)"
                                                    : "session restored (no re-join)");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "join failed (%d) — DevNonce persisted; retry will use a higher nonce", st);
    return ESP_FAIL;
}

extern "C" esp_err_t lora_send(const uint8_t *data, size_t len)
{
    int16_t st = s_node.sendReceive(data, len, 1, false);
    /* Persist session so frame counters survive reboots (replay protection). */
    nvs_save("session", s_node.getBufferSession(), RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
    if (st < RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "uplink failed (%d)", st);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "uplink OK (rx window=%d)", st);
    return ESP_OK;
}
