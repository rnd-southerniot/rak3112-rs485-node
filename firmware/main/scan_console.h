/*
 * scan_console.h — RS-485 register-discovery console commands for the Pi scanner/profiling station.
 *
 * Additive + read-only diagnostic REPL commands (scan-cfg / scan-probe / scan-ids / scan-read /
 * scan-sweep) that let a host (the Pi 5 scanner) drive the node's existing Modbus master over the
 * `esp>` console to sweep an unknown device's registers. They wrap only modbus_master_read/_probe
 * and rs485_init — never touch NVS, flash, or the LoRa path — and are registered ONLY from the
 * unprovisioned idle branch (where UART1 is free and no field app owns the bus). Gated by
 * CONFIG_APP_SCAN_CONSOLE (default n); absent from production images.
 */
#ifndef SCAN_CONSOLE_H
#define SCAN_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the scan-* commands onto the REPL created by prov_console_init(). Call once, from the
 * unprovisioned idle branch in app_main(), after prov_console_init(). */
void scan_console_register_commands(void);

#ifdef __cplusplus
}
#endif

#endif /* SCAN_CONSOLE_H */
