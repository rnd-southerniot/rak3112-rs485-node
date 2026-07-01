#include "lora.h"

#include <RadioLib.h>

#include "EspHalS3.h"
#include "esp_log.h"
#include "gpio_remap.h"
/* Real credentials header is gitignored (holds the AppKey secret, generated from firmware/.env).
 * Fall back to the committed placeholder template when it's absent (clean clone / CI build-check)
 * so the project always builds; placeholder creds cannot join a network. */
#if defined(__has_include) && __has_include("lora_credentials.h")
#include "lora_credentials.h"
#else
#pragma message(                                                                                   \
    "lora_credentials.h not found — using PLACEHOLDER OTAA creds (build-check only; copy lora_credentials.h.example to lora_credentials.h and fill real keys to join)")
#include "lora_credentials.h.example"
#endif
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "lora";
#define LORA_NVS_NS "lorawan" /* nonces + session (survive provisioning) */
#define PROV_NVS_NS "prov"    /* provisioned OTAA creds (7d) — priority over compiled */

/* Provisioned credentials, lazily loaded from NVS once. If absent, the compiled
 * lora_credentials.h values (real on a filled-in dev tree, all-zero placeholder otherwise) apply.
 */
static bool s_creds_loaded = false;
static bool s_nvs_creds = false;
static uint64_t s_deveui = 0;
static uint64_t s_joineui = 0;
static uint8_t s_appkey[16] = {0};

static void load_prov_creds(void)
{
    if (s_creds_loaded) {
        return;
    }
    s_creds_loaded = true;
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint64_t de = 0, je = 0;
        size_t n = sizeof(s_appkey);
        if (nvs_get_u64(h, "deveui", &de) == ESP_OK && nvs_get_u64(h, "joineui", &je) == ESP_OK &&
            nvs_get_blob(h, "appkey", s_appkey, &n) == ESP_OK && n == sizeof(s_appkey)) {
            s_deveui = de;
            s_joineui = je;
            s_nvs_creds = true;
        }
        nvs_close(h);
    }
}

/*
 * DevEUI derived from the ESP32-S3 factory MAC (EUI-48 -> EUI-64 with FF:FE inserted). The SX1262
 * radio has no DevEUI, so this is the board's stable identity: MAC 3c:dc:75:6f:85:dc yields
 * 3cdc75fffe6f85dc. Used as the DevEUI default (NVS-provisioned creds still take priority); the CRM
 * reads it back at the factory step to register the device + mint the AppKey.
 */
static uint64_t deveui_from_mac(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    return ((uint64_t)mac[0] << 56) | ((uint64_t)mac[1] << 48) | ((uint64_t)mac[2] << 40) |
           ((uint64_t)0xFFu << 32) | ((uint64_t)0xFEu << 24) | ((uint64_t)mac[3] << 16) |
           ((uint64_t)mac[4] << 8) | (uint64_t)mac[5];
}

extern "C" uint64_t lora_deveui_mac(void)
{
    return deveui_from_mac();
}

extern "C" uint64_t lora_deveui(void)
{
    load_prov_creds();
    return s_nvs_creds ? s_deveui : deveui_from_mac();
}

extern "C" bool lora_is_provisioned(void)
{
    load_prov_creds();
    if (s_nvs_creds) {
        return true;
    }
    for (size_t i = 0; i < sizeof(LORA_APPKEY); ++i) {
        if (LORA_APPKEY[i]) {
            return true; /* a real compiled key is acceptable */
        }
    }
    return false;
}

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
    load_prov_creds();
    /* DevEUI default is MAC-derived (the SX1262 has no DevEUI); NVS-provisioned creds still win. */
    const uint64_t deveui = s_nvs_creds ? s_deveui : deveui_from_mac();
    const uint64_t joineui = s_nvs_creds ? s_joineui : (uint64_t)LORA_JOINEUI;
    uint8_t *appkey = s_nvs_creds ? s_appkey : (uint8_t *)LORA_APPKEY;
    ESP_LOGI(TAG, "OTAA: DevEUI=%016llx JoinEUI=%016llx [creds:%s, DevEUI:%s]",
             (unsigned long long)deveui, (unsigned long long)joineui,
             s_nvs_creds ? "NVS" : "compiled", s_nvs_creds ? "NVS" : "MAC");
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
