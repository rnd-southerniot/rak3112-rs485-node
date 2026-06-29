/*
 * ota.h — dual-slot OTA helpers (Phase 7c).
 *
 * Partition layout (partitions.csv): ota_0 / ota_1 with otadata + bootloader app rollback.
 * The network downloader (WiFi/HTTPS or FUOTA) is deferred (OQ-10); this module provides the
 * boot-slot reporting, the mark-valid-on-health step, and console commands to activate a slot
 * whose image was staged over USB (parttool) — exercising the real rollback state machine.
 */
#ifndef OTA_H
#define OTA_H

#ifdef __cplusplus
extern "C" {
#endif

/* Log the running slot + its OTA state + the build tag (call early at boot). */
void ota_log_boot(void);

/* If the running image is PENDING_VERIFY (just OTA-activated), mark it valid → cancels the pending
 * rollback. Call once the node has reached a healthy state. No-op for a directly-flashed image. */
void ota_mark_valid(void);

/* Register the ota-status / ota-activate console commands (after the REPL is initialised). */
void ota_register_commands(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */
