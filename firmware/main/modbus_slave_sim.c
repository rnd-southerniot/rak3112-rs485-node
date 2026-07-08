/*
 * modbus_slave_sim.c — emulate a Honeywell EEM400-D-MO Modbus RTU slave (bench fixture).
 *
 * Serves FC03 (Read Holding Registers) from a fixed 52-register map (wire address 0..51 = datasheet
 * register R-1). Doubles (baudrate, serial, the WT energy counters) are big-endian u32 pairs (high
 * word first), exactly as the datasheet specifies. Per the EEM400 spec: only FC03 is implemented
 * (others -> ILLEGAL FUNCTION 0x01); a request out of range or for more than 20 registers ->
 * ILLEGAL DATA ADDRESS 0x02.
 *
 * RTU framing: read requests are a fixed 8 bytes; we accumulate bytes and treat an inter-byte gap
 * (read timeout) as a frame boundary, validating CRC before acting. The spec ONLY lives here — the
 * scanner under test never sees it, so discovery/inference is a genuine blind test.
 */
#include "modbus_slave_sim.h"

#include "sdkconfig.h"

#if CONFIG_APP_MODBUS_SLAVE_SIM

#include <stdint.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gpio_remap.h"
#include "modbus_rtu.h" /* modbus_crc16, MODBUS_FC_READ_HOLDING_REGISTERS */
#include "rs485.h"

static const char *TAG = "slavesim";

#define SIM_UNIT ((uint8_t)CONFIG_APP_SLAVE_SIM_UNIT)
#define SIM_BAUD (CONFIG_APP_SLAVE_SIM_BAUD)
#define REG_COUNT 52u
#define MAX_READ 20u /* EEM400: at most 20 registers per read */
#define REQ_LEN 8u   /* a read-request ADU is always 8 bytes */

#define EXC_ILLEGAL_FUNCTION 0x01u
#define EXC_ILLEGAL_ADDRESS 0x02u

/* Honeywell EEM400-D-MO holding registers (wire 0..51 = datasheet R-1). Values are plausible
 * bench readings; doubles are big-endian (high word first). */
static uint16_t s_regs[REG_COUNT] = {
    /* 0  R1  firmware version 1.1     */ 0x000B,
    /* 1  R2  # registers = 52         */ 0x0034,
    /* 2  R3  # flags = 0              */ 0x0000,
    /* 3  R4  baudrate high (=1)       */ 0x0001,
    /* 4  R5  baudrate low (=49664)    */ 0xC200, /* 115200 bps */
    /* 5  R6  not used                 */ 0x0000,
    /* 6  R7  type "EE"                */ 0x4545,
    /* 7  R8  type "M4"                */ 0x4D34,
    /* 8  R9  type "00"                */ 0x3030,
    /* 9  R10 type "-"                 */ 0x2D2D,
    /* 10 R11 type "D-"                */ 0x442D,
    /* 11 R12 type "MO"                */ 0x4D4F,
    /* 12 R13 type " " (non-MID)       */ 0x2020,
    /* 13 R14 type " " (non-MID)       */ 0x2020,
    /* 14 R15 HW version 1.1           */ 0x000B,
    /* 15 R16 serial low, high word    */ 0x0001,
    /* 16 R17 serial low, low word     */ 0x2345,
    /* 17 R18 serial high              */ 0x0067,
    /* 18 R19 not used                 */ 0x0000,
    /* 19 R20 not used                 */ 0x0000,
    /* 20 R21 not used                 */ 0x0000,
    /* 21 R22 status/protect           */ 0x0000,
    /* 22 R23 modbus timeout (ms)      */ 0x03E8,
    /* 23 R24 modbus address           */ 0x0000, /* set to SIM_UNIT at init */
    /* 24 R25 error register           */ 0x0000,
    /* 25 R26 not used                 */ 0x0000,
    /* 26 R27 tariff register          */ 0x0000,
    /* 27 R28 WT1 total high           */ 0x000D,
    /* 28 R29 WT1 total low            */ 0xEBDF, /* 912351 -> 9123.51 kWh */
    /* 29 R30 WT1 partial high         */ 0x000D,
    /* 30 R31 WT1 partial low          */ 0xEBDF,
    /* 31 R32 WT2 total high           */ 0x0007,
    /* 32 R33 WT2 total low            */ 0xA120, /* 500000 -> 5000.00 kWh */
    /* 33 R34 WT2 partial high         */ 0x0001,
    /* 34 R35 WT2 partial low          */ 0x86A0, /* 100000 -> 1000.00 kWh */
    /* 35 R36 URMS phase 1 (V)         */ 0x00E6, /* 230 V */
    /* 36 R37 IRMS phase 1 (0.1 A)     */ 0x0034, /* 52 -> 5.2 A */
    /* 37 R38 PRMS phase 1 (0.01 kW)   */ 0x0078, /* 120 -> 1.20 kW */
    /* 38 R39 QRMS phase 1 (0.01 kvar) */ 0x0032, /* 50 -> 0.50 kvar */
    /* 39 R40 cos phi phase 1 (0.01)   */ 0x005F, /* 95 -> 0.95 */
    /* 40 R41 URMS phase 2 (V)         */ 0x00E7, /* 231 V */
    /* 41 R42 IRMS phase 2 (0.1 A)     */ 0x0032, /* 5.0 A */
    /* 42 R43 PRMS phase 2 (0.01 kW)   */ 0x006E, /* 1.10 kW */
    /* 43 R44 QRMS phase 2 (0.01 kvar) */ 0x0032,
    /* 44 R45 cos phi phase 2 (0.01)   */ 0x0060, /* 0.96 */
    /* 45 R46 URMS phase 3 (V)         */ 0x00E5, /* 229 V */
    /* 46 R47 IRMS phase 3 (0.1 A)     */ 0x0030, /* 4.8 A */
    /* 47 R48 PRMS phase 3 (0.01 kW)   */ 0x0064, /* 1.00 kW */
    /* 48 R49 QRMS phase 3 (0.01 kvar) */ 0x0032,
    /* 49 R50 cos phi phase 3 (0.01)   */ 0x005E, /* 0.94 */
    /* 50 R51 PRMS total (0.01 kW)     */ 0x014A, /* 330 -> 3.30 kW */
    /* 51 R52 QRMS total (0.01 kvar)   */ 0x0096, /* 150 -> 1.50 kvar */
};

/* Append CRC-16/MODBUS and transmit a response ADU. */
static void reply(uint8_t *buf, size_t body_len)
{
    const uint16_t crc = modbus_crc16(buf, body_len);
    buf[body_len] = (uint8_t)(crc & 0xFF);
    buf[body_len + 1] = (uint8_t)((crc >> 8) & 0xFF);
    rs485_write(UART_NUM_1, buf, body_len + 2);
}

static void reply_exception(uint8_t unit, uint8_t fc, uint8_t code)
{
    uint8_t out[5];
    out[0] = unit;
    out[1] = (uint8_t)(fc | 0x80);
    out[2] = code;
    reply(out, 3);
    ESP_LOGW(TAG, "exception fc=0x%02X code=0x%02X", (unsigned)fc, (unsigned)code);
}

static void handle_request(uint8_t unit, uint8_t fc, uint16_t addr, uint16_t qty)
{
    if (fc != MODBUS_FC_READ_HOLDING_REGISTERS) {
        reply_exception(unit, fc, EXC_ILLEGAL_FUNCTION);
        return;
    }
    if (qty < 1 || qty > MAX_READ || (uint32_t)addr + qty > REG_COUNT) {
        reply_exception(unit, fc, EXC_ILLEGAL_ADDRESS);
        return;
    }
    uint8_t out[3 + 2 * MAX_READ + 2];
    out[0] = unit;
    out[1] = fc;
    out[2] = (uint8_t)(qty * 2);
    for (uint16_t i = 0; i < qty; ++i) {
        const uint16_t v = s_regs[addr + i];
        out[3 + i * 2] = (uint8_t)(v >> 8);
        out[4 + i * 2] = (uint8_t)(v & 0xFF);
    }
    reply(out, (size_t)(3 + qty * 2));
    ESP_LOGI(TAG, "FC03 unit=%u addr=%u qty=%u -> %u bytes", (unsigned)unit, (unsigned)addr,
             (unsigned)qty, (unsigned)(qty * 2));
}

void run_modbus_slave_sim(void)
{
    s_regs[23] = SIM_UNIT; /* register R24 (modbus address) reflects our unit */

    const rs485_config_t cfg = {
        .port = UART_NUM_1,
        .tx_gpio = PIN_RS485_TX,
        .rx_gpio = PIN_RS485_RX,
        .de_re_gpio = PIN_RS485_DE_RE,
        .baud_rate = SIM_BAUD,
        .parity = UART_PARITY_DISABLE, /* 8N1 (matches the scanner's 1-stop transport) */
        .rx_buffer_size = 512,
    };
    ESP_ERROR_CHECK(rs485_init(&cfg));
    ESP_LOGW(TAG, "EEM400 slave sim: unit=%u baud=%d 8N1, FC03, %u regs (wire 0..%u)",
             (unsigned)SIM_UNIT, SIM_BAUD, (unsigned)REG_COUNT, (unsigned)(REG_COUNT - 1));

    uint8_t frame[REQ_LEN];
    size_t have = 0;
    for (;;) {
        uint8_t b;
        const int n = rs485_read(UART_NUM_1, &b, 1, 50); /* 50 ms inter-byte gap = frame boundary */
        if (n <= 0) {
            have = 0; /* silence: resync to the next frame */
            continue;
        }
        if (have < REQ_LEN) {
            frame[have++] = b;
        } else {
            memmove(frame, frame + 1, REQ_LEN - 1);
            frame[REQ_LEN - 1] = b; /* slide the window while hunting for CRC alignment */
        }
        if (have < REQ_LEN) {
            continue;
        }
        const uint16_t crc = modbus_crc16(frame, 6);
        if ((uint16_t)(frame[6] | (frame[7] << 8)) != crc) {
            continue; /* not a valid frame yet — keep sliding */
        }
        const uint8_t unit = frame[0];
        const uint8_t fc = frame[1];
        const uint16_t addr = (uint16_t)((frame[2] << 8) | frame[3]);
        const uint16_t qty = (uint16_t)((frame[4] << 8) | frame[5]);
        have = 0; /* frame consumed */
        if (unit != SIM_UNIT) {
            continue; /* addressed to another slave (broadcast reads are not used) */
        }
        handle_request(unit, fc, addr, qty);
    }
}

#endif /* CONFIG_APP_MODBUS_SLAVE_SIM */
