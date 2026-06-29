/* ChirpStack v4 codec — Honeywell EEM400C-D-MO (device-type 0x03).
 * Mirrors payload_eem400.c scale constants exactly.
 * Add this branch inside your existing Decode() function, or use as-is.
 *
 * Scale constants (must match firmware payload_eem400.c):
 *   Energy : ÷10   → kWh   (LSB = 0.1 kWh)
 *   Voltage: ÷10   → V     (LSB = 0.1 V)
 *   Current: ÷10   → A     (LSB = 0.1 A)
 *   Power  : ÷10   → kW/kvar (LSB = 0.1)
 *   PF     : ÷100  → –     (LSB = 0.01)                                   */

/* eslint-disable no-var */

function decodeUplink(input) {
    var bytes = input.bytes;
    return { data: Decode(input.fPort, bytes) };
}

function Decode(fPort, bytes) {
    if (bytes.length < 3) {
        return { error: "payload too short" };
    }

    var schema = bytes[0];
    var device = bytes[1];
    var flags  = bytes[2];

    var out = {
        schema:    schema,
        simulated: !!(flags & 0x01),
        stale:     !!(flags & 0x02),
    };

    if (device === 0x02) {
        out.device = "MFM384";
        /* Insert MFM384 decode branch here. */
        return out;
    }

    if (device === 0x03) {
        return decodeEem400(out, bytes);
    }

    out.error = "unknown device-type 0x" + device.toString(16);
    return out;
}

/* ----------------------------------------------------------------- helpers */
function u32be(b, i) {
    /* Unsigned 32-bit big-endian.  Use >>> 0 to keep JS number unsigned.    */
    return (((b[i] << 24) | (b[i+1] << 16) | (b[i+2] << 8) | b[i+3]) >>> 0);
}
function u16be(b, i) {
    return ((b[i] << 8) | b[i+1]) >>> 0;
}
function i16be(b, i) {
    var v = u16be(b, i);
    return v >= 0x8000 ? v - 0x10000 : v;
}

/* ------------------------------------------------------- EEM400 body decode */
function decodeEem400(out, b) {
    if (b.length < 46) {
        out.error = "EEM400 payload too short (" + b.length + " bytes, need 46)";
        return out;
    }

    out.device = "EEM400C-D-MO";

    /* Offsets: header=3, then body as documented in payload_eem400.c         */
    out.t1_total_kwh   = u32be(b,  3) / 10.0;
    out.t1_part_kwh    = u32be(b,  7) / 10.0;
    out.t2_total_kwh   = u32be(b, 11) / 10.0;
    out.v1_v           = u16be(b, 15) / 10.0;
    out.v2_v           = u16be(b, 17) / 10.0;
    out.v3_v           = u16be(b, 19) / 10.0;
    out.i1_a           = u16be(b, 21) / 10.0;
    out.i2_a           = u16be(b, 23) / 10.0;
    out.i3_a           = u16be(b, 25) / 10.0;
    out.p1_kw          = i16be(b, 27) / 10.0;
    out.p2_kw          = i16be(b, 29) / 10.0;
    out.p3_kw          = i16be(b, 31) / 10.0;
    out.q1_kvar        = i16be(b, 33) / 10.0;
    out.q2_kvar        = i16be(b, 35) / 10.0;
    out.q3_kvar        = i16be(b, 37) / 10.0;
    out.p_total_kw     = i16be(b, 39) / 10.0;
    out.q_total_kvar   = i16be(b, 41) / 10.0;
    out.cos1           = b[43] / 100.0;
    out.cos2           = b[44] / 100.0;
    out.cos3           = b[45] / 100.0;

    return out;
}
