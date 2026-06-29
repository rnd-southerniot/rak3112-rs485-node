/*
 * provisioning.h — on-device provisioning console (7d).
 *
 * Starts a USB-Serial-JTAG REPL (its own task) with prov-* commands that write the NVS 'prov'
 * namespace additively (credentials + Modbus runtime config), preserving the 'lorawan'
 * nonces/session, then restart into field mode. Available in field mode too (re-point a deployed
 * node without erasing NVS). Driven by hand or by tools/provision_nvs.py.
 */
#ifndef PROVISIONING_H
#define PROVISIONING_H

#ifdef __cplusplus
extern "C" {
#endif

/* Set up + start the provisioning REPL on its own task, then return (non-blocking). */
void provisioning_console_start(void);

#ifdef __cplusplus
}
#endif

#endif /* PROVISIONING_H */
