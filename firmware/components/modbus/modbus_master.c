/*
 * modbus_master.c — on-target Modbus RTU master + bus scan. See modbus_master.h.
 */
#include "modbus_master.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rs485.h"

static const char *TAG = "modbus";

/* Largest response we read while probing/reading: header(3) + 2*qty data + CRC(2). */
#define MB_RESP_MAX 256

modbus_status_t modbus_master_read(uart_port_t port, uint8_t unit_id, uint8_t func, uint16_t addr,
                                   uint16_t qty, uint32_t timeout_ms, uint16_t *regs, uint8_t *exc)
{
    uint8_t req[MODBUS_READ_REQ_LEN];
    const size_t req_len = modbus_build_read(req, unit_id, func, addr, qty);
    if (req_len == 0 || regs == NULL) {
        return MODBUS_ERR_ARG;
    }

    /* Expected normal response length; an exception reply is 5 bytes (handled by the parser). */
    const size_t want = (size_t)5u + 2u * (size_t)qty;

    uart_flush_input(port); /* drop any stale bytes before the transaction */
    (void)rs485_write(port, req, req_len);
    uart_wait_tx_done(port, pdMS_TO_TICKS(100)); /* ensure frame is out + line turned to RX */

    uint8_t resp[MB_RESP_MAX];
    const int got = rs485_read(port, resp, want, timeout_ms);
    if (got <= 0) {
        return MODBUS_ERR_SHORT; /* timeout / nothing */
    }
    return modbus_parse_read_response(resp, (size_t)got, unit_id, func, qty, regs, exc);
}

mb_probe_t modbus_master_probe(uart_port_t port, uint8_t unit_id, uint16_t addr,
                               uint32_t timeout_ms, mb_probe_info_t *info)
{
    uint8_t req[MODBUS_READ_REQ_LEN];
    (void)modbus_build_read(req, unit_id, MODBUS_FC_READ_HOLDING_REGISTERS, addr, 1);

    uart_flush_input(port);
    const int64_t t0 = esp_timer_get_time();
    (void)rs485_write(port, req, sizeof(req));
    uart_wait_tx_done(port, pdMS_TO_TICKS(100));

    uint8_t resp[16];
    const int got = rs485_read(port, resp, 7, timeout_ms); /* 1-reg reply = 7 bytes; exc = 5 */
    const uint32_t lat = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    mb_probe_t result = MB_PROBE_ABSENT;
    uint16_t reg0 = 0;
    uint8_t exc = 0;
    if (got > 0) {
        uint16_t reg = 0;
        uint8_t e = 0;
        const modbus_status_t st = modbus_parse_read_response(
            resp, (size_t)got, unit_id, MODBUS_FC_READ_HOLDING_REGISTERS, 1, &reg, &e);
        if (st == MODBUS_OK) {
            result = MB_PROBE_PRESENT_DATA;
            reg0 = reg;
        } else if (st == MODBUS_ERR_EXCEPTION) {
            result = MB_PROBE_PRESENT_EXCEPTION;
            exc = e;
        } else {
            result = MB_PROBE_BADFRAME;
        }
    }

    if (info != NULL) {
        info->result = result;
        info->reg0 = reg0;
        info->exception = exc;
        info->rx_len = got;
        info->latency_ms = lat;
    }
    return result;
}

int modbus_master_scan(uart_port_t port, uint8_t id_lo, uint8_t id_hi, uint16_t probe_addr,
                       uint32_t timeout_ms, int baud_label, const char *framing)
{
    ESP_LOGI(TAG, "scan @ %d %s | ids %u-%u | FC03 @0x%04X | timeout %ums", baud_label, framing,
             (unsigned)id_lo, (unsigned)id_hi, (unsigned)probe_addr, (unsigned)timeout_ms);
    int found = 0;
    int garbage = 0;
    for (int id = id_lo; id <= (int)id_hi; ++id) {
        mb_probe_info_t info = {0};
        const mb_probe_t r = modbus_master_probe(port, (uint8_t)id, probe_addr, timeout_ms, &info);
        switch (r) {
        case MB_PROBE_PRESENT_DATA:
            ESP_LOGI(TAG, "  >> id %d PRESENT @ %d %s: reg[0x%04X]=0x%04X (%ums)", id, baud_label,
                     framing, (unsigned)probe_addr, (unsigned)info.reg0, (unsigned)info.latency_ms);
            ++found;
            break;
        case MB_PROBE_PRESENT_EXCEPTION:
            ESP_LOGI(TAG, "  >> id %d PRESENT @ %d %s: exception 0x%02X — device answered (%ums)",
                     id, baud_label, framing, (unsigned)info.exception, (unsigned)info.latency_ms);
            ++found;
            break;
        case MB_PROBE_BADFRAME:
            ESP_LOGW(TAG, "  ?? id %d garbage %d byte(s) @ %d %s — framing/parity/baud mismatch",
                     id, info.rx_len, baud_label, framing);
            ++garbage;
            break;
        case MB_PROBE_ABSENT:
        default:
            break; /* quiet on absent */
        }
        vTaskDelay(pdMS_TO_TICKS(20)); /* brief inter-probe gap */
    }
    ESP_LOGI(TAG, "scan done @ %d %s: %d device(s), %d garbage", baud_label, framing, found,
             garbage);
    return found;
}
