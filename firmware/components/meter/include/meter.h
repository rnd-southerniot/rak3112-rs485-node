/*
 * meter.h — sample a Modbus field device (SELEC MFM384 / RS-FSJT-N01), or synthesize simulated
 * data. The real reads drive the Phase 6 modbus_master over the Phase 4 rs485 transport; the sim
 * generators let the full sample -> payload -> LoRaWAN uplink path be exercised with no hardware.
 */
#ifndef METER_H
#define METER_H

#include "device_profile.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "telemetry.h"

/*
 * Read the MFM384 measurands carried in the uplink: V1N/V2N/V3N, Total kW, Total kWh, Freq, Avg PF.
 * FC04 input registers, float32 CDAB, on `port` (already rs485_init'd at 9600 8N1). Returns ESP_OK
 * only if every field read succeeds; on any failure returns ESP_FAIL and leaves partial values.
 */
esp_err_t meter_read_mfm384(uart_port_t port, uint8_t unit, mfm384_sample_t *out);

/* Read RS-FSJT wind speed: FC03 holding reg 0, uint16, value/10 = m/s (4800 8N1). */
esp_err_t meter_read_rsfsjt(uart_port_t port, uint8_t unit, rsfsjt_sample_t *out);

/*
 * Fill *out with plausible, time-varying simulated data (deterministic in `tick`, the sample
 * index). MFM384: ~230 V 3-phase, ~5 kW, slowly accumulating kWh, ~50 Hz, ~0.95 PF.
 * RS-FSJT: 0.5..9.5 m/s. No SDK calls — safe anywhere.
 */
void meter_sim_mfm384(uint32_t tick, mfm384_sample_t *out);
void meter_sim_rsfsjt(uint32_t tick, rsfsjt_sample_t *out);

/*
 * Generic profile-driven read (ADR-006 increment 3): read each of `p->meas` over Modbus and decode
 * into `values` (engineering units; `values` must hold >= p->n_meas floats). Each measurand uses
 * its own fc/type/word-order/scale (fc 0 falls back to p->default_fc). Returns ESP_OK only if every
 * measurand read succeeds; on any failure that value is set 0 and ESP_FAIL is returned (caller
 * flags the uplink STALE). `unit` is the Modbus slave ID (discovered by scan / provisioned, not in
 * `p`).
 */
esp_err_t meter_read_profile(uart_port_t port, uint8_t unit, const device_profile_t *p,
                             float *values, size_t n_values);

/*
 * Discover the Modbus slave ID on the bus (ADR-006 increment 4). Probes unit IDs [id_lo..id_hi]
 * with the profile's scan params (scan_fc/scan_reg/scan_qty); a CRC-valid reply OR a Modbus
 * exception counts as present (only a timeout = absent). Returns the first responding ID, or -1 if
 * none answered. Probe a single ID by passing id_lo == id_hi.
 */
int meter_scan_for_unit(uart_port_t port, const device_profile_t *p, uint8_t id_lo, uint8_t id_hi,
                        uint32_t timeout_ms);

#endif /* METER_H */
