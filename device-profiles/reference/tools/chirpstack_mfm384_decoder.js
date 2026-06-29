/* ChirpStack v4 codec — Selec MFM384-C 3-phase energy meter (device type 0x02).
 * Mirrors payload_mfm384.c scale constants exactly.
 *
 * Scale constants (must match firmware payload_mfm384.c):
 *   Voltage  : ÷10    → V       (LSB = 0.1 V)
 *   Current  : ÷100   → A       (LSB = 0.01 A)
 *   Power    : ÷10    → kW/kvar (LSB = 0.1 kW, signed)
 *   PF       : ÷100   → –       (LSB = 0.01, signed i8: –100…+100)
 *   Frequency: ÷100   → Hz      (LSB = 0.01 Hz)
 *   Energy   : ÷100   → kWh     (LSB = 0.01 kWh)
 *
 * Body layout (bytes[3..38], 36 bytes):
 *  Abs   Body  Sz  Field       Type  Divisor  Unit
 *   [3]  [ 0]   2  v1n          u16    10     V
 *   [5]  [ 2]   2  v2n          u16    10     V
 *   [7]  [ 4]   2  v3n          u16    10     V
 *   [9]  [ 6]   2  v12          u16    10     V
 *  [11]  [ 8]   2  v23          u16    10     V
 *  [13]  [10]   2  v31          u16    10     V
 *  [15]  [12]   2  i1           u16   100     A
 *  [17]  [14]   2  i3           u16   100     A  (I2 not read, see meter_mfm384.c)
 *  [19]  [16]   2  kw1          i16    10     kW
 *  [21]  [18]   2  kw2          i16    10     kW
 *  [23]  [20]   2  kw3          i16    10     kW
 *  [25]  [22]   2  total_kw     i16    10     kW  (DS 30042 [C1])
 *  [27]  [24]   2  total_kvar   i16    10     kvar
 *  [29]  [26]   1  pf1           i8   100     –
 *  [30]  [27]   1  pf2           i8   100     –
 *  [31]  [28]   1  pf3           i8   100     –
 *  [32]  [29]   1  avg_pf        i8   100     –
 *  [33]  [30]   2  freq_hz      u16   100     Hz
 *  [35]  [32]   4  total_kwh    u32   100     kWh
 *
 * Total: 3-byte header + 36-byte body = 39 bytes.                           */

/* eslint-disable no-var */

function decodeUplink(input) {
    var bytes = input.bytes;
    return { data: Decode(input.fPort, bytes) };
}

function Decode(fPort, bytes) {
    if (bytes.length < 3) return { error: "payload too short" };

    var schema = bytes[0];
    var device = bytes[1];
    var flags  = bytes[2];

    var out = {
        schema:    schema,
        simulated: !!(flags & 0x01),
        stale:     !!(flags & 0x02),
    };

    if (device === 0x02) {
        return decodeMfm384(out, bytes);
    }

    out.error = "unknown device-type 0x" + device.toString(16);
    return out;
}

/* ----------------------------------------------------------------- helpers */
function u32be(b, i) { return (((b[i]<<24)|(b[i+1]<<16)|(b[i+2]<<8)|b[i+3])>>>0); }
function u16be(b, i) { return ((b[i]<<8)|b[i+1])>>>0; }
function i16be(b, i) { var v = u16be(b,i); return v >= 0x8000 ? v - 0x10000 : v; }
function i8(b, i)    { return b[i] >= 0x80 ? b[i] - 0x100 : b[i]; }

/* ----------------------------------------------------- MFM384 body decode */
function decodeMfm384(out, b) {
    if (b.length < 39) {
        out.error = "MFM384 payload too short (" + b.length + " bytes, need 39)";
        return out;
    }

    out.device = "MFM384-C";

    /* Line-to-neutral voltages (bytes 3..8) */
    out.v1n_v   = u16be(b,  3) / 10.0;  /* u16, 0.1 V */
    out.v2n_v   = u16be(b,  5) / 10.0;
    out.v3n_v   = u16be(b,  7) / 10.0;

    /* Line-to-line voltages (bytes 9..14) */
    out.v12_v   = u16be(b,  9) / 10.0;
    out.v23_v   = u16be(b, 11) / 10.0;
    out.v31_v   = u16be(b, 13) / 10.0;

    /* Currents (bytes 15..18) */
    out.i1_a    = u16be(b, 15) / 100.0;  /* u16, 0.01 A */
    out.i3_a    = u16be(b, 17) / 100.0;

    /* Per-phase active power (bytes 19..24) */
    out.kw1     = i16be(b, 19) / 10.0;  /* i16, 0.1 kW */
    out.kw2     = i16be(b, 21) / 10.0;
    out.kw3     = i16be(b, 23) / 10.0;

    /* Totals (bytes 25..28) */
    out.total_kw   = i16be(b, 25) / 10.0;  /* DS 30042 [C1]: Total KW  */
    out.total_kvar = i16be(b, 27) / 10.0;  /* DS 30046: Total KVAr     */

    /* Power factors (bytes 29..32) */
    out.pf1     = i8(b, 29) / 100.0;  /* i8, 0.01 */
    out.pf2     = i8(b, 30) / 100.0;
    out.pf3     = i8(b, 31) / 100.0;
    out.avg_pf  = i8(b, 32) / 100.0;

    /* Frequency (bytes 33..34) */
    out.freq_hz = u16be(b, 33) / 100.0;  /* u16, 0.01 Hz */

    /* Energy (bytes 35..38) */
    out.total_kwh = u32be(b, 35) / 100.0;  /* u32, 0.01 kWh */

    return out;
}
