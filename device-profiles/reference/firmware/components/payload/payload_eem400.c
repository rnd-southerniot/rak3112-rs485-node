/* SPDX-License-Identifier: MIT */
/* ADR-005 payload encoder for Honeywell EEM400C-D-MO.
 * Pure function — no I/O, host-testable without hardware.
 *
 * Body layout (43 bytes; total with 3-byte header = 46 ≤ 53 AS923-DR3):
 *
 *  Off  Sz  Field          Type  Scale   Unit
 *    0   4  t1_total_kwh   u32   0.1 kWh/LSB   (T1 total energy)
 *    4   4  t1_part_kwh    u32   0.1 kWh/LSB   (T1 partial/resettable)
 *    8   4  t2_total_kwh   u32   0.1 kWh/LSB   (T2 total energy)
 *   12   2  v1             u16   0.1 V/LSB
 *   14   2  v2             u16   0.1 V/LSB
 *   16   2  v3             u16   0.1 V/LSB
 *   18   2  i1             u16   0.1 A/LSB
 *   20   2  i2             u16   0.1 A/LSB
 *   22   2  i3             u16   0.1 A/LSB
 *   24   2  p1             i16   0.1 kW/LSB
 *   26   2  p2             i16   0.1 kW/LSB
 *   28   2  p3             i16   0.1 kW/LSB
 *   30   2  q1             i16   0.1 kvar/LSB
 *   32   2  q2             i16   0.1 kvar/LSB
 *   34   2  q3             i16   0.1 kvar/LSB
 *   36   2  p_total        i16   0.1 kW/LSB
 *   38   2  q_total        i16   0.1 kvar/LSB
 *   40   1  cos1           u8    0.01/LSB      (PF phase 1)
 *   41   1  cos2           u8    0.01/LSB
 *   42   1  cos3           u8    0.01/LSB
 *
 * Scale-factor choice rationale:
 *   Energy  ×10  : 0.1 kWh preserves meter resolution; u32 holds ~429 MWh.
 *   Voltage ×10  : 0.1 V resolves fine for LV metering (230 V nominal).
 *   Current ×10  : 0.1 A matches meter register resolution (÷10 raw scale).
 *   Power   ×10  : 0.1 kW matches datasheet multiplier 10^-1; i16 ≤ ±3276 kW.
 *   PF      ×100 : 0.01 matches datasheet multiplier 10^-2; fits u8 (0..100).*/

#include <stdint.h>
#include "meter.h"
#include "telemetry.h"

#define EEM400_BODY_LEN    43u
#define EEM400_TOTAL_LEN   (TELEMETRY_HEADER_LEN + EEM400_BODY_LEN)  /* 46 */

/* Named scale constants — mirror exactly in the ChirpStack JS decoder. */
#define EEM400_PL_ENERGY_SCALE   10u   /* LSB = 0.1 kWh  */
#define EEM400_PL_VOLT_SCALE     10u   /* LSB = 0.1 V    */
#define EEM400_PL_CURR_SCALE     10u   /* LSB = 0.1 A    */
#define EEM400_PL_POWER_SCALE    10    /* LSB = 0.1 kW   */
#define EEM400_PL_PF_SCALE       100u  /* LSB = 0.01     */

static void put_u32_be(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >>  8);
    b[3] = (uint8_t)(v);
}

static void put_u16_be(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)(v);
}

static void put_i16_be(uint8_t *b, int16_t v)
{
    put_u16_be(b, (uint16_t)v);
}

/* Returns total bytes written (EEM400_TOTAL_LEN = 46) on success, -1 if buf
 * is too small.  Caller sets TELEMETRY_FLAG_SIMULATED or _STALE in `flags`. */
int payload_encode_eem400(const eem400_sample_t *s, uint8_t flags,
                          uint8_t *buf, size_t n)
{
    if (n < EEM400_TOTAL_LEN) return -1;

    buf[0] = TELEMETRY_SCHEMA_VERSION;
    buf[1] = TELEMETRY_DEV_EEM400;
    buf[2] = flags;

    uint8_t *b = buf + TELEMETRY_HEADER_LEN;

    put_u32_be(&b[ 0], (uint32_t)(s->t1_total_kwh * (float)EEM400_PL_ENERGY_SCALE + 0.5f));
    put_u32_be(&b[ 4], (uint32_t)(s->t1_part_kwh  * (float)EEM400_PL_ENERGY_SCALE + 0.5f));
    put_u32_be(&b[ 8], (uint32_t)(s->t2_total_kwh * (float)EEM400_PL_ENERGY_SCALE + 0.5f));

    put_u16_be(&b[12], (uint16_t)(s->v1 * (float)EEM400_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[14], (uint16_t)(s->v2 * (float)EEM400_PL_VOLT_SCALE + 0.5f));
    put_u16_be(&b[16], (uint16_t)(s->v3 * (float)EEM400_PL_VOLT_SCALE + 0.5f));

    put_u16_be(&b[18], (uint16_t)(s->i1 * (float)EEM400_PL_CURR_SCALE + 0.5f));
    put_u16_be(&b[20], (uint16_t)(s->i2 * (float)EEM400_PL_CURR_SCALE + 0.5f));
    put_u16_be(&b[22], (uint16_t)(s->i3 * (float)EEM400_PL_CURR_SCALE + 0.5f));

    put_i16_be(&b[24], (int16_t)(s->p1 * (float)EEM400_PL_POWER_SCALE + 0.5f));
    put_i16_be(&b[26], (int16_t)(s->p2 * (float)EEM400_PL_POWER_SCALE + 0.5f));
    put_i16_be(&b[28], (int16_t)(s->p3 * (float)EEM400_PL_POWER_SCALE + 0.5f));

    put_i16_be(&b[30], (int16_t)(s->q1 * (float)EEM400_PL_POWER_SCALE + 0.5f));
    put_i16_be(&b[32], (int16_t)(s->q2 * (float)EEM400_PL_POWER_SCALE + 0.5f));
    put_i16_be(&b[34], (int16_t)(s->q3 * (float)EEM400_PL_POWER_SCALE + 0.5f));

    put_i16_be(&b[36], (int16_t)(s->p_total * (float)EEM400_PL_POWER_SCALE + 0.5f));
    put_i16_be(&b[38], (int16_t)(s->q_total * (float)EEM400_PL_POWER_SCALE + 0.5f));

    b[40] = (uint8_t)(s->cos1 * (float)EEM400_PL_PF_SCALE + 0.5f);
    b[41] = (uint8_t)(s->cos2 * (float)EEM400_PL_PF_SCALE + 0.5f);
    b[42] = (uint8_t)(s->cos3 * (float)EEM400_PL_PF_SCALE + 0.5f);

    return (int)EEM400_TOTAL_LEN;
}
