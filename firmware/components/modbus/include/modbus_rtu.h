/*
 * modbus_rtu.h — pure Modbus RTU master framing (Phase 6, ADR-005 prep).
 *
 * Transport-free: builds request ADUs and parses response ADUs (CRC-16/MODBUS, exception
 * decoding, register extraction) with no SDK dependency, so the logic is unit-tested host-side
 * (tests/host) under -Wall -Wextra -Werror. The on-target transaction layer (modbus_master.*)
 * drives these over the Phase 4 rs485 transport. Master read path only for now
 * (FC03 holding / FC04 input registers) — the configurable register set this node samples.
 */
#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <stddef.h>
#include <stdint.h>

/* Read function codes (master). */
#define MODBUS_FC_READ_HOLDING_REGISTERS 0x03u
#define MODBUS_FC_READ_INPUT_REGISTERS 0x04u

/* Write function codes (master). FC06 writes one 16-bit holding register; FC16 writes several
 * consecutive registers in one transaction (needed for 32-bit values across a register pair). */
#define MODBUS_FC_WRITE_SINGLE_REGISTER 0x06u
#define MODBUS_FC_WRITE_MULTIPLE_REGISTERS 0x10u

/* FC16 request = 7-byte header (slave,func,addr_hi,addr_lo,qty_hi,qty_lo,bytecount) + 2*qty data
 * + 2 CRC. Its normal response is 8 bytes (slave,func,addr_hi,addr_lo,qty_hi,qty_lo,crc_lo,crc_hi).
 */
#define MODBUS_WRITE_MULTI_RESP_LEN 8u
#define MODBUS_MAX_WRITE_REGS 123u

/* A read-registers request ADU is always 8 bytes: slave, func, addr_hi, addr_lo, qty_hi,
 * qty_lo, crc_lo, crc_hi. */
#define MODBUS_READ_REQ_LEN 8u

/* A write-single-register request — and its normal response — is 8 bytes: slave, func,
 * addr_hi, addr_lo, val_hi, val_lo, crc_lo, crc_hi. The slave echoes the request verbatim. */
#define MODBUS_WRITE_SINGLE_LEN 8u

/* Max registers per FC03/FC04 read (2 bytes each → 250 data bytes, fits the 256-byte RTU ADU). */
#define MODBUS_MAX_READ_REGS 125u

typedef enum {
    MODBUS_OK = 0,
    MODBUS_ERR_ARG = -1,       /* bad caller arguments */
    MODBUS_ERR_SHORT = -2,     /* response shorter than the minimum valid frame */
    MODBUS_ERR_CRC = -3,       /* CRC-16 mismatch */
    MODBUS_ERR_SLAVE = -4,     /* unexpected slave address */
    MODBUS_ERR_FUNC = -5,      /* unexpected function code (not the request's, not its exception) */
    MODBUS_ERR_BYTECOUNT = -6, /* byte-count / length disagrees with the requested quantity */
    MODBUS_ERR_EXCEPTION = -7, /* slave returned a Modbus exception; code in *exception_out */
} modbus_status_t;

/* CRC-16/MODBUS (poly 0xA001, init 0xFFFF, no final XOR). Known-answer: "123456789" → 0x4B37. */
uint16_t modbus_crc16(const uint8_t *data, size_t len);

/*
 * Build a read-registers request into `out` (must hold MODBUS_READ_REQ_LEN bytes). CRC is
 * appended low byte first (Modbus wire order). Returns MODBUS_READ_REQ_LEN, or 0 on bad args
 * (null out, func not FC03/FC04, qty 0 or > MODBUS_MAX_READ_REGS).
 */
size_t modbus_build_read(uint8_t *out, uint8_t slave, uint8_t func, uint16_t start_addr,
                         uint16_t qty);

/*
 * Parse a read-registers response ADU. Validates length, CRC, slave, and function; decodes a
 * Modbus exception (func | 0x80) into *exception_out (if non-null) returning MODBUS_ERR_EXCEPTION.
 * On MODBUS_OK, writes `expect_qty` big-endian register words into regs_out[0..expect_qty-1].
 */
modbus_status_t modbus_parse_read_response(const uint8_t *adu, size_t len, uint8_t expect_slave,
                                           uint8_t expect_func, uint16_t expect_qty,
                                           uint16_t *regs_out, uint8_t *exception_out);

/*
 * Build a write-single-register (FC06) request into `out` (must hold MODBUS_WRITE_SINGLE_LEN
 * bytes). CRC appended low byte first. Returns MODBUS_WRITE_SINGLE_LEN, or 0 on a null pointer.
 * NOTE: FC06 writes a device register — on a motor drive this can COMMAND MOTION. Callers must
 * gate that with hardware-safety checks; the framing layer itself is value-agnostic.
 */
size_t modbus_build_write_single(uint8_t *out, uint8_t slave, uint16_t addr, uint16_t value);

/*
 * Parse a write-single-register (FC06) response ADU. A normal response echoes the request, so
 * this validates length, CRC, slave, function, and that the echoed addr/value match what was
 * requested. Decodes an exception (0x86) into *exception_out returning MODBUS_ERR_EXCEPTION.
 * An addr/value echo mismatch returns MODBUS_ERR_BYTECOUNT.
 */
modbus_status_t modbus_parse_write_single_response(const uint8_t *adu, size_t len,
                                                   uint8_t expect_slave, uint16_t expect_addr,
                                                   uint16_t expect_value, uint8_t *exception_out);

/*
 * Build a write-multiple-registers (FC16) request into `out` (capacity `out_cap`). Writes `qty`
 * registers from `regs[0..qty-1]` to consecutive addresses starting at `addr`. Returns the total
 * frame length (9 + 2*qty), or 0 on bad args / insufficient capacity. Each register goes on the
 * wire big-endian (hi byte first); CRC low byte first.
 * NOTE: like FC06, this can COMMAND MOTION on a drive — gate with hardware-safety checks.
 */
size_t modbus_build_write_multi(uint8_t *out, size_t out_cap, uint8_t slave, uint16_t addr,
                                uint16_t qty, const uint16_t *regs);

/*
 * Parse a write-multiple-registers (FC16) response (echoes addr + qty, 8 bytes). Validates length,
 * CRC, slave, function, and that the echoed addr/qty match. Exception (0x90) -> *exception_out and
 * MODBUS_ERR_EXCEPTION; addr/qty echo mismatch -> MODBUS_ERR_BYTECOUNT.
 */
modbus_status_t modbus_parse_write_multi_response(const uint8_t *adu, size_t len,
                                                  uint8_t expect_slave, uint16_t expect_addr,
                                                  uint16_t expect_qty, uint8_t *exception_out);

/*
 * Word order for a 32-bit value spread across two consecutive Modbus registers. `regs[0]` is the
 * register at the lower wire address (read first), `regs[1]` the next.
 *   ABCD — big-endian word order: regs[0] is the high word. Modbus-standard / IEEE-754 byte order.
 *          The SELEC MFM384 reports this via its endianness register (40070 = 1).
 *   CDAB — word-swapped: regs[0] is the low word (common on some PLCs / energy meters).
 */
typedef enum {
    MODBUS_WORD_ORDER_ABCD = 0,
    MODBUS_WORD_ORDER_CDAB = 1,
} modbus_word_order_t;

/*
 * Reconstruct an IEEE-754 float32 from two registers (as returned by modbus_parse_read_response,
 * i.e. each already host-endian big-endian-decoded). No type-punning UB: assembles a uint32_t in
 * the selected word order then memcpy's into a float. Known-answer: {0x447A,0x0000} ABCD = 1000.0f.
 */
float modbus_regs_to_f32(const uint16_t *regs, modbus_word_order_t order);

#endif /* MODBUS_RTU_H */
