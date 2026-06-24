/*
 * payload.c — compact versioned binary encoder (ADR-005). Pure C, no SDK. See payload.h.
 */
#include "payload.h"

/* Saturating float -> scaled unsigned integer. */
static uint32_t sat_u32(float v, float scale, uint32_t max)
{
    if (v <= 0.0f) {
        return 0u;
    }
    const float scaled = v * scale + 0.5f; /* round to nearest */
    if (scaled >= (float)max) {
        return max;
    }
    return (uint32_t)scaled;
}

static uint16_t sat_u16(float v, float scale)
{
    return (uint16_t)sat_u32(v, scale, 0xFFFFu);
}

/* Saturating float -> scaled signed 16-bit. */
static int16_t sat_i16(float v, float scale)
{
    const float scaled = v * scale + (v >= 0.0f ? 0.5f : -0.5f);
    if (scaled >= 32767.0f) {
        return 32767;
    }
    if (scaled <= -32768.0f) {
        return -32768;
    }
    return (int16_t)scaled;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFFu);
}

static size_t put_header(uint8_t *out, telemetry_device_t dev, uint8_t flags)
{
    out[0] = TELEMETRY_SCHEMA_VERSION;
    out[1] = (uint8_t)dev;
    out[2] = flags;
    return 3u;
}

size_t payload_encode_mfm384(const mfm384_sample_t *s, uint8_t flags, uint8_t *out, size_t cap)
{
    if (s == NULL || out == NULL || cap < 19u) {
        return 0u;
    }
    size_t n = put_header(out, TELEMETRY_DEV_MFM384, flags);
    put_u16(&out[n], sat_u16(s->v1n, 10.0f));
    n += 2u;
    put_u16(&out[n], sat_u16(s->v2n, 10.0f));
    n += 2u;
    put_u16(&out[n], sat_u16(s->v3n, 10.0f));
    n += 2u;
    put_u16(&out[n], (uint16_t)sat_i16(s->total_kw, 10.0f)); /* signed, wire as two's complement */
    n += 2u;
    put_u32(&out[n], sat_u32(s->total_kwh, 100.0f, 0xFFFFFFFFu));
    n += 4u;
    put_u16(&out[n], sat_u16(s->freq, 100.0f));
    n += 2u;
    put_u16(&out[n], (uint16_t)sat_i16(s->avg_pf, 1000.0f));
    n += 2u;
    return n; /* 19 */
}

size_t payload_encode_rsfsjt(const rsfsjt_sample_t *s, uint8_t flags, uint8_t *out, size_t cap)
{
    if (s == NULL || out == NULL || cap < 5u) {
        return 0u;
    }
    size_t n = put_header(out, TELEMETRY_DEV_RSFSJT, flags);
    put_u16(&out[n], sat_u16(s->wind_mps, 100.0f));
    n += 2u;
    return n; /* 5 */
}
