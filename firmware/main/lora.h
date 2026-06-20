/*
 * lora.h — C-callable LoRaWAN (RadioLib/SX1262) API for app_main.c (Phase 5, ADR-003/004).
 * AS923-1 OTAA. Credentials come from the gitignored generated lora_credentials.h.
 */
#ifndef LORA_H
#define LORA_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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
