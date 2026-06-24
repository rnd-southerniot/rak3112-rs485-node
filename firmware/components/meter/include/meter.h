/*
 * meter.h — sample a Modbus field device (SELEC MFM384 / RS-FSJT-N01), or synthesize simulated
 * data. The real reads drive the Phase 6 modbus_master over the Phase 4 rs485 transport; the sim
 * generators let the full sample -> payload -> LoRaWAN uplink path be exercised with no hardware.
 */
#ifndef METER_H
#define METER_H

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

#endif /* METER_H */
