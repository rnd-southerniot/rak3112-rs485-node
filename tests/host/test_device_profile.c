/*
 * Host tests for the profile-driven core (ADR-006): dp_decode (all types/word orders) and
 * dp_encode_payload (generic ADR-005 frame). Same CHECK harness as the other host tests.
 */
#include "device_profile.h"
#include "telemetry.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                 \
        }                                                                                          \
    } while (0)

static int approx(float a, float b)
{
    return (a - b) < 0.01f && (b - a) < 0.01f;
}

static void test_dtype_regs(void)
{
    CHECK(dp_dtype_regs(DP_U16) == 1);
    CHECK(dp_dtype_regs(DP_I16) == 1);
    CHECK(dp_dtype_regs(DP_U32) == 2);
    CHECK(dp_dtype_regs(DP_I32) == 2);
    CHECK(dp_dtype_regs(DP_F32) == 2);
}

static void test_decode_16(void)
{
    uint16_t r = 2304;
    CHECK(approx(dp_decode(&r, DP_U16, DP_ABCD, 0.1f, 0.0f), 230.4f)); /* MFM384-style /10 sanity */
    uint16_t neg = 0xFFDD;                                             /* -35 */
    CHECK(approx(dp_decode(&neg, DP_I16, DP_ABCD, 0.1f, 0.0f), -3.5f));
    uint16_t u = 0xFFDD;
    CHECK(approx(dp_decode(&u, DP_U16, DP_ABCD, 1.0f, 0.0f), 65501.0f)); /* unsigned, no sign */
}

static void test_decode_32_word_orders(void)
{
    /* 1000.0f == 0x447A0000. Lay the four wire bytes 44 7A 00 00 out per order so each decodes
     * 1000. */
    const uint16_t abcd[2] = {0x447A, 0x0000};
    const uint16_t cdab[2] = {0x0000, 0x447A};
    const uint16_t badc[2] = {0x7A44, 0x0000};
    const uint16_t dcba[2] = {0x0000, 0x7A44};
    CHECK(approx(dp_decode(abcd, DP_F32, DP_ABCD, 1.0f, 0.0f), 1000.0f));
    CHECK(approx(dp_decode(cdab, DP_F32, DP_CDAB, 1.0f, 0.0f), 1000.0f));
    CHECK(approx(dp_decode(badc, DP_F32, DP_BADC, 1.0f, 0.0f), 1000.0f));
    CHECK(approx(dp_decode(dcba, DP_F32, DP_DCBA, 1.0f, 0.0f), 1000.0f));

    /* u32 ABCD with scale (EEM400 energy style): 0x000DEC9F = 912543, x0.1 = 91254.3 kWh. */
    const uint16_t e[2] = {0x000D, 0xEC9F};
    CHECK(approx(dp_decode(e, DP_U32, DP_ABCD, 0.1f, 0.0f), 91254.3f));

    /* i32 ABCD signed (DSE total watts can be negative): 0xFFFFFFD8 = -40. */
    const uint16_t w[2] = {0xFFFF, 0xFFD8};
    CHECK(approx(dp_decode(w, DP_I32, DP_ABCD, 1.0f, 0.0f), -40.0f));
}

/* A small MFM384-shaped profile: 3 voltages + total kW + freq + kWh, mirroring the Phase 6c bytes.
 */
static const dp_measurand_t MEAS[] = {
    {0, 4, DP_F32, DP_CDAB, 1.0f, 0.0f},  /* 0 v1n */
    {2, 4, DP_F32, DP_CDAB, 1.0f, 0.0f},  /* 1 v2n */
    {4, 4, DP_F32, DP_CDAB, 1.0f, 0.0f},  /* 2 v3n */
    {44, 4, DP_F32, DP_CDAB, 1.0f, 0.0f}, /* 3 total_kw */
    {56, 4, DP_F32, DP_CDAB, 1.0f, 0.0f}, /* 4 freq */
    {58, 4, DP_F32, DP_CDAB, 1.0f, 0.0f}, /* 5 total_kwh */
};
static const dp_field_t FIELDS[] = {
    {0, 0, DP_ENC_U16, 10.0f},   /* v1n x10 */
    {1, 2, DP_ENC_U16, 10.0f},   /* v2n x10 */
    {2, 4, DP_ENC_U16, 10.0f},   /* v3n x10 */
    {3, 6, DP_ENC_I16, 10.0f},   /* total_kw x10 */
    {4, 8, DP_ENC_U16, 100.0f},  /* freq x100 */
    {5, 10, DP_ENC_U32, 100.0f}, /* total_kwh x100 */
};
static const device_profile_t PROFILE = {
    .device_byte = 0x01,
    .baud = 9600,
    .parity = 'N',
    .stop_bits = 1,
    .default_fc = 4,
    .default_word = DP_CDAB,
    .scan_fc = 3,
    .scan_reg = 6,
    .scan_qty = 1,
    .n_meas = 6,
    .meas = MEAS,
    .total_len = 17, /* 3 header + 14 body */
    .n_fields = 6,
    .fields = FIELDS,
};

static void test_encode_payload(void)
{
    const float values[6] = {230.4f, 231.0f, 229.8f, 5.0f, 50.0f, 1000.0f};
    uint8_t out[64] = {0};
    const size_t n =
        dp_encode_payload(&PROFILE, values, TELEMETRY_FLAG_SIMULATED, out, sizeof(out));
    CHECK(n == 17u);
    const uint8_t expect[17] = {
        0x01, 0x01, 0x01,       /* schema, device 0x01, flags=simulated */
        0x09, 0x00,             /* v1n 230.4 x10 = 2304 */
        0x09, 0x06,             /* v2n 231.0 x10 = 2310 */
        0x08, 0xFA,             /* v3n 229.8 x10 = 2298 */
        0x00, 0x32,             /* total_kw 5.0 x10 = 50 */
        0x13, 0x88,             /* freq 50.0 x100 = 5000 */
        0x00, 0x01, 0x86, 0xA0, /* kwh 1000 x100 = 100000 */
    };
    CHECK(memcmp(out, expect, sizeof(expect)) == 0);

    /* Negative power encodes as two's-complement i16. */
    const float v2[6] = {0, 0, 0, -3.5f, 0, 0};
    (void)dp_encode_payload(&PROFILE, v2, 0, out, sizeof(out));
    CHECK(out[3 + 6] == 0xFF && out[3 + 7] == 0xDD); /* -35 */

    /* cap too small rejected. */
    CHECK(dp_encode_payload(&PROFILE, values, 0, out, 16u) == 0u);
}

static void test_blob_roundtrip(void)
{
    uint8_t blob[512] = {0};
    const size_t want =
        DP_BLOB_HEADER + 6u * DP_BLOB_MEAS_REC + 6u * DP_BLOB_FIELD_REC + DP_BLOB_CRC;
    CHECK(dp_blob_size(&PROFILE) == want);

    const size_t n = dp_serialize(&PROFILE, blob, sizeof(blob));
    CHECK(n == want);

    dp_profile_storage_t store;
    CHECK(dp_deserialize(blob, n, &store));
    const device_profile_t *q = &store.profile;
    CHECK(q->device_byte == 0x01 && q->baud == 9600 && q->parity == 'N' && q->stop_bits == 1);
    CHECK(q->default_fc == 4 && q->default_word == DP_CDAB);
    CHECK(q->scan_fc == 3 && q->scan_reg == 6 && q->scan_qty == 1);
    CHECK(q->total_len == 17 && q->n_meas == 6 && q->n_fields == 6);
    /* measurand + field tables survive intact */
    CHECK(q->meas[5].reg == 58 && q->meas[5].type == DP_F32 && q->meas[5].word == DP_CDAB);
    CHECK(q->fields[5].value_index == 5 && q->fields[5].offset == 10 &&
          q->fields[5].enc == DP_ENC_U32 && approx(q->fields[5].scale, 100.0f));

    /* The deserialized profile encodes the same bytes as the original (end-to-end). */
    const float values[6] = {230.4f, 231.0f, 229.8f, 5.0f, 50.0f, 1000.0f};
    uint8_t a[64] = {0}, b[64] = {0};
    const size_t la = dp_encode_payload(&PROFILE, values, 0, a, sizeof(a));
    const size_t lb = dp_encode_payload(q, values, 0, b, sizeof(b));
    CHECK(la == lb && la > 0 && memcmp(a, b, la) == 0);
}

static void test_blob_rejects(void)
{
    uint8_t blob[512] = {0};
    const size_t n = dp_serialize(&PROFILE, blob, sizeof(blob));
    dp_profile_storage_t store;

    /* CRC corruption rejected. */
    uint8_t bad[512];
    memcpy(bad, blob, n);
    bad[20] ^= 0xFFu;
    CHECK(!dp_deserialize(bad, n, &store));

    /* Wrong version rejected. */
    memcpy(bad, blob, n);
    bad[0] = 0x99u;
    CHECK(!dp_deserialize(bad, n, &store));

    /* Truncated length rejected. */
    CHECK(!dp_deserialize(blob, n - 1u, &store));

    /* Serialize rejects a too-small buffer. */
    CHECK(dp_serialize(&PROFILE, blob, n - 1u) == 0u);
}

int main(void)
{
    test_dtype_regs();
    test_decode_16();
    test_decode_32_word_orders();
    test_encode_payload();
    test_blob_roundtrip();
    test_blob_rejects();
    printf("device_profile: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
