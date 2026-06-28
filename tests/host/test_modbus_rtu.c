/*
 * Host unit tests for modbus_rtu (pure C, no SDK). Compiled and run by ctest.
 * Same minimal CHECK harness as test_ring_buffer.c: main() exits non-zero on any failure.
 */
#include "modbus_rtu.h"

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

/* CRC-16/MODBUS standard known-answer test: ASCII "123456789" → 0x4B37. */
static void test_crc16_kat(void)
{
    const uint8_t kat[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(modbus_crc16(kat, sizeof(kat)) == 0x4B37u);
    /* Empty input returns the init value. */
    CHECK(modbus_crc16(kat, 0) == 0xFFFFu);
}

static void test_build_read(void)
{
    /* Read 2 holding registers from addr 0x0000, slave 1. */
    uint8_t req[MODBUS_READ_REQ_LEN] = {0};
    const size_t n = modbus_build_read(req, 1, MODBUS_FC_READ_HOLDING_REGISTERS, 0x0000, 2);
    CHECK(n == MODBUS_READ_REQ_LEN);
    const uint8_t expect[6] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02};
    CHECK(memcmp(req, expect, 6) == 0);
    /* CRC appended low byte first, and self-consistent with modbus_crc16 over the first 6 bytes. */
    const uint16_t crc = modbus_crc16(req, 6);
    CHECK(req[6] == (uint8_t)(crc & 0xFFu));
    CHECK(req[7] == (uint8_t)(crc >> 8));

    /* Bad args rejected. */
    CHECK(modbus_build_read(NULL, 1, 0x03, 0, 1) == 0);
    CHECK(modbus_build_read(req, 1, 0x06 /* not a read FC */, 0, 1) == 0);
    CHECK(modbus_build_read(req, 1, 0x03, 0, 0) == 0);
    CHECK(modbus_build_read(req, 1, 0x03, 0, MODBUS_MAX_READ_REGS + 1) == 0);
}

/* Helper: append a valid CRC to an ADU body of `body_len` bytes; returns total length. */
static size_t frame_with_crc(uint8_t *adu, size_t body_len)
{
    const uint16_t crc = modbus_crc16(adu, body_len);
    adu[body_len] = (uint8_t)(crc & 0xFFu);
    adu[body_len + 1] = (uint8_t)(crc >> 8);
    return body_len + 2u;
}

static void test_parse_ok(void)
{
    /* Response to "read 2 holding regs": slave 1, fc 3, bc 4, regs 0x1234, 0xABCD. */
    uint8_t adu[16] = {0x01, 0x03, 0x04, 0x12, 0x34, 0xAB, 0xCD};
    const size_t len = frame_with_crc(adu, 7);
    uint16_t regs[2] = {0};
    uint8_t exc = 0xFF;
    const modbus_status_t st =
        modbus_parse_read_response(adu, len, 1, MODBUS_FC_READ_HOLDING_REGISTERS, 2, regs, &exc);
    CHECK(st == MODBUS_OK);
    CHECK(regs[0] == 0x1234u);
    CHECK(regs[1] == 0xABCDu);
}

static void test_parse_exception(void)
{
    /* Exception: slave 1, fc 0x83 (=0x03|0x80), code 0x02 (illegal data address). */
    uint8_t adu[8] = {0x01, 0x83, 0x02};
    const size_t len = frame_with_crc(adu, 3);
    uint16_t regs[2] = {0};
    uint8_t exc = 0;
    const modbus_status_t st =
        modbus_parse_read_response(adu, len, 1, MODBUS_FC_READ_HOLDING_REGISTERS, 2, regs, &exc);
    CHECK(st == MODBUS_ERR_EXCEPTION);
    CHECK(exc == 0x02u);
}

static void test_parse_errors(void)
{
    uint16_t regs[2] = {0};

    /* CRC mismatch: build a good frame then corrupt a data byte. */
    uint8_t adu[16] = {0x01, 0x03, 0x04, 0x12, 0x34, 0xAB, 0xCD};
    size_t len = frame_with_crc(adu, 7);
    adu[3] ^= 0xFFu; /* corrupt after CRC computed */
    CHECK(modbus_parse_read_response(adu, len, 1, 0x03, 2, regs, NULL) == MODBUS_ERR_CRC);

    /* Too short. */
    uint8_t shortf[4] = {0x01, 0x03, 0x00, 0x00};
    CHECK(modbus_parse_read_response(shortf, 4, 1, 0x03, 2, regs, NULL) == MODBUS_ERR_SHORT);

    /* Wrong slave. */
    uint8_t a2[16] = {0x02, 0x03, 0x04, 0x12, 0x34, 0xAB, 0xCD};
    len = frame_with_crc(a2, 7);
    CHECK(modbus_parse_read_response(a2, len, 1, 0x03, 2, regs, NULL) == MODBUS_ERR_SLAVE);

    /* Wrong function (not the request's, not its exception). */
    uint8_t a3[16] = {0x01, 0x04, 0x04, 0x12, 0x34, 0xAB, 0xCD};
    len = frame_with_crc(a3, 7);
    CHECK(modbus_parse_read_response(a3, len, 1, 0x03, 2, regs, NULL) == MODBUS_ERR_FUNC);

    /* Byte-count / length mismatch: claims 2 regs but only carries 1. */
    uint8_t a4[16] = {0x01, 0x03, 0x02, 0x12, 0x34};
    len = frame_with_crc(a4, 5);
    CHECK(modbus_parse_read_response(a4, len, 1, 0x03, 2, regs, NULL) == MODBUS_ERR_BYTECOUNT);

    /* Bad caller args. */
    CHECK(modbus_parse_read_response(NULL, len, 1, 0x03, 2, regs, NULL) == MODBUS_ERR_ARG);
    CHECK(modbus_parse_read_response(a4, len, 1, 0x03, 0, regs, NULL) == MODBUS_ERR_ARG);
}

/* IEEE-754 float32 reconstruction from two registers, both word orders. */
static void test_regs_to_f32(void)
{
    /* 1000.0f == 0x447A0000. ABCD: high word in regs[0]. CDAB: high word in regs[1]. */
    const uint16_t abcd[2] = {0x447Au, 0x0000u};
    const uint16_t cdab[2] = {0x0000u, 0x447Au};
    CHECK(modbus_regs_to_f32(abcd, MODBUS_WORD_ORDER_ABCD) == 1000.0f);
    CHECK(modbus_regs_to_f32(cdab, MODBUS_WORD_ORDER_CDAB) == 1000.0f);
    /* Cross-check the orders are genuinely distinct (ABCD of the CDAB layout != 1000.0f). */
    CHECK(modbus_regs_to_f32(cdab, MODBUS_WORD_ORDER_ABCD) != 1000.0f);

    /* A non-trivial value: 12.34f == 0x41454B85 (A=0x41,B=0x45,C=0x4B,D=0x85). */
    const uint16_t v[2] = {0x4145u, 0x4B85u};
    const float f = modbus_regs_to_f32(v, MODBUS_WORD_ORDER_ABCD);
    CHECK(f > 12.33f && f < 12.35f);

    /* 0.0f and NULL safety. */
    const uint16_t zero[2] = {0x0000u, 0x0000u};
    CHECK(modbus_regs_to_f32(zero, MODBUS_WORD_ORDER_ABCD) == 0.0f);
    CHECK(modbus_regs_to_f32(NULL, MODBUS_WORD_ORDER_ABCD) == 0.0f);
}

static void test_build_write_single(void)
{
    /* Write value 0x4001 to holding register 0x1801, slave 1 (an IG28ET jog control word). */
    uint8_t req[MODBUS_WRITE_SINGLE_LEN] = {0};
    const size_t n = modbus_build_write_single(req, 1, 0x1801, 0x4001);
    CHECK(n == MODBUS_WRITE_SINGLE_LEN);
    const uint8_t expect[6] = {0x01, 0x06, 0x18, 0x01, 0x40, 0x01};
    CHECK(memcmp(req, expect, 6) == 0);
    const uint16_t crc = modbus_crc16(req, 6);
    CHECK(req[6] == (uint8_t)(crc & 0xFFu)); /* CRC low byte first */
    CHECK(req[7] == (uint8_t)(crc >> 8));
    /* Null out rejected. */
    CHECK(modbus_build_write_single(NULL, 1, 0x1801, 0x4001) == 0);
}

static void test_parse_write_single(void)
{
    uint8_t exc = 0;

    /* Normal response echoes the request verbatim → MODBUS_OK when addr+value match. */
    uint8_t ok[MODBUS_WRITE_SINGLE_LEN] = {0x01, 0x06, 0x18, 0x01, 0x40, 0x01};
    size_t len = frame_with_crc(ok, 6);
    CHECK(modbus_parse_write_single_response(ok, len, 1, 0x1801, 0x4001, &exc) == MODBUS_OK);

    /* Exception (0x86 = 0x06|0x80), code 0x02. */
    uint8_t ex[8] = {0x01, 0x86, 0x02};
    len = frame_with_crc(ex, 3);
    exc = 0;
    CHECK(modbus_parse_write_single_response(ex, len, 1, 0x1801, 0x4001, &exc) ==
          MODBUS_ERR_EXCEPTION);
    CHECK(exc == 0x02u);

    /* Echo mismatch: slave wrote a different value than asked → MODBUS_ERR_BYTECOUNT. */
    uint8_t mm[MODBUS_WRITE_SINGLE_LEN] = {0x01, 0x06, 0x18, 0x01, 0x40, 0x02};
    len = frame_with_crc(mm, 6);
    CHECK(modbus_parse_write_single_response(mm, len, 1, 0x1801, 0x4001, NULL) ==
          MODBUS_ERR_BYTECOUNT);

    /* Wrong slave, CRC corruption, too-short, and null all rejected. */
    uint8_t ws[MODBUS_WRITE_SINGLE_LEN] = {0x02, 0x06, 0x18, 0x01, 0x40, 0x01};
    len = frame_with_crc(ws, 6);
    CHECK(modbus_parse_write_single_response(ws, len, 1, 0x1801, 0x4001, NULL) == MODBUS_ERR_SLAVE);

    uint8_t bad[MODBUS_WRITE_SINGLE_LEN] = {0x01, 0x06, 0x18, 0x01, 0x40, 0x01};
    len = frame_with_crc(bad, 6);
    bad[4] ^= 0xFFu; /* corrupt after CRC computed */
    CHECK(modbus_parse_write_single_response(bad, len, 1, 0x1801, 0x4001, NULL) == MODBUS_ERR_CRC);

    uint8_t shortf[4] = {0x01, 0x06, 0x18, 0x01};
    CHECK(modbus_parse_write_single_response(shortf, 4, 1, 0x1801, 0x4001, NULL) ==
          MODBUS_ERR_SHORT);
    CHECK(modbus_parse_write_single_response(NULL, len, 1, 0x1801, 0x4001, NULL) == MODBUS_ERR_ARG);
}

static void test_build_write_multi(void)
{
    /* Golden vector from the LEESN 485 manual (run-to-absolute-position 10000 @ 0x00D0):
     * 01 10 00 D0 00 02 04 27 10 00 00 F5 82 — qty 2, low word 0x2710 first, high word 0x0000. */
    uint8_t out[16] = {0};
    const uint16_t regs[2] = {0x2710u, 0x0000u};
    const size_t n = modbus_build_write_multi(out, sizeof(out), 1, 0x00D0, 2, regs);
    const uint8_t expect[13] = {0x01, 0x10, 0x00, 0xD0, 0x00, 0x02, 0x04,
                                0x27, 0x10, 0x00, 0x00, 0xF5, 0x82};
    CHECK(n == 13u); /* 9 + 2*qty */
    CHECK(memcmp(out, expect, 13) == 0);

    /* Bad args: null out/regs, qty 0, qty over cap, and too-small buffer all rejected. */
    CHECK(modbus_build_write_multi(NULL, sizeof(out), 1, 0x00D0, 2, regs) == 0);
    CHECK(modbus_build_write_multi(out, sizeof(out), 1, 0x00D0, 2, NULL) == 0);
    CHECK(modbus_build_write_multi(out, sizeof(out), 1, 0x00D0, 0, regs) == 0);
    CHECK(modbus_build_write_multi(out, 8, 1, 0x00D0, 2, regs) == 0); /* needs 13, cap 8 */
}

static void test_parse_write_multi(void)
{
    uint8_t exc = 0;
    /* Normal response echoes addr + qty: 01 10 00 D0 00 02 + CRC. */
    uint8_t ok[MODBUS_WRITE_MULTI_RESP_LEN] = {0x01, 0x10, 0x00, 0xD0, 0x00, 0x02};
    size_t len = frame_with_crc(ok, 6);
    CHECK(modbus_parse_write_multi_response(ok, len, 1, 0x00D0, 2, &exc) == MODBUS_OK);

    /* Exception 0x90, code 0x03. */
    uint8_t ex[8] = {0x01, 0x90, 0x03};
    len = frame_with_crc(ex, 3);
    CHECK(modbus_parse_write_multi_response(ex, len, 1, 0x00D0, 2, &exc) == MODBUS_ERR_EXCEPTION);
    CHECK(exc == 0x03u);

    /* qty echo mismatch -> BYTECOUNT. */
    uint8_t mm[MODBUS_WRITE_MULTI_RESP_LEN] = {0x01, 0x10, 0x00, 0xD0, 0x00, 0x01};
    len = frame_with_crc(mm, 6);
    CHECK(modbus_parse_write_multi_response(mm, len, 1, 0x00D0, 2, NULL) == MODBUS_ERR_BYTECOUNT);
}

int main(void)
{
    test_crc16_kat();
    test_build_read();
    test_parse_ok();
    test_parse_exception();
    test_parse_errors();
    test_regs_to_f32();
    test_build_write_single();
    test_parse_write_single();
    test_build_write_multi();
    test_parse_write_multi();

    printf("modbus_rtu: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
