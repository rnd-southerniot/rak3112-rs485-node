/*
 * modbus_rtu.c — pure Modbus RTU master framing. See modbus_rtu.h.
 * No SDK dependency: builds host-side for unit tests and on-target for the master driver.
 */
#include "modbus_rtu.h"

#include <string.h>

uint16_t modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}

size_t modbus_build_read(uint8_t *out, uint8_t slave, uint8_t func, uint16_t start_addr,
                         uint16_t qty)
{
    if (out == NULL) {
        return 0;
    }
    if (func != MODBUS_FC_READ_HOLDING_REGISTERS && func != MODBUS_FC_READ_INPUT_REGISTERS) {
        return 0;
    }
    if (qty < 1u || qty > MODBUS_MAX_READ_REGS) {
        return 0;
    }

    out[0] = slave;
    out[1] = func;
    out[2] = (uint8_t)(start_addr >> 8);
    out[3] = (uint8_t)(start_addr & 0xFFu);
    out[4] = (uint8_t)(qty >> 8);
    out[5] = (uint8_t)(qty & 0xFFu);

    const uint16_t crc = modbus_crc16(out, 6u);
    out[6] = (uint8_t)(crc & 0xFFu); /* CRC low byte first on the wire */
    out[7] = (uint8_t)(crc >> 8);
    return MODBUS_READ_REQ_LEN;
}

modbus_status_t modbus_parse_read_response(const uint8_t *adu, size_t len, uint8_t expect_slave,
                                           uint8_t expect_func, uint16_t expect_qty,
                                           uint16_t *regs_out, uint8_t *exception_out)
{
    if (adu == NULL || regs_out == NULL) {
        return MODBUS_ERR_ARG;
    }
    if (expect_qty < 1u || expect_qty > MODBUS_MAX_READ_REGS) {
        return MODBUS_ERR_ARG;
    }
    /* Shortest valid response is an exception: slave, func|0x80, code, crc_lo, crc_hi = 5 bytes. */
    if (len < 5u) {
        return MODBUS_ERR_SHORT;
    }

    /* CRC covers everything but the trailing 2 CRC bytes. Check it before trusting any field. */
    const uint16_t calc_crc = modbus_crc16(adu, len - 2u);
    const uint16_t wire_crc = (uint16_t)((uint16_t)adu[len - 2u] | ((uint16_t)adu[len - 1u] << 8));
    if (calc_crc != wire_crc) {
        return MODBUS_ERR_CRC;
    }

    if (adu[0] != expect_slave) {
        return MODBUS_ERR_SLAVE;
    }

    if (adu[1] == (uint8_t)(expect_func | 0x80u)) {
        if (exception_out != NULL) {
            *exception_out = adu[2];
        }
        return MODBUS_ERR_EXCEPTION;
    }
    if (adu[1] != expect_func) {
        return MODBUS_ERR_FUNC;
    }

    /* Normal read response: slave, func, byte_count, data[2*qty], crc_lo, crc_hi. */
    const uint8_t byte_count = adu[2];
    if (byte_count != (uint8_t)(2u * expect_qty)) {
        return MODBUS_ERR_BYTECOUNT;
    }
    if (len != (size_t)(5u + 2u * expect_qty)) {
        return MODBUS_ERR_BYTECOUNT;
    }

    for (uint16_t i = 0; i < expect_qty; ++i) {
        const size_t off = (size_t)3u + (size_t)(2u * i);
        regs_out[i] = (uint16_t)(((uint16_t)adu[off] << 8) | (uint16_t)adu[off + 1u]);
    }
    return MODBUS_OK;
}

float modbus_regs_to_f32(const uint16_t *regs, modbus_word_order_t order)
{
    if (regs == NULL) {
        return 0.0f;
    }
    const uint16_t hi = (order == MODBUS_WORD_ORDER_CDAB) ? regs[1] : regs[0];
    const uint16_t lo = (order == MODBUS_WORD_ORDER_CDAB) ? regs[0] : regs[1];
    const uint32_t bits = ((uint32_t)hi << 16) | (uint32_t)lo;
    float f;
    memcpy(&f, &bits, sizeof(f)); /* type-pun without aliasing UB */
    return f;
}
