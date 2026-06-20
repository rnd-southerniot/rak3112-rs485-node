/*
 * rs485.h — half-duplex RS-485 transport over an ESP32-S3 UART (TP8485E / U9).
 *
 * Per ADR-002 rev2: UART RS-485 half-duplex mode drives the DE/RE line in hardware
 * (deterministic turnaround) with STANDARD (non-inverted) polarity — empirically confirmed
 * on the project board (Phase 4, 2026-06-20): GPIO21 HIGH = transmit, LOW (idle) = receive.
 * (ADR-001 EC-5a's "inverted" reading was wrong.) DE and RE are tied, so the node never
 * receives its own transmission (no TX echo on RX).
 */
#ifndef RS485_H
#define RS485_H

#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

typedef struct {
    uart_port_t port;      /* e.g. UART_NUM_1 */
    int tx_gpio;           /* PIN_RS485_TX  (GPIO43) */
    int rx_gpio;           /* PIN_RS485_RX  (GPIO44) */
    int de_re_gpio;        /* PIN_RS485_DE_RE (GPIO21) — driven as inverted RTS */
    int baud_rate;         /* 8N1 */
    size_t rx_buffer_size; /* driver RX ring (>= 2*UART_HW_FIFO_LEN) */
} rs485_config_t;

/* Install + configure the UART for inverted-RTS half-duplex RS-485. */
esp_err_t rs485_init(const rs485_config_t *cfg);

/* Blocking write of `len` bytes; HW asserts DE around the frame. Returns bytes sent or -1. */
int rs485_write(uart_port_t port, const uint8_t *data, size_t len);

/* Read up to `len` bytes, waiting up to `timeout_ms`. Returns bytes read (0 on timeout) or -1. */
int rs485_read(uart_port_t port, uint8_t *out, size_t len, uint32_t timeout_ms);

#endif /* RS485_H */
