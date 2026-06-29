/* SPDX-License-Identifier: MIT */
/* ADR-005 payload encoder for Deep Sea Electronics generator controller.
 * Pure function — no I/O, host-testable without hardware.
 *
 * Body layout (44 bytes; total with 3-byte header = 47 ≤ 53 AS923-DR3):
 *
 *  Off  Sz  Field           Type   Scale       Unit
 *    0   1  fuel_pct         u8    1 %/LSB      % (0-130)
 *    1   2  batt_v           u16   0.1 V/LSB    V
 *    3   2  engine_rpm       u16   1 RPM/LSB    RPM (0-6000)
 *    5   2  gen_freq_hz      u16   0.1 Hz/LSB   Hz
 *    7   2  gen_l1n_v        u16   0.1 V/LSB    V
 *    9   2  gen_l2n_v        u16   0.1 V/LSB    V
 *   11   2  gen_l3n_v        u16   0.1 V/LSB    V
 *   13   2  gen_l1l2_v       u16   0.1 V/LSB    V
 *   15   2  gen_l2l3_v       u16   0.1 V/LSB    V
 *   17   2  gen_l3l1_v       u16   0.1 V/LSB    V
 *   19   2  mains_freq_hz    u16   0.1 Hz/LSB   Hz
 *   21   2  mains_l1n_v      u16   0.1 V/LSB    V
 *   23   2  mains_l2n_v      u16   0.1 V/LSB    V
 *   25   2  mains_l3n_v      u16   0.1 V/LSB    V
 *   27   2  mains_l1l2_v     u16   0.1 V/LSB    V
 *   29   2  mains_l2l3_v     u16   0.1 V/LSB    V
 *   31   2  mains_l3l1_v     u16   0.1 V/LSB    V
 *   33   4  gen_total_w      i32   1 W/LSB       W (signed: ±99 999 999)
 *   37   1  gen_avg_pf       i8    0.01/LSB      – (–1.00 … +1.00 → –100…+100)
 *   38   2  gen_pct_power    i16   0.1 %/LSB     %
 *   40   4  engine_run_s     u32   1 s/LSB       seconds
 *
 * Scale-factor choice rationale:
 *   Voltage  ×10 : 0.1 V preserves meter resolution (raw × 0.1 = V).
 *   Frequency×10 : 0.1 Hz; u16 max 6553.5 Hz well covers 70 Hz DS max.
 *   Power    ×1  : 1 W; i32 covers DS range ±99 999 999 W exactly.
 *   PF       ×100: 0.01; DS raw ×0.1 gives –10…+10; ×100 = –100…+100 → i8.
 *   % power  ×10 : 0.1 %; DS raw ×0.1 gives ±999.9; ×10 → ±9999 fits i16.
 *   Run time ×1  : 1 s; u32 holds 136 years.                               */

#include <stdint.h>
#include "meter.h"
#include "telemetry.h"

#define DSE_BODY_LEN    44u
#define DSE_TOTAL_LEN   (TELEMETRY_HEADER_LEN + DSE_BODY_LEN)  /* 47 bytes */

/* Named scale constants — mirror exactly in chirpstack_dse_decoder.js.      */
#define DSE_PL_VOLT_SCALE     10u   /* LSB = 0.1 V    */
#define DSE_PL_FREQ_SCALE     10u   /* LSB = 0.1 Hz   */
#define DSE_PL_WATT_SCALE      1    /* LSB = 1 W      */
#define DSE_PL_PF_SCALE      100    /* LSB = 0.01     */
#define DSE_PL_PCT_SCALE      10    /* LSB = 0.1 %    */

static void put_u32_be(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >>  8); b[3] = (uint8_t)(v);
}
static void put_i32_be(uint8_t *b, int32_t v)  { put_u32_be(b, (uint32_t)v); }
static void put_u16_be(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static void put_i16_be(uint8_t *b, int16_t v)  { put_u16_be(b, (uint16_t)v); }

/* Signed round-half-away-from-zero: works for negative values unlike +0.5f. */
static int32_t fround_i32(float v) { return (int32_t)(v >= 0.0f ? v + 0.5f : v - 0.5f); }
static int16_t fround_i16(float v) { return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f); }
static int8_t  fround_i8(float v)  { return  (int8_t)(v >= 0.0f ? v + 0.5f : v - 0.5f); }

/* Returns total bytes written (DSE_TOTAL_LEN = 47) on success, -1 if too
 * small.  Caller sets TELEMETRY_FLAG_SIMULATED or _STALE in `flags`.        */
int payload_encode_dse(const dse_sample_t *s, uint8_t flags,
                       uint8_t *buf, size_t n)
{
    if (n < DSE_TOTAL_LEN) return -1;

    buf[0] = TELEMETRY_SCHEMA_VERSION;
    buf[1] = TELEMETRY_DEV_DEEPSEA;
    buf[2] = flags;

    uint8_t *b = buf + TELEMETRY_HEADER_LEN;

    b[0] = (uint8_t)(s->fuel_pct + 0.5f);                                    /* off 0 */
    put_u16_be(&b[ 1], (uint16_t)(s->batt_v      * (float)DSE_PL_VOLT_SCALE + 0.5f)); /* off 1 */
    put_u16_be(&b[ 3], (uint16_t)(s->engine_rpm  + 0.5f));                   /* off 3 */
    put_u16_be(&b[ 5], (uint16_t)(s->gen_freq_hz * (float)DSE_PL_FREQ_SCALE + 0.5f)); /* off 5 */

    put_u16_be(&b[ 7], (uint16_t)(s->gen_l1n_v   * (float)DSE_PL_VOLT_SCALE + 0.5f)); /* off 7 */
    put_u16_be(&b[ 9], (uint16_t)(s->gen_l2n_v   * (float)DSE_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[11], (uint16_t)(s->gen_l3n_v   * (float)DSE_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[13], (uint16_t)(s->gen_l1l2_v  * (float)DSE_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[15], (uint16_t)(s->gen_l2l3_v  * (float)DSE_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[17], (uint16_t)(s->gen_l3l1_v  * (float)DSE_PL_VOLT_SCALE + 0.5f));

    put_u16_be(&b[19], (uint16_t)(s->mains_freq_hz * (float)DSE_PL_FREQ_SCALE + 0.5f)); /* off 19 */
    put_u16_be(&b[21], (uint16_t)(s->mains_l1n_v   * (float)DSE_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[23], (uint16_t)(s->mains_l2n_v   * (float)DSE_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[25], (uint16_t)(s->mains_l3n_v   * (float)DSE_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[27], (uint16_t)(s->mains_l1l2_v  * (float)DSE_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[29], (uint16_t)(s->mains_l2l3_v  * (float)DSE_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[31], (uint16_t)(s->mains_l3l1_v  * (float)DSE_PL_VOLT_SCALE + 0.5f));

    put_i32_be(&b[33], fround_i32(s->gen_total_w   * (float)DSE_PL_WATT_SCALE)); /* off 33 */
    b[37] = (uint8_t)fround_i8(s->gen_avg_pf  * (float)DSE_PL_PF_SCALE);       /* off 37 */
    put_i16_be(&b[38], fround_i16(s->gen_pct_power * (float)DSE_PL_PCT_SCALE)); /* off 38 */
    put_u32_be(&b[40], (uint32_t)(s->engine_run_s + 0.5f));                  /* off 40 */

    return (int)DSE_TOTAL_LEN;
}
