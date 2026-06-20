#include "lora.h"

#include <RadioLib.h>

#include "EspHalS3.h"
#include "esp_log.h"
#include "gpio_remap.h"
#include "lora_credentials.h"
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
    ESP_LOGI(TAG, "OTAA: DevEUI=%016llx JoinEUI=%016llx", (unsigned long long)LORA_DEVEUI,
             (unsigned long long)LORA_JOINEUI);
    int16_t st =
        s_node.beginOTAA(LORA_JOINEUI, LORA_DEVEUI, (uint8_t *)LORA_APPKEY, (uint8_t *)LORA_APPKEY);
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
