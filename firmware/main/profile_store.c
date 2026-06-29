/* profile_store.c — NVS persistence for a device profile blob (ADR-006 increment 2b). */
#include "profile_store.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "profile_store";
#define PROV_NS "prov"
#define PROFILE_KEY "profile"

bool profile_store_save(const uint8_t *blob, size_t len)
{
    if (blob == NULL || len == 0 || len > PROFILE_BLOB_MAX) {
        return false;
    }
    /* Validate before persisting — never store a blob the firmware can't load. */
    static dp_profile_storage_t scratch;
    if (!dp_deserialize(blob, len, &scratch)) {
        ESP_LOGW(TAG, "rejected invalid profile blob (%u B)", (unsigned)len);
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(PROV_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, PROFILE_KEY, blob, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "stored profile blob: %u B, device_byte=0x%02X, %u measurands", (unsigned)len,
                 scratch.profile.device_byte, scratch.profile.n_meas);
    }
    return err == ESP_OK;
}

bool profile_store_load(dp_profile_storage_t *store)
{
    if (store == NULL) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(PROV_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    static uint8_t blob[PROFILE_BLOB_MAX];
    size_t len = sizeof(blob);
    const esp_err_t err = nvs_get_blob(h, PROFILE_KEY, blob, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return false;
    }
    return dp_deserialize(blob, len, store);
}

bool profile_store_present(uint8_t *device_byte_out, uint8_t *n_meas_out)
{
    static dp_profile_storage_t scratch;
    if (!profile_store_load(&scratch)) {
        return false;
    }
    if (device_byte_out != NULL) {
        *device_byte_out = scratch.profile.device_byte;
    }
    if (n_meas_out != NULL) {
        *n_meas_out = scratch.profile.n_meas;
    }
    return true;
}
