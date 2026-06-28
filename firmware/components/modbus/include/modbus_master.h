/*
 * modbus_master.h — on-target Modbus RTU master over the Phase 4 rs485 transport (Phase 6b).
 *
 * Drives the pure modbus_rtu framing across an already-initialised RS-485 UART. Includes a
 * bus-scan / device-discovery helper used for industrial bring-up: probe a range of unit IDs,
 * classifying "present" as ANY CRC-valid reply with the matching ID — a Modbus *exception*
 * still proves the device is there. Only a timeout means absent.
 */
#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#include <stdint.h>

#include "driver/uart.h"
#include "modbus_rtu.h"

typedef enum {
    MB_PROBE_ABSENT = 0,        /* no/short response within the timeout */
    MB_PROBE_PRESENT_DATA,      /* valid read response carrying register data */
    MB_PROBE_PRESENT_EXCEPTION, /* device replied with a Modbus exception (still present) */
    MB_PROBE_BADFRAME,          /* bytes received but CRC/ID/func mismatch (noise / wrong baud) */
} mb_probe_t;

typedef struct {
    mb_probe_t result;
    uint16_t reg0;       /* first register, when MB_PROBE_PRESENT_DATA */
    uint8_t exception;   /* exception code, when MB_PROBE_PRESENT_EXCEPTION */
    int rx_len;          /* bytes received (diagnostic) */
    uint32_t latency_ms; /* request -> response latency */
} mb_probe_info_t;

/*
 * One read transaction (FC03 holding / FC04 input). `regs` must hold `qty` words. Returns the
 * modbus_status_t from parsing (MODBUS_OK on success; MODBUS_ERR_EXCEPTION with *exc set; or a
 * framing/timeout error — MODBUS_ERR_SHORT is also returned on a read timeout).
 */
modbus_status_t modbus_master_read(uart_port_t port, uint8_t unit_id, uint8_t func, uint16_t addr,
                                   uint16_t qty, uint32_t timeout_ms, uint16_t *regs, uint8_t *exc);

/*
 * One write-single-register transaction (FC06). Writes `value` to holding register `addr` on
 * `unit_id`, then validates the slave's echo. Returns MODBUS_OK on a matching echo;
 * MODBUS_ERR_EXCEPTION (with *exc set) if the slave rejected it; or a framing/timeout error.
 *
 * WARNING: on a motor drive, FC06 to a control/enable register COMMANDS MOTION. Only call this
 * behind an explicit hardware-safety gate (motor secured, unloaded, current-limited).
 */
modbus_status_t modbus_master_write_single(uart_port_t port, uint8_t unit_id, uint16_t addr,
                                           uint16_t value, uint32_t timeout_ms, uint8_t *exc);

/*
 * One write-multiple-registers transaction (FC16). Writes `qty` registers from `regs` to
 * consecutive addresses starting at `addr` (used for 32-bit values across a register pair), then
 * validates the slave's addr/qty echo. Returns MODBUS_OK / MODBUS_ERR_EXCEPTION / framing error.
 *
 * WARNING: on a motor drive this COMMANDS MOTION (e.g. run-to-position). Gate with hardware safety.
 */
modbus_status_t modbus_master_write_multi(uart_port_t port, uint8_t unit_id, uint16_t addr,
                                          uint16_t qty, const uint16_t *regs, uint32_t timeout_ms,
                                          uint8_t *exc);

/* Probe a single unit ID (FC03, 1 register @ `addr`). Fills *info if non-null. */
mb_probe_t modbus_master_probe(uart_port_t port, uint8_t unit_id, uint16_t addr,
                               uint32_t timeout_ms, mb_probe_info_t *info);

/*
 * Scan unit IDs [id_lo..id_hi] on an already-initialised RS-485 `port`. Logs only the present /
 * garbage IDs plus a one-line summary (quiet on absent, so a baud x parity sweep stays readable).
 * `baud_label` and `framing` (e.g. "8E1") are for log output only. Returns the number of present
 * devices (valid data or exception).
 */
int modbus_master_scan(uart_port_t port, uint8_t id_lo, uint8_t id_hi, uint16_t probe_addr,
                       uint32_t timeout_ms, int baud_label, const char *framing);

#endif /* MODBUS_MASTER_H */
