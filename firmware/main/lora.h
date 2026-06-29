/*
 * lora.h — C-callable LoRaWAN (RadioLib/SX1262) API for app_main.c (Phase 5, ADR-003/004).
 * AS923-1 OTAA. Credentials come from the gitignored generated lora_credentials.h.
 */
#ifndef LORA_H
#define LORA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True once OTAA credentials are available: the NVS 'prov' namespace (set by the provisioning
 * console / tools/provision_nvs.py) takes priority; otherwise a real compiled key counts. A
 * placeholder (all-zero) compiled key with empty NVS returns false → the node awaits provisioning.
 */
bool lora_is_provisioned(void);

/* radio.begin (TCXO 1.8V, DIO2 RF switch, AS923 dwell). */
esp_err_t lora_init(void);
/* OTAA join (beginOTAA + activateOTAA). Blocks. */
esp_err_t lora_join(void);
/* One unconfirmed uplink on fPort 1. */
esp_err_t lora_send(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LORA_H */
