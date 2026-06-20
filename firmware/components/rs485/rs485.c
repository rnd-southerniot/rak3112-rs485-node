#include "rs485.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "rs485";

esp_err_t rs485_init(const rs485_config_t *cfg)
{
    const uart_config_t uc = {
        .baud_rate = cfg->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(cfg->port, cfg->rx_buffer_size, 0, 0, NULL, 0), TAG,
                        "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(cfg->port, &uc), TAG, "uart_param_config");
    /* RTS pin carries DE/RE; no CTS. */
    ESP_RETURN_ON_ERROR(
        uart_set_pin(cfg->port, cfg->tx_gpio, cfg->rx_gpio, cfg->de_re_gpio, UART_PIN_NO_CHANGE),
        TAG, "uart_set_pin");
    /* Hardware drives RTS(=DE/RE) asserted through the last stop bit — no SW turnaround race. */
    ESP_RETURN_ON_ERROR(uart_set_mode(cfg->port, UART_MODE_RS485_HALF_DUPLEX), TAG,
                        "uart_set_mode");
    /* Q6 inverts DE/RE (ADR-001 EC-5a): invert RTS so GPIO LOW = TX, HIGH = RX. */
    ESP_RETURN_ON_ERROR(uart_set_line_inverse(cfg->port, UART_SIGNAL_RTS_INV), TAG,
                        "uart_set_line_inverse");

    ESP_LOGI(TAG, "RS-485 UART%d up: TX=%d RX=%d DE/RE=%d @ %d 8N1 (half-duplex, RTS inverted)",
             (int)cfg->port, cfg->tx_gpio, cfg->rx_gpio, cfg->de_re_gpio, cfg->baud_rate);
    return ESP_OK;
}

int rs485_write(uart_port_t port, const uint8_t *data, size_t len)
{
    return uart_write_bytes(port, data, len);
}

int rs485_read(uart_port_t port, uint8_t *out, size_t len, uint32_t timeout_ms)
{
    return uart_read_bytes(port, out, len, pdMS_TO_TICKS(timeout_ms));
}
