/* SPDX-License-Identifier: MIT */
/* Selec MFM384-C — 3-phase multifunction energy meter.
 * Modbus reader and deterministic simulator.
 *
 * Wire authority: MFM384_with_esp.ino
 *   Baud 9600, 8N1, FC04 (input registers), slave 1.
 * Cross-check: datasheet OP INST MFM384-C OP2042-V01, pp.3-4.
 *
 * Word order — CDAB (Mid Little Endian, DS §"Data Format: Mid Little Endian"):
 *   Register N   = low  word (bytes C, D)
 *   Register N+1 = high word (bytes A, B)
 *   u32 = (high << 16) | low → IEEE 754 float.
 *   Arduino confirms: lo=buf[0], hi=buf[1]; u32=(hi<<16)|lo.
 *   DS default is ABCD; the proven code implies config reg 40070 = 0 (CDAB).
 *
 * CONFLICT LOG (Arduino wins on addresses / FC / word order / data type;
 *               datasheet wins on measurand names):
 *
 *   [C1] Arduino reg 42 labeled "kVAr Phase 3" — DS 30042 = "Total KW".
 *        kVAr3 is at DS 30040 (reg 40), not read by Arduino.  We read reg 42
 *        as the Arduino does and name it total_kw per the DS.
 *
 *   [C2] Arduino reg 44 labeled "Total kW" — DS 30044 = "Total KVA".
 *        Same root cause: Arduino labels are shifted by one entry relative to
 *        the DS table.  We read reg 44 and name it total_kva per the DS.
 *
 *   [C3] I2 (DS 30018, reg 18) is present in the meter but omitted by the
 *        Arduino reference.  Not included here — callers can add a third
 *        read if needed.                                                     */

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "meter.h"
#include "modbus_master.h"

/* FC04 (Read Input Registers). Arduino: node.readInputRegisters(). */
#define MFM384_FC    0x04u

/* Group A: V, I, kW — regs 0..29 (qty=30).
 * Covers V1N(0), V2N(2), V3N(4), AvgLN(6), V12(8), V23(10), V31(12),
 * AvgLL(14), I1(16), I2(18 unused), I3(20), AvgI(22),
 * kW1(24), kW2(26), kW3(28).  Contiguous — one transaction.              */
#define MFM384_ADDR_GRP_A    0u
#define MFM384_QTY_GRP_A    30u

/* Group B: totals + PF + freq + kWh — regs 42..59 (qty=18).
 * Total KW(42)[C1], Total KVA(44)[C2], Total KVAr(46),
 * PF1(48), PF2(50), PF3(52), AvgPF(54), Freq(56), kWh(58).              */
#define MFM384_ADDR_GRP_B   42u
#define MFM384_QTY_GRP_B    18u

/* Sim constants. */
#define SIM_TICK_HOURS  (30.0 / 3600.0)   /* 30-second sample interval */


/* CDAB float decode: reg[0] = low word (C,D), reg[1] = high word (A,B).
 * DS §"Data Format: Mid Little Endian" example: 1234.12 kWh = 0x449A43D7;
 * register 30090 holds 0x43D7 (C,D = low), register 30091 holds 0x449A (A,B = high).
 * Arduino confirms: lo=buf[0], hi=buf[1]; u32=(hi<<16)|lo.                */
static float regs_to_f32_cdab(const uint16_t *r)
{
    uint32_t u32 = ((uint32_t)r[1] << 16) | r[0];
    float f;
    memcpy(&f, &u32, sizeof(f));
    return f;
}

/* ============================================================= Real reader */
esp_err_t meter_read_mfm384(uart_port_t port, uint8_t unit, mfm384_sample_t *out)
{
    uint8_t  exc;
    uint16_t ga[MFM384_QTY_GRP_A];
    uint16_t gb[MFM384_QTY_GRP_B];

    /* Group A: V, I, kW (regs 0-29).  Arduino reads each pair individually;
     * consolidated here to one transaction (meter has no stated block limit, and
     * qty=30 is well within the Modbus spec 125-register max for FC04).      */
    if (modbus_master_read(port, unit, MFM384_FC,
                           MFM384_ADDR_GRP_A, MFM384_QTY_GRP_A,
                           500u, ga, &exc) != MODBUS_OK) return ESP_FAIL;

    /* Offsets within ga[], each float = 2 registers, CDAB.
     * DS §"Modbus Register Addresses List" pp.3, 30000-series table.        */
    out->v1n      = regs_to_f32_cdab(&ga[ 0]);  /* reg 0  Voltage V1N        */
    out->v2n      = regs_to_f32_cdab(&ga[ 2]);  /* reg 2  Voltage V2N        */
    out->v3n      = regs_to_f32_cdab(&ga[ 4]);  /* reg 4  Voltage V3N        */
    out->v_avg_ln = regs_to_f32_cdab(&ga[ 6]);  /* reg 6  Average Voltage LN */
    out->v12      = regs_to_f32_cdab(&ga[ 8]);  /* reg 8  Voltage V12        */
    out->v23      = regs_to_f32_cdab(&ga[10]);  /* reg 10 Voltage V23        */
    out->v31      = regs_to_f32_cdab(&ga[12]);  /* reg 12 Voltage V31        */
    out->v_avg_ll = regs_to_f32_cdab(&ga[14]);  /* reg 14 Average Voltage LL */
    out->i1       = regs_to_f32_cdab(&ga[16]);  /* reg 16 Current I1         */
    /* ga[18-19] = I2, not read by Arduino reference [C3] — skip             */
    out->i3       = regs_to_f32_cdab(&ga[20]);  /* reg 20 Current I3         */
    out->i_avg    = regs_to_f32_cdab(&ga[22]);  /* reg 22 Average Current    */
    out->kw1      = regs_to_f32_cdab(&ga[24]);  /* reg 24 kW Phase 1         */
    out->kw2      = regs_to_f32_cdab(&ga[26]);  /* reg 26 kW Phase 2         */
    out->kw3      = regs_to_f32_cdab(&ga[28]);  /* reg 28 kW Phase 3         */

    /* Group B: totals, PF, frequency, energy (regs 42-59).                  */
    if (modbus_master_read(port, unit, MFM384_FC,
                           MFM384_ADDR_GRP_B, MFM384_QTY_GRP_B,
                           500u, gb, &exc) != MODBUS_OK) return ESP_FAIL;

    /* gb[0] = reg 42, gb[2] = reg 44, … each pair = one float.             */
    out->total_kw   = regs_to_f32_cdab(&gb[ 0]);  /* reg 42 Total KW   [C1] */
    out->total_kva  = regs_to_f32_cdab(&gb[ 2]);  /* reg 44 Total KVA  [C2] */
    out->total_kvar = regs_to_f32_cdab(&gb[ 4]);  /* reg 46 Total KVAr      */
    out->pf1        = regs_to_f32_cdab(&gb[ 6]);  /* reg 48 PF1             */
    out->pf2        = regs_to_f32_cdab(&gb[ 8]);  /* reg 50 PF2             */
    out->pf3        = regs_to_f32_cdab(&gb[10]);  /* reg 52 PF3             */
    out->avg_pf     = regs_to_f32_cdab(&gb[12]);  /* reg 54 Average PF      */
    out->freq_hz    = regs_to_f32_cdab(&gb[14]);  /* reg 56 Frequency       */
    out->total_kwh  = regs_to_f32_cdab(&gb[16]);  /* reg 58 Total net kWh   */

    return ESP_OK;
}

/* ============================================================ Simulator     */
/* Pure, no I/O, deterministic from `tick` (30 s interval).
 * Nominal: 3×230 V LN, 5 A, ~1 kW per phase, PF ≈ 0.87, 50 Hz.           */
void meter_sim_mfm384(uint32_t tick, mfm384_sample_t *out)
{
    static double s_kwh = 0.0;

    float ph  = (float)tick * (2.0f * (float)M_PI * 30.0f / 3600.0f);
    float ph2 = ph + (float)(2.0 * M_PI / 3.0);
    float ph3 = ph + (float)(4.0 * M_PI / 3.0);

    /* Voltages — 225..235 V LN, derived LL. */
    out->v1n      = 230.0f + 5.0f * sinf(ph  * 0.09f);
    out->v2n      = 230.0f + 5.0f * sinf(ph2 * 0.09f);
    out->v3n      = 230.0f + 5.0f * sinf(ph3 * 0.09f);
    out->v_avg_ln = (out->v1n + out->v2n + out->v3n) / 3.0f;
    out->v12      = (out->v1n + out->v2n) * 0.866f;
    out->v23      = (out->v2n + out->v3n) * 0.866f;
    out->v31      = (out->v3n + out->v1n) * 0.866f;
    out->v_avg_ll = (out->v12 + out->v23 + out->v31) / 3.0f;

    /* Currents — 4..6 A. */
    out->i1    = 5.0f + 1.0f * sinf(ph  * 0.13f);
    out->i3    = 5.0f + 1.0f * sinf(ph3 * 0.13f);
    out->i_avg = (out->i1 + out->i3) / 2.0f;  /* approx — I2 not sampled */

    /* Per-phase power — 0.8..1.2 kW. */
    out->kw1 = 1.0f + 0.2f * sinf(ph  * 0.07f);
    out->kw2 = 1.0f + 0.2f * sinf(ph2 * 0.07f);
    out->kw3 = 1.0f + 0.2f * sinf(ph3 * 0.07f);

    /* Totals. */
    out->total_kw   = out->kw1 + out->kw2 + out->kw3;
    out->total_kvar = out->total_kw * 0.5f  * (1.0f + 0.1f * sinf(ph * 0.05f));
    out->total_kva  = sqrtf(out->total_kw * out->total_kw +
                            out->total_kvar * out->total_kvar);

    /* Power factors. */
    out->pf1    = out->kw1 / sqrtf(out->kw1*out->kw1 +
                                    (out->total_kvar/3.0f)*(out->total_kvar/3.0f));
    out->pf2    = out->kw2 / sqrtf(out->kw2*out->kw2 +
                                    (out->total_kvar/3.0f)*(out->total_kvar/3.0f));
    out->pf3    = out->kw3 / sqrtf(out->kw3*out->kw3 +
                                    (out->total_kvar/3.0f)*(out->total_kvar/3.0f));
    out->avg_pf = out->total_kw / out->total_kva;

    out->freq_hz = 50.0f + 0.1f * sinf(ph * 0.11f);  /* 49.9..50.1 Hz */

    /* Energy accumulator: P_total [kW] × Δt [h] = ΔkWh. */
    s_kwh += (double)out->total_kw * SIM_TICK_HOURS;
    out->total_kwh = (float)s_kwh;
}
