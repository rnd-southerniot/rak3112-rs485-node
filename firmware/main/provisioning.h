/*
 * provisioning.h — on-device provisioning commands (7d + ADR-006).
 *
 * prov-lorawan/-modbus/-profile/-show/-clear/-done write the NVS 'prov' namespace additively
 * (credentials + Modbus runtime config + a device-profile blob), preserving the 'lorawan'
 * nonces/session, then restart into field mode. Driven by hand or by tools/provision_nvs.py.
 *
 * The single USB-Serial-JTAG REPL is owned by prov_console.cpp (which also hosts the CRM-contract
 * `prov modbus|creds|show` commands); this module only registers its commands onto it.
 */
#ifndef PROVISIONING_H
#define PROVISIONING_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the prov-* commands on the already-created esp_console (no REPL create/start). Call
 * after esp_console_new_repl_* and before esp_console_start_repl (see prov_console_init). */
void provisioning_register_commands(void);

#ifdef __cplusplus
}
#endif

#endif /* PROVISIONING_H */
