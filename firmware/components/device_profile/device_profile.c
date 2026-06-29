/*
 * device_profile.c — pure numeric core for the data-driven runtime (ADR-006). See device_profile.h.
 * No SDK dependency: builds host-side under ctest and on-target for the generic reader.
 */
#include "device_profile.h"

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
