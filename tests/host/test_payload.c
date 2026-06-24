/*
 * Host unit tests for the ADR-005 payload encoder (pure C). Same CHECK harness as the others.
 */
#include "payload.h"

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

static void test_mfm384_exact_bytes(void)
{
    const mfm384_sample_t s = {
        .v1n = 230.4f,
        .v2n = 231.0f,
        .v3n = 229.8f,
        .total_kw = 5.0f,
        .total_kwh = 1000.0f,
        .freq = 50.0f,
        .avg_pf = 0.95f,
    };
    uint8_t out[PAYLOAD_MAX] = {0};
    const size_t n = payload_encode_mfm384(&s, TELEMETRY_FLAG_SIMULATED, out, sizeof(out));
    CHECK(n == 19u);
    const uint8_t expect[19] = {
        0x01, 0x01, 0x01,       /* ver, device=MFM384, flags=simulated */
        0x09, 0x00,             /* V1N 230.4 ×10 = 2304 */
        0x09, 0x06,             /* V2N 231.0 ×10 = 2310 */
        0x08, 0xFA,             /* V3N 229.8 ×10 = 2298 */
        0x00, 0x32,             /* Total kW 5.0 ×10 = 50 */
        0x00, 0x01, 0x86, 0xA0, /* Total kWh 1000.0 ×100 = 100000 */
        0x13, 0x88,             /* Freq 50.0 ×100 = 5000 */
        0x03, 0xB6,             /* Avg PF 0.95 ×1000 = 950 */
    };
    CHECK(memcmp(out, expect, sizeof(expect)) == 0);
}

static void test_mfm384_signed_and_saturation(void)
{
    uint8_t out[PAYLOAD_MAX] = {0};

    /* Negative power and PF encode as two's-complement int16. */
    mfm384_sample_t s = {.total_kw = -3.5f, .avg_pf = -0.5f};
    (void)payload_encode_mfm384(&s, 0, out, sizeof(out));
    CHECK(out[9] == 0xFF && out[10] == 0xDD);  /* -35 */
    CHECK(out[17] == 0xFE && out[18] == 0x0C); /* -500 */

    /* Energy beyond uint32 range saturates, not wraps. */
    s = (mfm384_sample_t){.total_kwh = 1.0e9f}; /* ×100 = 1e11 > 0xFFFFFFFF */
    (void)payload_encode_mfm384(&s, 0, out, sizeof(out));
    CHECK(out[11] == 0xFF && out[12] == 0xFF && out[13] == 0xFF && out[14] == 0xFF);

    /* Negative voltage clamps to 0 (unsigned field). */
    s = (mfm384_sample_t){.v1n = -5.0f};
    (void)payload_encode_mfm384(&s, 0, out, sizeof(out));
    CHECK(out[3] == 0x00 && out[4] == 0x00);
}

static void test_rsfsjt_and_argchecks(void)
{
    const rsfsjt_sample_t w = {.wind_mps = 12.34f};
    uint8_t out[PAYLOAD_MAX] = {0};
    const size_t n = payload_encode_rsfsjt(&w, 0, out, sizeof(out));
    CHECK(n == 5u);
    const uint8_t expect[5] = {0x01, 0x02, 0x00, 0x04, 0xD2}; /* 12.34 ×100 = 1234 */
    CHECK(memcmp(out, expect, sizeof(expect)) == 0);

    /* Bad args / insufficient capacity rejected. */
    CHECK(payload_encode_mfm384(NULL, 0, out, sizeof(out)) == 0u);
    CHECK(payload_encode_mfm384(&(mfm384_sample_t){0}, 0, out, 18u) == 0u);
    CHECK(payload_encode_rsfsjt(&w, 0, out, 4u) == 0u);
}

int main(void)
{
    test_mfm384_exact_bytes();
    test_mfm384_signed_and_saturation();
    test_rsfsjt_and_argchecks();

    printf("payload: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
