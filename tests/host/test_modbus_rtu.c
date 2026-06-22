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

int main(void)
{
    test_crc16_kat();
    test_build_read();
    test_parse_ok();
    test_parse_exception();
    test_parse_errors();

    printf("modbus_rtu: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
