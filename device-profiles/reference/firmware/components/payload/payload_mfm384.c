/* SPDX-License-Identifier: MIT */
/* ADR-005 payload encoder for Selec MFM384-C.
 * Pure function — no I/O, host-testable without hardware.
 *
 * Body layout (36 bytes; total with 3-byte header = 39 ≤ 53 AS923-DR3):
 *
 *  Off  Sz  Field        Type   Scale      Unit
 *    0   2  v1n           u16   0.1 V/LSB   V
 *    2   2  v2n           u16   0.1 V/LSB   V
 *    4   2  v3n           u16   0.1 V/LSB   V
 *    6   2  v12           u16   0.1 V/LSB   V
 *    8   2  v23           u16   0.1 V/LSB   V
 *   10   2  v31           u16   0.1 V/LSB   V
 *   12   2  i1            u16   0.01 A/LSB  A
 *   14   2  i3            u16   0.01 A/LSB  A
 *   16   2  kw1           i16   0.1 kW/LSB  kW  (signed: import +, export –)
 *   18   2  kw2           i16   0.1 kW/LSB  kW
 *   20   2  kw3           i16   0.1 kW/LSB  kW
 *   22   2  total_kw      i16   0.1 kW/LSB  kW
 *   24   2  total_kvar    i16   0.1 kvar/LSB kvar
 *   26   1  pf1           i8    0.01/LSB    –  (signed per IEC, see DS)
 *   27   1  pf2           i8    0.01/LSB    –
 *   28   1  pf3           i8    0.01/LSB    –
 *   29   1  avg_pf        i8    0.01/LSB    –
 *   30   2  freq_hz       u16   0.01 Hz/LSB Hz
 *   32   4  total_kwh     u32   0.01 kWh/LSB kWh
 *
 * Scale-factor choice rationale:
 *   Voltage  ×10  : 0.1 V preserves meter accuracy (±0.5% of FS ≈ ±1V at 200V).
 *   Current  ×100 : 0.01 A is fine for the 6 A CT secondary; u16 → max 655.35 A.
 *   Power    ×10  : 0.1 kW; i16 covers ±3276 kW which handles most LV panels.
 *   PF       ×100 : 0.01; DS accuracy ±0.01; range –1.00…+1.00 fits i8.
 *   Freq     ×100 : 0.01 Hz; DS accuracy ±0.1%; 50 Hz × 100 = 5000 fits u16.
 *   kWh      ×100 : 0.01 kWh; u32 holds up to ~42.9 GWh.                   */

#include <stdint.h>
#include "meter.h"
#include "telemetry.h"

#define MFM384_BODY_LEN    36u
#define MFM384_TOTAL_LEN   (TELEMETRY_HEADER_LEN + MFM384_BODY_LEN)  /* 39 */

/* Named scale constants — mirror exactly in chirpstack_mfm384_decoder.js.   */
#define MFM384_PL_VOLT_SCALE    10u   /* LSB = 0.1 V    */
#define MFM384_PL_CURR_SCALE   100u   /* LSB = 0.01 A   */
#define MFM384_PL_POWER_SCALE   10    /* LSB = 0.1 kW   */
#define MFM384_PL_PF_SCALE     100    /* LSB = 0.01     */
#define MFM384_PL_FREQ_SCALE   100u   /* LSB = 0.01 Hz  */
#define MFM384_PL_KWH_SCALE    100u   /* LSB = 0.01 kWh */

static void put_u32_be(uint8_t *b, uint32_t v)
{
    b[0]=(uint8_t)(v>>24); b[1]=(uint8_t)(v>>16);
    b[2]=(uint8_t)(v>>8);  b[3]=(uint8_t)v;
}
static void put_u16_be(uint8_t *b, uint16_t v) { b[0]=(uint8_t)(v>>8); b[1]=(uint8_t)v; }
static void put_i16_be(uint8_t *b, int16_t v)  { put_u16_be(b, (uint16_t)v); }

static int16_t fround_i16(float v) { return (int16_t)(v >= 0.0f ? v+0.5f : v-0.5f); }
static int8_t  fround_i8(float v)  { return  (int8_t)(v >= 0.0f ? v+0.5f : v-0.5f); }

/* Returns total bytes written (MFM384_TOTAL_LEN = 39) on success, -1 if buf
 * is too small.  Caller sets TELEMETRY_FLAG_SIMULATED or _STALE in `flags`. */
int payload_encode_mfm384(const mfm384_sample_t *s, uint8_t flags,
                          uint8_t *buf, size_t n)
{
    if (n < MFM384_TOTAL_LEN) return -1;

    buf[0] = TELEMETRY_SCHEMA_VERSION;
    buf[1] = TELEMETRY_DEV_MFM384;
    buf[2] = flags;

    uint8_t *b = buf + TELEMETRY_HEADER_LEN;

    put_u16_be(&b[ 0], (uint16_t)(s->v1n * (float)MFM384_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[ 2], (uint16_t)(s->v2n * (float)MFM384_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[ 4], (uint16_t)(s->v3n * (float)MFM384_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[ 6], (uint16_t)(s->v12 * (float)MFM384_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[ 8], (uint16_t)(s->v23 * (float)MFM384_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[10], (uint16_t)(s->v31 * (float)MFM384_PL_VOLT_SCALE + 0.5f));

    put_u16_be(&b[12], (uint16_t)(s->i1 * (float)MFM384_PL_CURR_SCALE + 0.5f));
    put_u16_be(&b[14], (uint16_t)(s->i3 * (float)MFM384_PL_CURR_SCALE + 0.5f));

    put_i16_be(&b[16], fround_i16(s->kw1        * (float)MFM384_PL_POWER_SCALE));
    put_i16_be(&b[18], fround_i16(s->kw2        * (float)MFM384_PL_POWER_SCALE));
    put_i16_be(&b[20], fround_i16(s->kw3        * (float)MFM384_PL_POWER_SCALE));
    put_i16_be(&b[22], fround_i16(s->total_kw   * (float)MFM384_PL_POWER_SCALE));
    put_i16_be(&b[24], fround_i16(s->total_kvar * (float)MFM384_PL_POWER_SCALE));

    b[26] = (uint8_t)fround_i8(s->pf1    * (float)MFM384_PL_PF_SCALE);
    b[27] = (uint8_t)fround_i8(s->pf2    * (float)MFM384_PL_PF_SCALE);
    b[28] = (uint8_t)fround_i8(s->pf3    * (float)MFM384_PL_PF_SCALE);
    b[29] = (uint8_t)fround_i8(s->avg_pf * (float)MFM384_PL_PF_SCALE);

    put_u16_be(&b[30], (uint16_t)(s->freq_hz   * (float)MFM384_PL_FREQ_SCALE + 0.5f));
    put_u32_be(&b[32], (uint32_t)(s->total_kwh * (float)MFM384_PL_KWH_SCALE  + 0.5f));

    return (int)MFM384_TOTAL_LEN;
}
