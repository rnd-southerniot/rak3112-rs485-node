/* SPDX-License-Identifier: MIT */
/* Honeywell EEM400C-D-MO — Modbus reader and deterministic simulator.
 *
 * Wire authority: honeywell_data_read_only_with_rak4630.ino
 *   Baud 19200, 8E1, FC03 (holding registers), slave 0x01.
 * Cross-check: datasheet P+P26/595 EN01/05.2013, §"Registers" pp.6-7.
 *
 * CONFLICT LOG (Arduino wins per project rule):
 *   [C1] T2 registers (addrs 31, 33): Arduino reads as T2 kWh; datasheet
 *        marks R32-35 "Not Used". We follow Arduino.
 *   [C2] Current scaling: Arduino divides by 10 (SERIAL_8E1 code path).
 *        Datasheet "Ex: 314 = 314 A" implies ×1 for non-5/5 CTs; "except
 *        5/5 = 10^-1 A" implies ÷10 for 5/5. Arduino uses ÷10 — we match.
 *   [C3] Power scaling: datasheet multiplier 10^-1 (÷10 → kW). Arduino
 *        switch uses (i % 5) which maps per-phase P/Q to case 2/3 → ÷100.
 *        This is an i%5 wrap bug (15 fields + 2 totals; modulo wraps wrong
 *        for indices 15,16). We use the datasheet multiplier (÷10) which is
 *        physically correct (e.g. raw 1545 → 154.5 kW per datasheet, not
 *        15.45 kW). Flagged — operator should verify against live meter.   */

#include <math.h>
#include <stdint.h>
#include "meter.h"
#include "modbus_master.h"

/* FC03: "Only Read Holding Registers [03]… instructions are recognized."
 * Datasheet §"Data transmission". Arduino: node.readHoldingRegisters().     */
#define EEM400_FC    0x03u

/* Energy block: R28-29=T1 total, R30-31=T1 partial, R32-33=T2 total [C1].
 * Protocol address = R-1 per datasheet §"Data transmission".                */
#define EEM400_ADDR_ENERGY  27u   /* Datasheet R28, 0-based: 28-1=27        */
#define EEM400_QTY_ENERGY    6u   /* 3 × uint32 = 3 × 2 registers           */

/* Measurement block: R36 (URMS ph1) through R52 (QRMS total), qty=17.       */
#define EEM400_ADDR_MEAS    35u   /* Datasheet R36, 0-based: 36-1=35        */
#define EEM400_QTY_MEAS     17u

/* Raw-to-engineering scalars (datasheet §"Registers" pp.6-7).
 * Voltage : raw register value = V directly (multiplier 1).
 * Current : raw × 0.1 = A for 5/5 CT (see [C2] above).
 * Power   : raw × 0.1 = kW or kvar (multiplier 10^-1, see [C3] above).
 * PF      : raw × 0.01 (multiplier 10^-2, Ex: 67 → 0.67).
 * Energy  : u32 raw × 0.1 = kWh (multiplier 10^-1, Ex: 912351 → 91235.1). */
#define EEM400_SCALE_V    1.0f
#define EEM400_SCALE_I    0.1f
#define EEM400_SCALE_P    0.1f
#define EEM400_SCALE_Q    0.1f
#define EEM400_SCALE_COS  0.01f
#define EEM400_SCALE_KWH  0.1f

/* Sim constants — each tick represents one 30-second sample interval. */
#define SIM_TICK_HOURS  (30.0 / 3600.0)

/* Datasheet §"Registers": "for double registers the high register is sent
 * first (big_Endian)".  Arduino read_energy_counter confirms:
 *   uint32_t fullValue = ((uint32_t)highWord << 16) | lowWord;             */
static uint32_t regs_to_u32_abcd(const uint16_t *r)
{
    return ((uint32_t)r[0] << 16) | r[1];
}

/* ============================================================== Real reader */
esp_err_t meter_read_eem400(uart_port_t port, uint8_t unit, eem400_sample_t *out)
{
    uint8_t  exc;
    uint16_t regs_e[EEM400_QTY_ENERGY];
    uint16_t regs_m[EEM400_QTY_MEAS];

    /* 1. Energy counters: R28-33, six 16-bit registers → three uint32 values.
     *    Arduino: separate read_energy_counter() calls; consolidated here to
     *    one bus transaction (Honeywell allows up to 20 regs per FC03 read). */
    if (modbus_master_read(port, unit, EEM400_FC,
                           EEM400_ADDR_ENERGY, EEM400_QTY_ENERGY,
                           500u, regs_e, &exc) != MODBUS_OK) {
        return ESP_FAIL;
    }
    out->t1_total_kwh = (float)regs_to_u32_abcd(&regs_e[0]) * EEM400_SCALE_KWH;
    out->t1_part_kwh  = (float)regs_to_u32_abcd(&regs_e[2]) * EEM400_SCALE_KWH;
    out->t2_total_kwh = (float)regs_to_u32_abcd(&regs_e[4]) * EEM400_SCALE_KWH; /* [C1] */

    /* 2. Measurement block: R36-R52, seventeen single 16-bit registers.
     *    Arduino: read_modbus_registers_block(0x0023, 17, buf).              */
    if (modbus_master_read(port, unit, EEM400_FC,
                           EEM400_ADDR_MEAS, EEM400_QTY_MEAS,
                           500u, regs_m, &exc) != MODBUS_OK) {
        return ESP_FAIL;
    }

    /* Offsets within block, datasheet §"Registers" pp.6-7 (addr = R-1):    */
    out->v1      = (float)regs_m[ 0] * EEM400_SCALE_V;   /* R36 URMS ph1 [V]    */
    out->i1      = (float)regs_m[ 1] * EEM400_SCALE_I;   /* R37 IRMS ph1 [A]    */
    out->p1      = (float)regs_m[ 2] * EEM400_SCALE_P;   /* R38 PRMS ph1 [kW]   */
    out->q1      = (float)regs_m[ 3] * EEM400_SCALE_Q;   /* R39 QRMS ph1 [kvar] */
    out->cos1    = (float)regs_m[ 4] * EEM400_SCALE_COS; /* R40 cos phi 1       */
    out->v2      = (float)regs_m[ 5] * EEM400_SCALE_V;   /* R41 URMS ph2 [V]    */
    out->i2      = (float)regs_m[ 6] * EEM400_SCALE_I;   /* R42 IRMS ph2 [A]    */
    out->p2      = (float)regs_m[ 7] * EEM400_SCALE_P;   /* R43 PRMS ph2 [kW]   */
    out->q2      = (float)regs_m[ 8] * EEM400_SCALE_Q;   /* R44 QRMS ph2 [kvar] */
    out->cos2    = (float)regs_m[ 9] * EEM400_SCALE_COS; /* R45 cos phi 2       */
    out->v3      = (float)regs_m[10] * EEM400_SCALE_V;   /* R46 URMS ph3 [V]    */
    out->i3      = (float)regs_m[11] * EEM400_SCALE_I;   /* R47 IRMS ph3 [A]    */
    out->p3      = (float)regs_m[12] * EEM400_SCALE_P;   /* R48 PRMS ph3 [kW]   */
    out->q3      = (float)regs_m[13] * EEM400_SCALE_Q;   /* R49 QRMS ph3 [kvar] */
    out->cos3    = (float)regs_m[14] * EEM400_SCALE_COS; /* R50 cos phi 3       */
    out->p_total = (float)regs_m[15] * EEM400_SCALE_P;   /* R51 PRMS total [kW] */
    out->q_total = (float)regs_m[16] * EEM400_SCALE_Q;   /* R52 QRMS total [kvar]*/

    return ESP_OK;
}

/* ============================================================= Simulator    */
/* Pure, no I/O, deterministic from `tick` (one tick = 30 s interval).
 * Nominal: 3×230 V, 25 A, ~5 kW per phase, PF ≈ 0.98.
 * Slow sinusoids use different prime-ish frequencies so readings don't
 * correlate visually — same technique as meter_sim_mfm384.                  */
void meter_sim_eem400(uint32_t tick, eem400_sample_t *out)
{
    static double s_t1_kwh  = 0.0;
    static double s_t1_part = 0.0;
    static double s_t2_kwh  = 0.0;

    /* Scale tick to a phase angle: one full sine cycle per simulated hour.  */
    float ph = (float)tick * (2.0f * (float)M_PI * 30.0f / 3600.0f);

    /* 120° offsets for the three phases. */
    float ph2 = ph + (float)(2.0 * M_PI / 3.0);
    float ph3 = ph + (float)(4.0 * M_PI / 3.0);

    /* Datasheet operating range: 3×230/400 V, CT up to 1500 A.
     * Sim stays in a plausible LV building-services envelope.               */
    out->v1 = 230.0f + 5.0f  * sinf(ph  * 0.11f);  /* 225..235 V */
    out->v2 = 230.0f + 5.0f  * sinf(ph2 * 0.11f);
    out->v3 = 230.0f + 5.0f  * sinf(ph3 * 0.11f);

    out->i1 = 25.0f  + 3.0f  * sinf(ph  * 0.13f);  /* 22..28 A  */
    out->i2 = 25.0f  + 3.0f  * sinf(ph2 * 0.13f);
    out->i3 = 25.0f  + 3.0f  * sinf(ph3 * 0.13f);

    out->p1 = 5.0f   + 0.5f  * sinf(ph  * 0.07f);  /* 4.5..5.5 kW   */
    out->p2 = 5.0f   + 0.5f  * sinf(ph2 * 0.07f);
    out->p3 = 5.0f   + 0.5f  * sinf(ph3 * 0.07f);

    out->q1 = 1.0f   + 0.2f  * sinf(ph  * 0.09f);  /* 0.8..1.2 kvar */
    out->q2 = 1.0f   + 0.2f  * sinf(ph2 * 0.09f);
    out->q3 = 1.0f   + 0.2f  * sinf(ph3 * 0.09f);

    out->p_total = out->p1 + out->p2 + out->p3;
    out->q_total = out->q1 + out->q2 + out->q3;

    /* cos phi = P / √(P² + Q²) — derived, not read from a separate register. */
    out->cos1 = out->p1 / sqrtf(out->p1 * out->p1 + out->q1 * out->q1);
    out->cos2 = out->p2 / sqrtf(out->p2 * out->p2 + out->q2 * out->q2);
    out->cos3 = out->p3 / sqrtf(out->p3 * out->p3 + out->q3 * out->q3);

    /* Energy accumulators: P [kW] × Δt [h] = ΔkWh.
     * T2 simulates an off-peak tariff at 30% of T1 consumption.            */
    s_t1_kwh  += (double)out->p_total  * SIM_TICK_HOURS;
    s_t1_part += (double)out->p_total  * SIM_TICK_HOURS;
    s_t2_kwh  += (double)(out->p_total * 0.3f) * SIM_TICK_HOURS;

    out->t1_total_kwh = (float)s_t1_kwh;
    out->t1_part_kwh  = (float)s_t1_part;
    out->t2_total_kwh = (float)s_t2_kwh;
}
