/*
 * device_profile.c — pure numeric core for the data-driven runtime (ADR-006). See device_profile.h.
 * No SDK dependency: builds host-side under ctest and on-target for the generic reader.
 */
#include "device_profile.h"

#include <math.h>
#include <string.h>

#include "telemetry.h" /* TELEMETRY_SCHEMA_VERSION, TELEMETRY_HEADER_LEN */

uint8_t dp_dtype_regs(dp_dtype_t type)
{
    return (type == DP_U32 || type == DP_I32 || type == DP_F32) ? 2u : 1u;
}

/* Assemble the 32-bit wire value from two registers per word order. */
static uint32_t assemble_u32(const uint16_t *regs, dp_word_t word)
{
    const uint8_t a = (uint8_t)(regs[0] >> 8); /* wire byte A (MSB of reg0) */
    const uint8_t b = (uint8_t)(regs[0] & 0xFFu);
    const uint8_t c = (uint8_t)(regs[1] >> 8);
    const uint8_t d = (uint8_t)(regs[1] & 0xFFu);
    uint8_t m0, m1, m2, m3; /* most- to least-significant of the assembled value */
    switch (word) {
    case DP_CDAB:
        m0 = c;
        m1 = d;
        m2 = a;
        m3 = b;
        break;
    case DP_BADC:
        m0 = b;
        m1 = a;
        m2 = d;
        m3 = c;
        break;
    case DP_DCBA:
        m0 = d;
        m1 = c;
        m2 = b;
        m3 = a;
        break;
    case DP_ABCD:
    default:
        m0 = a;
        m1 = b;
        m2 = c;
        m3 = d;
        break;
    }
    return ((uint32_t)m0 << 24) | ((uint32_t)m1 << 16) | ((uint32_t)m2 << 8) | (uint32_t)m3;
}

float dp_decode(const uint16_t *regs, dp_dtype_t type, dp_word_t word, float scale, float offset)
{
    if (regs == NULL) {
        return 0.0f;
    }
    float raw;
    switch (type) {
    case DP_U16:
        raw = (float)regs[0];
        break;
    case DP_I16:
        raw = (float)(int16_t)regs[0];
        break;
    case DP_U32:
        raw = (float)assemble_u32(regs, word);
        break;
    case DP_I32:
        raw = (float)(int32_t)assemble_u32(regs, word);
        break;
    case DP_F32: {
        const uint32_t bits = assemble_u32(regs, word);
        float f;
        memcpy(&f, &bits, sizeof(f)); /* no aliasing UB */
        return f * scale + offset;
    }
    default:
        return 0.0f;
    }
    return raw * scale + offset;
}

/* Saturating engineering -> scaled integer helpers. */
static uint32_t sat_u(float v, float scale, uint32_t max)
{
    if (v <= 0.0f) {
        return 0u;
    }
    const float s = v * scale + 0.5f;
    return (s >= (float)max) ? max : (uint32_t)s;
}

static int32_t sat_i(float v, float scale, int32_t lo, int32_t hi)
{
    const float s = v * scale + (v >= 0.0f ? 0.5f : -0.5f);
    if (s >= (float)hi) {
        return hi;
    }
    if (s <= (float)lo) {
        return lo;
    }
    return (int32_t)s;
}

static uint8_t enc_size(dp_enc_t e)
{
    switch (e) {
    case DP_ENC_U8:
    case DP_ENC_I8:
        return 1u;
    case DP_ENC_U16:
    case DP_ENC_I16:
        return 2u;
    case DP_ENC_U32:
    case DP_ENC_I32:
        return 4u;
    default:
        return 0u;
    }
}

/* Write a field's scaled value big-endian at p. */
static void put_field(uint8_t *p, dp_enc_t enc, float value, float scale)
{
    switch (enc) {
    case DP_ENC_U8:
        p[0] = (uint8_t)sat_u(value, scale, 0xFFu);
        break;
    case DP_ENC_I8:
        p[0] = (uint8_t)(int8_t)sat_i(value, scale, -128, 127);
        break;
    case DP_ENC_U16: {
        const uint32_t v = sat_u(value, scale, 0xFFFFu);
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
        break;
    }
    case DP_ENC_I16: {
        const uint16_t v = (uint16_t)(int16_t)sat_i(value, scale, -32768, 32767);
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
        break;
    }
    case DP_ENC_U32: {
        const uint32_t v = sat_u(value, scale, 0xFFFFFFFFu);
        p[0] = (uint8_t)(v >> 24);
        p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);
        p[3] = (uint8_t)v;
        break;
    }
    case DP_ENC_I32: {
        /* Full int32 range; sat_i's float compare can't represent INT32 limits exactly, so scale
         * then clamp in double-free integer space via the float guard is adequate for our ranges.
         */
        const float s = value * scale + (value >= 0.0f ? 0.5f : -0.5f);
        int32_t v;
        if (s >= 2147483647.0f) {
            v = 2147483647;
        } else if (s <= -2147483648.0f) {
            v = -2147483647 - 1;
        } else {
            v = (int32_t)s;
        }
        const uint32_t u = (uint32_t)v;
        p[0] = (uint8_t)(u >> 24);
        p[1] = (uint8_t)(u >> 16);
        p[2] = (uint8_t)(u >> 8);
        p[3] = (uint8_t)u;
        break;
    }
    default:
        break;
    }
}

size_t dp_encode_payload(const device_profile_t *profile, const float *values, uint8_t flags,
                         uint8_t *out, size_t cap)
{
    if (profile == NULL || values == NULL || out == NULL) {
        return 0u;
    }
    const size_t total = profile->total_len;
    if (total < TELEMETRY_HEADER_LEN || cap < total) {
        return 0u;
    }

    out[0] = TELEMETRY_SCHEMA_VERSION;
    out[1] = profile->device_byte;
    out[2] = flags;

    for (uint8_t i = 0; i < profile->n_fields; ++i) {
        const dp_field_t *f = &profile->fields[i];
        const size_t end = (size_t)TELEMETRY_HEADER_LEN + f->offset + enc_size(f->enc);
        if (end > total) {
            return 0u; /* field would run past the declared frame */
        }
        put_field(&out[TELEMETRY_HEADER_LEN + f->offset], f->enc, values[f->value_index], f->scale);
    }
    return total;
}

/* Nominal wire value per encoding (~40% of the positive range) — keeps simulated engineering values
 * off the field limits so they encode without saturating and decode to sensible magnitudes. */
static float enc_nominal(dp_enc_t e)
{
    switch (e) {
    case DP_ENC_U8:
    case DP_ENC_I8:
        return 100.0f;
    case DP_ENC_U16:
    case DP_ENC_I16:
        return 3000.0f;
    case DP_ENC_U32:
    case DP_ENC_I32:
        return 100000.0f;
    default:
        return 1.0f;
    }
}

void dp_simulate(uint32_t tick, const device_profile_t *p, float *values, size_t n_values)
{
    if (p == NULL || values == NULL || n_values < p->n_meas) {
        return;
    }
    for (uint8_t i = 0; i < p->n_meas; ++i) {
        values[i] = 0.0f; /* measurands not carried in the payload stay 0 */
    }
    /* Size each measurand's value from the payload field that carries it (encoding + scale), so the
     * encoded frame doesn't saturate and decodes to a moderate value; a gentle per-field sinusoid
     * gives deterministic variation. At tick 0 the first field's value == enc_nominal/scale
     * exactly. */
    for (uint8_t f = 0; f < p->n_fields; ++f) {
        const dp_field_t *fld = &p->fields[f];
        if (fld->value_index >= p->n_meas) {
            continue;
        }
        const float scale = (fld->scale != 0.0f) ? fld->scale : 1.0f;
        const float nominal = enc_nominal(fld->enc) / scale;
        const float phase = (float)tick * 0.10f + (float)f * 0.70f;
        values[fld->value_index] = nominal * (1.0f + 0.08f * sinf(phase));
    }
}

/* ----------------------------------------------------------------- NVS blob */

/* CRC-16/MODBUS (poly 0xA001) — local copy so the pure core stays self-contained. */
static uint16_t dp_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* float <-> big-endian 4 bytes, endianness-independent (via the bit pattern). */
static void put_be_f32(uint8_t *p, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    put_be32(p, bits);
}

static float get_be_f32(const uint8_t *p)
{
    const uint32_t bits = get_be32(p);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

size_t dp_blob_size(const device_profile_t *p)
{
    if (p == NULL || p->n_meas > DP_MAX_MEAS || p->n_fields > DP_MAX_FIELDS) {
        return 0u;
    }
    return DP_BLOB_HEADER + (size_t)p->n_meas * DP_BLOB_MEAS_REC +
           (size_t)p->n_fields * DP_BLOB_FIELD_REC + DP_BLOB_CRC;
}

size_t dp_serialize(const device_profile_t *p, uint8_t *out, size_t cap)
{
    const size_t total = dp_blob_size(p);
    if (total == 0u || out == NULL || cap < total) {
        return 0u;
    }
    size_t o = 0;
    out[o++] = DP_BLOB_VERSION;
    out[o++] = p->device_byte;
    put_be32(&out[o], p->baud);
    o += 4;
    out[o++] = (uint8_t)p->parity;
    out[o++] = p->stop_bits;
    out[o++] = p->default_fc;
    out[o++] = (uint8_t)p->default_word;
    out[o++] = p->scan_fc;
    put_be16(&out[o], p->scan_reg);
    o += 2;
    put_be16(&out[o], p->scan_qty);
    o += 2;
    out[o++] = p->total_len;
    out[o++] = p->n_meas;
    out[o++] = p->n_fields;

    for (uint8_t i = 0; i < p->n_meas; ++i) {
        const dp_measurand_t *m = &p->meas[i];
        put_be16(&out[o], m->reg);
        o += 2;
        out[o++] = m->fc;
        out[o++] = (uint8_t)m->type;
        out[o++] = (uint8_t)m->word;
        put_be_f32(&out[o], m->scale);
        o += 4;
        put_be_f32(&out[o], m->offset);
        o += 4;
    }
    for (uint8_t i = 0; i < p->n_fields; ++i) {
        const dp_field_t *f = &p->fields[i];
        out[o++] = f->value_index;
        out[o++] = f->offset;
        out[o++] = (uint8_t)f->enc;
        put_be_f32(&out[o], f->scale);
        o += 4;
    }
    put_be16(&out[o], dp_crc16(out, o)); /* CRC over everything before the trailer */
    o += 2;
    return o;
}

bool dp_deserialize(const uint8_t *blob, size_t len, dp_profile_storage_t *store)
{
    if (blob == NULL || store == NULL || len < DP_BLOB_HEADER + DP_BLOB_CRC) {
        return false;
    }
    if (blob[0] != DP_BLOB_VERSION) {
        return false;
    }
    const uint8_t n_meas = blob[16];
    const uint8_t n_fields = blob[17];
    if (n_meas > DP_MAX_MEAS || n_fields > DP_MAX_FIELDS) {
        return false;
    }
    const size_t expect = DP_BLOB_HEADER + (size_t)n_meas * DP_BLOB_MEAS_REC +
                          (size_t)n_fields * DP_BLOB_FIELD_REC + DP_BLOB_CRC;
    if (len != expect) {
        return false;
    }
    if (dp_crc16(blob, len - DP_BLOB_CRC) != get_be16(&blob[len - DP_BLOB_CRC])) {
        return false;
    }

    device_profile_t *p = &store->profile;
    p->device_byte = blob[1];
    p->baud = get_be32(&blob[2]);
    p->parity = (char)blob[6];
    p->stop_bits = blob[7];
    p->default_fc = blob[8];
    p->default_word = (dp_word_t)blob[9];
    p->scan_fc = blob[10];
    p->scan_reg = get_be16(&blob[11]);
    p->scan_qty = get_be16(&blob[13]);
    p->total_len = blob[15];
    p->n_meas = n_meas;
    p->n_fields = n_fields;

    size_t o = DP_BLOB_HEADER;
    for (uint8_t i = 0; i < n_meas; ++i) {
        store->meas[i].reg = get_be16(&blob[o]);
        o += 2;
        store->meas[i].fc = blob[o++];
        store->meas[i].type = (dp_dtype_t)blob[o++];
        store->meas[i].word = (dp_word_t)blob[o++];
        store->meas[i].scale = get_be_f32(&blob[o]);
        o += 4;
        store->meas[i].offset = get_be_f32(&blob[o]);
        o += 4;
    }
    for (uint8_t i = 0; i < n_fields; ++i) {
        store->fields[i].value_index = blob[o++];
        store->fields[i].offset = blob[o++];
        store->fields[i].enc = (dp_enc_t)blob[o++];
        store->fields[i].scale = get_be_f32(&blob[o]);
        o += 4;
    }
    p->meas = store->meas;
    p->fields = store->fields;
    return true;
}
