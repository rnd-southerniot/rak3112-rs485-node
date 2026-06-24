/*
 * meter.c — Modbus field-device reads + simulated data generators. See meter.h.
 */
#include "meter.h"

#include <math.h>

#include "modbus_master.h"
#include "modbus_rtu.h"

/* One MFM384 float32 measurand: FC04 input registers, 2 regs, CDAB word order. */
static esp_err_t read_f32_cdab(uart_port_t port, uint8_t unit, uint16_t reg, float *out)
{
    uint16_t regs[2] = {0, 0};
    uint8_t exc = 0;
    const modbus_status_t st =
        modbus_master_read(port, unit, MODBUS_FC_READ_INPUT_REGISTERS, reg, 2, 500, regs, &exc);
    if (st != MODBUS_OK) {
        return ESP_FAIL;
    }
    *out = modbus_regs_to_f32(regs, MODBUS_WORD_ORDER_CDAB);
    return ESP_OK;
}

esp_err_t meter_read_mfm384(uart_port_t port, uint8_t unit, mfm384_sample_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    if (read_f32_cdab(port, unit, MFM384_REG_V1N, &out->v1n) != ESP_OK)
        err = ESP_FAIL;
    if (read_f32_cdab(port, unit, MFM384_REG_V2N, &out->v2n) != ESP_OK)
        err = ESP_FAIL;
    if (read_f32_cdab(port, unit, MFM384_REG_V3N, &out->v3n) != ESP_OK)
        err = ESP_FAIL;
    if (read_f32_cdab(port, unit, MFM384_REG_TOTAL_KW, &out->total_kw) != ESP_OK)
        err = ESP_FAIL;
    if (read_f32_cdab(port, unit, MFM384_REG_TOTAL_KWH, &out->total_kwh) != ESP_OK)
        err = ESP_FAIL;
    if (read_f32_cdab(port, unit, MFM384_REG_FREQ, &out->freq) != ESP_OK)
        err = ESP_FAIL;
    if (read_f32_cdab(port, unit, MFM384_REG_AVG_PF, &out->avg_pf) != ESP_OK)
        err = ESP_FAIL;
    return err;
}

esp_err_t meter_read_rsfsjt(uart_port_t port, uint8_t unit, rsfsjt_sample_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t reg = 0;
    uint8_t exc = 0;
    const modbus_status_t st = modbus_master_read(port, unit, MODBUS_FC_READ_HOLDING_REGISTERS,
                                                  RSFSJT_REG_WIND, 1, 500, &reg, &exc);
    if (st != MODBUS_OK) {
        return ESP_FAIL;
    }
    out->wind_mps = (float)reg / 10.0f; /* RS-FSJT: raw/10 = m/s (MODBUS_MAP); confirm on bench */
    return ESP_OK;
}

void meter_sim_mfm384(uint32_t tick, mfm384_sample_t *out)
{
    if (out == NULL) {
        return;
    }
    const float ph = (float)tick * 0.10f;
    out->v1n = 230.0f + 3.0f * sinf(ph);
    out->v2n = 230.0f + 3.0f * sinf(ph + 2.0944f); /* +120 deg */
    out->v3n = 230.0f + 3.0f * sinf(ph + 4.1888f); /* +240 deg */
    out->total_kw = 5.0f + 1.5f * sinf(ph * 0.5f);
    /* Energy accumulates: total_kw averaged over a nominal 60 s sample interval. */
    static float kwh = 1000.0f;
    kwh += out->total_kw * (60.0f / 3600.0f);
    out->total_kwh = kwh;
    out->freq = 50.0f + 0.05f * sinf(ph);
    out->avg_pf = 0.95f + 0.03f * sinf(ph * 0.30f);
}

void meter_sim_rsfsjt(uint32_t tick, rsfsjt_sample_t *out)
{
    if (out == NULL) {
        return;
    }
    const float ph = (float)tick * 0.20f;
    out->wind_mps = 5.0f + 4.5f * sinf(ph); /* 0.5 .. 9.5 m/s */
}
