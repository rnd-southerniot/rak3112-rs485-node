/*
 * prov_console.h — NVS provisioning REPL console (Phase 7).
 *
 * Implements the frozen /v1/provisioning-protocol contract from
 * api/routers/builds.py (lines 155-243):
 *
 *   prov modbus <baud> <parity> <stopBits> <slaveId>
 *     -> NVS keys: modbus_baud, modbus_parity, modbus_stop, modbus_slave
 *
 *   prov creds <devEui> <joinEui> <appKey>
 *     -> NVS keys: lorawan_deveui, lorawan_joineui, lorawan_appkey
 *
 *   prov show
 *     -> prints NVS state; appKey ALWAYS redacted (T-02-02 / guardrail §3 #4)
 *
 * Console interface: USB-Serial-JTAG at 115200, prompt "esp> ".
 * Guardrail: appKey must NEVER appear in console output — not echoed,
 * not printed in logs, not stored in any plaintext output path.
 */
#ifndef PROV_CONSOLE_H
#define PROV_CONSOLE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise and start the NVS provisioning REPL on USB-Serial-JTAG.
 *
 * Registers: prov modbus / prov creds / prov show.
 * Must be called AFTER nvs_flash_init() (i.e. after lora_init()).
 * The REPL runs as a separate FreeRTOS task — does NOT block the caller.
 *
 * @return ESP_OK on success, propagated error otherwise.
 */
esp_err_t prov_console_init(void);

#ifdef __cplusplus
}
#endif

#endif /* PROV_CONSOLE_H */
