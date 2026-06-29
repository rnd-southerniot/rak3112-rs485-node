/* ChirpStack v4 codec — Deep Sea Electronics generator controller (0x04).
 * Mirrors payload_dse.c scale constants exactly.
 *
 * Scale constants (must match firmware payload_dse.c):
 *   Voltage  : ÷10    → V    (LSB = 0.1 V)
 *   Frequency: ÷10    → Hz   (LSB = 0.1 Hz)
 *   Power    : ÷1     → W    (LSB = 1 W,  signed)
 *   PF       : ÷100   → –    (LSB = 0.01, signed i8: –100…+100)
 *   % power  : ÷10    → %    (LSB = 0.1 %, signed)
 *   Run time : ÷1     → s    (LSB = 1 s,  unsigned)                        */

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

    if (device === 0x03) {
        out.device = "EEM400C-D-MO";
        /* See chirpstack_eem400_decoder.js for EEM400 branch. */
        return out;
    }

    if (device === 0x04) {
        return decodeDse(out, bytes);
    }

    out.error = "unknown device-type 0x" + device.toString(16);
    return out;
}

/* ----------------------------------------------------------------- helpers */
function u32be(b, i) { return (((b[i]<<24)|(b[i+1]<<16)|(b[i+2]<<8)|b[i+3])>>>0); }
function i32be(b, i) { var v = u32be(b,i); return v >= 0x80000000 ? v - 0x100000000 : v; }
function u16be(b, i) { return ((b[i]<<8)|b[i+1])>>>0; }
function i8(b, i)    { return b[i] >= 0x80 ? b[i] - 0x100 : b[i]; }
function i16be(b, i) { var v = u16be(b,i); return v >= 0x8000 ? v - 0x10000 : v; }

/* ------------------------------------------------------- DSE body decode   *
 * Byte offsets: header = bytes[0..2], body = bytes[3..46].                  */
function decodeDse(out, b) {
    if (b.length < 47) {
        out.error = "DSE payload too short (" + b.length + " bytes, need 47)";
        return out;
    }

    out.device = "DSE-genset";

    /* off 0 (body base = 3) */
    out.fuel_pct        = b[3];                  /* u8, 1 %/LSB              */
    out.batt_v          = u16be(b,  4) / 10.0;  /* u16, 0.1 V               */
    out.engine_rpm      = u16be(b,  6);          /* u16, 1 RPM               */
    out.gen_freq_hz     = u16be(b,  8) / 10.0;  /* u16, 0.1 Hz              */

    out.gen_l1n_v       = u16be(b, 10) / 10.0;
    out.gen_l2n_v       = u16be(b, 12) / 10.0;
    out.gen_l3n_v       = u16be(b, 14) / 10.0;
    out.gen_l1l2_v      = u16be(b, 16) / 10.0;
    out.gen_l2l3_v      = u16be(b, 18) / 10.0;
    out.gen_l3l1_v      = u16be(b, 20) / 10.0;

    out.mains_freq_hz   = u16be(b, 22) / 10.0;
    out.mains_l1n_v     = u16be(b, 24) / 10.0;
    out.mains_l2n_v     = u16be(b, 26) / 10.0;
    out.mains_l3n_v     = u16be(b, 28) / 10.0;
    out.mains_l1l2_v    = u16be(b, 30) / 10.0;
    out.mains_l2l3_v    = u16be(b, 32) / 10.0;
    out.mains_l3l1_v    = u16be(b, 34) / 10.0;

    out.gen_total_w     = i32be(b, 36);          /* i32, 1 W (signed)        */
    out.gen_avg_pf      = i8(b, 40)  / 100.0;   /* i8,  0.01 (signed)       */
    out.gen_pct_power   = i16be(b, 41) / 10.0;  /* i16, 0.1 % (signed)      */
    out.engine_run_s    = u32be(b, 43);          /* u32, 1 s                 */

    return out;
}
