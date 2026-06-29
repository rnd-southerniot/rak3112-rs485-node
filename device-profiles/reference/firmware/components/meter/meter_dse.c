/* SPDX-License-Identifier: MIT */
/* Deep Sea Electronics generator controller — Modbus reader and simulator.
 *
 * Wire authority: Deep-Sea-code.ino
 *   Baud 115200, 8N2, FC03 (holding registers), slave IDs 10/11/12.
 * Cross-check: deep-sea-register-addredss.pdf (Basic / Derived / Accumulated
 *   Instrumentation tables).
 *
 * CONFLICT LOG (Arduino wins on addresses / FC / data type / word order /
 *               scale; datasheet wins on measurand names):
 *
 *   [C1] LABEL ERRORS in Arduino code — register addresses ARE correct and
 *        proven; the descriptive names are wrong for all voltage channels
 *        in the 1032–1070 range.  The Arduino author confused Generator ↔
 *        Mains and mis-numbered phases.  Correct names per datasheet:
 *          Arduino address 1032→Gen L1-N   (Arduino called "Mains L3-L1")
 *          Arduino address 1034→Gen L2-N   (Arduino called "Gen L3-L1")
 *          Arduino address 1036→Gen L3-N   (Arduino called "Mains L1-L2")
 *          Arduino address 1038→Gen L1-L2  ✓ (name matches)
 *          Arduino address 1040→Gen L2-L3  (Arduino called "Mains L2-L3")
 *          Arduino address 1042→Gen L3-L1  (Arduino called "Gen L2-L3")
 *          Arduino address 1060→Mains L1-N (Arduino called "Gen L1-N")
 *          Arduino address 1062→Mains L2-N (Arduino called "Gen L2-N")
 *          Arduino address 1064→Mains L3-N (Arduino called "Mains L1-N")
 *          Arduino address 1066→Mains L1-L2(Arduino called "Mains L2-N")
 *          Arduino address 1068→Mains L2-L3 ✓
 *          Arduino address 1070→Mains L3-L1(Arduino called "Gen L3-N")
 *
 *   [C2] SIGN — Gen Total Watts (R1536, 32S in datasheet) can be negative.
 *        Arduino reads as uint32 (no sign cast) → wrong for negative power.
 *        Firmware casts to int32_t before conversion.
 *
 *   [C3] SIGN — Gen Avg PF (R1557, 16S) and Gen % Full Power (R1630, 16S)
 *        are signed per datasheet; Arduino reads as uint16 and multiplies,
 *        giving wrong results when values are negative.
 *        Firmware casts to int16_t.                                         */

#include <math.h>
#include <stdint.h>
#include "meter.h"
#include "modbus_master.h"

/* FC03 (Read Holding Registers).  Arduino: node.readHoldingRegisters().     */
#define DSE_FC   0x03u

/* All register addresses below are DS absolute addresses = Modbus 0-based   *
 * frame addresses (DS does not apply the "R-1" convention that Honeywell     *
 * uses; Arduino sends these values verbatim and it works).                   *
 *                                                                             *
 * Group A: basic single/double params (1027–1031, qty=5).                   *
 *   [0] 1027 Fuel Level        u16  ×1   %                                  *
 *   [1] 1028 (unused padding)                                                *
 *   [2] 1029 Battery voltage   u16  ×0.1 V                                  *
 *   [3] 1030 Engine speed      u16  ×1   RPM                                *
 *   [4] 1031 Gen frequency     u16  ×0.1 Hz                                 */
#define DSE_ADDR_GRP_A   1027u
#define DSE_QTY_GRP_A       5u

/* Group B: generator voltages (1032–1043, qty=12, 6×u32 ABCD ×0.1 V).      *
 *   [0-1]  1032 Gen L1-N  [2-3]  1034 Gen L2-N  [4-5]  1036 Gen L3-N      *
 *   [6-7]  1038 Gen L1-L2 [8-9]  1040 Gen L2-L3 [10-11] 1042 Gen L3-L1    */
#define DSE_ADDR_GRP_B   1032u
#define DSE_QTY_GRP_B      12u

/* Group C: mains freq + voltages (1059–1071, qty=13).                       *
 *   [0]    1059 Mains freq   u16  ×0.1 Hz                                   *
 *   [1-2]  1060 Mains L1-N  [3-4]  1062 Mains L2-N  [5-6]  1064 Mains L3-N*
 *   [7-8]  1066 Mains L1-L2 [9-10] 1068 Mains L2-L3 [11-12] 1070 L3-L1    */
#define DSE_ADDR_GRP_C   1059u
#define DSE_QTY_GRP_C      13u

/* Single-shot registers (separate transactions; gaps prevent batching).      */
#define DSE_ADDR_GEN_W    1536u   /* Generator total watts, i32, ×1 W  [C2] */
#define DSE_ADDR_GEN_PF   1557u   /* Generator avg PF, i16s, ×0.1      [C3] */
#define DSE_ADDR_GEN_PCT  1630u   /* Generator % full power, i16s, ×0.1[C3] */
#define DSE_ADDR_RUN_TIME 1798u   /* Engine run time, u32, ×1 s              */

/* Raw→engineering scalars. */
#define DSE_SCALE_FUEL   1.0f
#define DSE_SCALE_BATT   0.1f
#define DSE_SCALE_RPM    1.0f
#define DSE_SCALE_FREQ   0.1f
#define DSE_SCALE_VOLT   0.1f
#define DSE_SCALE_WATT   1.0f
#define DSE_SCALE_PF     0.1f
#define DSE_SCALE_PCT    0.1f
#define DSE_SCALE_SECS   1.0f

/* Sim constants. */
#define SIM_TICK_HOURS  (30.0 / 3600.0)   /* 30-second sample interval */


/* ABCD big-endian u32: Arduino confirms (high<<16)|low in all qty=2 reads.  */
static uint32_t regs_to_u32(const uint16_t *r)
{
    return ((uint32_t)r[0] << 16) | r[1];
}

/* ============================================================= Real reader */
esp_err_t meter_read_dse(uart_port_t port, uint8_t unit, dse_sample_t *out)
{
    uint8_t  exc;
    uint16_t ga[DSE_QTY_GRP_A];
    uint16_t gb[DSE_QTY_GRP_B];
    uint16_t gc[DSE_QTY_GRP_C];
    uint16_t rw[2], rpf[1], rpct[1], rrt[2];

    /* Group A: fuel level, battery voltage, engine speed, gen frequency.    */
    if (modbus_master_read(port, unit, DSE_FC,
                           DSE_ADDR_GRP_A, DSE_QTY_GRP_A,
                           500u, ga, &exc) != MODBUS_OK) return ESP_FAIL;

    out->fuel_pct    = (float)ga[0] * DSE_SCALE_FUEL;  /* R1027 */
    /* ga[1] = R1028, unused by Arduino — skip                               */
    out->batt_v      = (float)ga[2] * DSE_SCALE_BATT;  /* R1029 */
    out->engine_rpm  = (float)ga[3] * DSE_SCALE_RPM;   /* R1030 */
    out->gen_freq_hz = (float)ga[4] * DSE_SCALE_FREQ;  /* R1031 */

    /* Group B: six generator voltage pairs (ABCD u32 each). [C1]           */
    if (modbus_master_read(port, unit, DSE_FC,
                           DSE_ADDR_GRP_B, DSE_QTY_GRP_B,
                           500u, gb, &exc) != MODBUS_OK) return ESP_FAIL;

    out->gen_l1n_v   = (float)regs_to_u32(&gb[ 0]) * DSE_SCALE_VOLT;  /* R1032 */
    out->gen_l2n_v   = (float)regs_to_u32(&gb[ 2]) * DSE_SCALE_VOLT;  /* R1034 */
    out->gen_l3n_v   = (float)regs_to_u32(&gb[ 4]) * DSE_SCALE_VOLT;  /* R1036 */
    out->gen_l1l2_v  = (float)regs_to_u32(&gb[ 6]) * DSE_SCALE_VOLT;  /* R1038 */
    out->gen_l2l3_v  = (float)regs_to_u32(&gb[ 8]) * DSE_SCALE_VOLT;  /* R1040 */
    out->gen_l3l1_v  = (float)regs_to_u32(&gb[10]) * DSE_SCALE_VOLT;  /* R1042 */

    /* Group C: mains frequency then six mains voltage pairs. [C1]          */
    if (modbus_master_read(port, unit, DSE_FC,
                           DSE_ADDR_GRP_C, DSE_QTY_GRP_C,
                           500u, gc, &exc) != MODBUS_OK) return ESP_FAIL;

    out->mains_freq_hz = (float)gc[0]                  * DSE_SCALE_FREQ; /* R1059 */
    out->mains_l1n_v   = (float)regs_to_u32(&gc[ 1])  * DSE_SCALE_VOLT; /* R1060 */
    out->mains_l2n_v   = (float)regs_to_u32(&gc[ 3])  * DSE_SCALE_VOLT; /* R1062 */
    out->mains_l3n_v   = (float)regs_to_u32(&gc[ 5])  * DSE_SCALE_VOLT; /* R1064 */
    out->mains_l1l2_v  = (float)regs_to_u32(&gc[ 7])  * DSE_SCALE_VOLT; /* R1066 */
    out->mains_l2l3_v  = (float)regs_to_u32(&gc[ 9])  * DSE_SCALE_VOLT; /* R1068 */
    out->mains_l3l1_v  = (float)regs_to_u32(&gc[11])  * DSE_SCALE_VOLT; /* R1070 */

    /* Generator total watts — 32S signed; Arduino read as uint32 [C2].     */
    if (modbus_master_read(port, unit, DSE_FC,
                           DSE_ADDR_GEN_W, 2u,
                           500u, rw, &exc) != MODBUS_OK) return ESP_FAIL;
    out->gen_total_w = (float)(int32_t)regs_to_u32(rw) * DSE_SCALE_WATT;

    /* Generator average PF — 16S signed; Arduino read as uint16 [C3].      */
    if (modbus_master_read(port, unit, DSE_FC,
                           DSE_ADDR_GEN_PF, 1u,
                           500u, rpf, &exc) != MODBUS_OK) return ESP_FAIL;
    out->gen_avg_pf = (float)(int16_t)rpf[0] * DSE_SCALE_PF;

    /* Generator % of full power — 16S signed [C3].                         */
    if (modbus_master_read(port, unit, DSE_FC,
                           DSE_ADDR_GEN_PCT, 1u,
                           500u, rpct, &exc) != MODBUS_OK) return ESP_FAIL;
    out->gen_pct_power = (float)(int16_t)rpct[0] * DSE_SCALE_PCT;

    /* Engine run time — u32, seconds.                                       */
    if (modbus_master_read(port, unit, DSE_FC,
                           DSE_ADDR_RUN_TIME, 2u,
                           500u, rrt, &exc) != MODBUS_OK) return ESP_FAIL;
    out->engine_run_s = (float)regs_to_u32(rrt) * DSE_SCALE_SECS;

    return ESP_OK;
}

/* ============================================================ Simulator     */
/* Pure, no I/O, deterministic from `tick` (30 s interval).
 * Nominal: 4-pole diesel genset at 1500 RPM/50 Hz, 3×230V LN, 150 kW load,
 * 85% PF, 75% fuel, 13.5V battery.  Mains present and stable.              */
void meter_sim_dse(uint32_t tick, dse_sample_t *out)
{
    static double s_run_s = 0.0;

    /* Slow sinusoids for gentle variation — prime-ish frequency ratios so
     * channels don't correlate visually, same technique as other sim funcs. */
    float ph  = (float)tick * (2.0f * (float)M_PI * 30.0f / 3600.0f);
    float ph2 = ph + (float)(2.0 * M_PI / 3.0);
    float ph3 = ph + (float)(4.0 * M_PI / 3.0);

    /* Engine vitals. */
    out->fuel_pct    = 75.0f  - (float)tick * 0.001f;  /* slow drain */
    if (out->fuel_pct < 5.0f) out->fuel_pct = 5.0f;
    out->batt_v      = 13.5f  + 0.3f * sinf(ph * 0.05f);  /* 13.2..13.8 V */
    out->engine_rpm  = 1500.0f + 5.0f * sinf(ph * 0.11f); /* 1495..1505 RPM */

    /* Generator output — 50 Hz, 3×230V LN / 400V LL. */
    out->gen_freq_hz = 50.0f  + 0.2f  * sinf(ph * 0.07f); /* 49.8..50.2 Hz */
    out->gen_l1n_v   = 230.0f + 4.0f  * sinf(ph  * 0.09f);
    out->gen_l2n_v   = 230.0f + 4.0f  * sinf(ph2 * 0.09f);
    out->gen_l3n_v   = 230.0f + 4.0f  * sinf(ph3 * 0.09f);
    /* L-L ≈ L-N × √3 with slight asymmetry. */
    out->gen_l1l2_v  = (out->gen_l1n_v + out->gen_l2n_v) * 0.866f;
    out->gen_l2l3_v  = (out->gen_l2n_v + out->gen_l3n_v) * 0.866f;
    out->gen_l3l1_v  = (out->gen_l3n_v + out->gen_l1n_v) * 0.866f;

    /* Mains — present and stable (slightly different from generator). */
    out->mains_freq_hz = 50.0f + 0.1f  * sinf(ph * 0.03f);
    out->mains_l1n_v   = 231.0f + 3.0f * sinf(ph  * 0.13f);
    out->mains_l2n_v   = 231.0f + 3.0f * sinf(ph2 * 0.13f);
    out->mains_l3n_v   = 231.0f + 3.0f * sinf(ph3 * 0.13f);
    out->mains_l1l2_v  = (out->mains_l1n_v + out->mains_l2n_v) * 0.866f;
    out->mains_l2l3_v  = (out->mains_l2n_v + out->mains_l3n_v) * 0.866f;
    out->mains_l3l1_v  = (out->mains_l3n_v + out->mains_l1n_v) * 0.866f;

    /* Derived generator power quantities. */
    out->gen_total_w   = 150000.0f + 5000.0f * sinf(ph * 0.07f); /* ~150 kW */
    out->gen_avg_pf    = 0.85f     + 0.03f   * sinf(ph * 0.09f); /* 0.82..0.88 */
    out->gen_pct_power = 80.0f     + 3.0f    * sinf(ph * 0.11f); /* ~80% */

    /* Run-time accumulates every tick (30 s per tick). */
    s_run_s += 30.0;
    out->engine_run_s = (float)s_run_s;
}
