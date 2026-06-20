/*
 * EspHalS3.h — RadioLib HAL for ESP32-S3 under native ESP-IDF (Phase 5, ADR-003).
 *
 * RadioLib 7.7.1 ships only an ESP32-classic example HAL (examples/NonArduino/ESP-IDF/EspHal.h)
 * that #errors on S2/S3 and bit-bangs the classic SPI registers. This HAL reimplements the
 * RadioLibHal interface on top of the portable ESP-IDF `spi_master` + `gpio` drivers, so it
 * works on the ESP32-S3. Constructor arg order matches the upstream EspHal(sck, miso, mosi).
 */
#ifndef ESP_HAL_S3_H
#define ESP_HAL_S3_H

#include <RadioLib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#define LOW 0x0
#define HIGH 0x1
#define INPUT 0x01
#define OUTPUT 0x03
#define RISING 0x01
#define FALLING 0x02

#define ESP_HAL_S3_SPI_HOST SPI2_HOST
#define ESP_HAL_S3_SPI_HZ (2 * 1000 * 1000)

class EspHalS3 : public RadioLibHal
{
  public:
    EspHalS3(int8_t sck, int8_t miso, int8_t mosi)
        : RadioLibHal(INPUT, OUTPUT, LOW, HIGH, RISING, FALLING), spiSCK(sck), spiMISO(miso),
          spiMOSI(mosi)
    {
    }

    void init() override
    {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = spiMOSI;
        buscfg.miso_io_num = spiMISO;
        buscfg.sclk_io_num = spiSCK;
        buscfg.quadwp_io_num = -1;
        buscfg.quadhd_io_num = -1;
        buscfg.max_transfer_sz = 0;
        spi_bus_initialize(ESP_HAL_S3_SPI_HOST, &buscfg, SPI_DMA_DISABLED);

        spi_device_interface_config_t devcfg = {};
        devcfg.clock_speed_hz = ESP_HAL_S3_SPI_HZ;
        devcfg.mode = 0;          /* SX126x = SPI mode 0 */
        devcfg.spics_io_num = -1; /* RadioLib drives NSS via digitalWrite */
        devcfg.queue_size = 1;
        spi_bus_add_device(ESP_HAL_S3_SPI_HOST, &devcfg, &spiDev);
    }

    void term() override
    {
        if (spiDev) {
            spi_bus_remove_device(spiDev);
            spiDev = nullptr;
        }
        spi_bus_free(ESP_HAL_S3_SPI_HOST);
    }

    void pinMode(uint32_t pin, uint32_t mode) override
    {
        if (pin == RADIOLIB_NC)
            return;
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = (1ULL << pin);
        cfg.mode = (mode == OUTPUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&cfg);
    }

    void digitalWrite(uint32_t pin, uint32_t value) override
    {
        if (pin == RADIOLIB_NC)
            return;
        gpio_set_level((gpio_num_t)pin, value);
    }

    uint32_t digitalRead(uint32_t pin) override
    {
        if (pin == RADIOLIB_NC)
            return 0;
        return gpio_get_level((gpio_num_t)pin);
    }

    void attachInterrupt(uint32_t irq, void (*cb)(void), uint32_t mode) override
    {
        if (irq == RADIOLIB_NC)
            return;
        static bool isrInstalled = false;
        if (!isrInstalled) {
            gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
            isrInstalled = true;
        }
        gpio_set_intr_type((gpio_num_t)irq, (mode == RISING)    ? GPIO_INTR_POSEDGE
                                            : (mode == FALLING) ? GPIO_INTR_NEGEDGE
                                                                : GPIO_INTR_ANYEDGE);
        gpio_isr_handler_add((gpio_num_t)irq, (gpio_isr_t)(void *)cb, nullptr);
    }

    void detachInterrupt(uint32_t irq) override
    {
        if (irq == RADIOLIB_NC)
            return;
        gpio_isr_handler_remove((gpio_num_t)irq);
        gpio_set_intr_type((gpio_num_t)irq, GPIO_INTR_DISABLE);
    }

    void delay(unsigned long ms) override
    {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    void delayMicroseconds(unsigned long us) override
    {
        esp_rom_delay_us(us);
    }
    unsigned long millis() override
    {
        return (unsigned long)(esp_timer_get_time() / 1000ULL);
    }
    unsigned long micros() override
    {
        return (unsigned long)esp_timer_get_time();
    }

    long pulseIn(uint32_t pin, uint32_t state, unsigned long timeout) override
    {
        if (pin == RADIOLIB_NC)
            return 0;
        unsigned long start = micros(), t = micros();
        while (digitalRead(pin) == state) {
            if ((micros() - t) > timeout)
                return 0;
        }
        return micros() - start;
    }

    void spiBegin()
    {
    }
    void spiBeginTransaction()
    {
    }
    void spiEndTransaction()
    {
    }
    void spiEnd()
    {
    }

    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override
    {
        spi_transaction_t t = {};
        t.length = len * 8; /* bits */
        t.tx_buffer = out;
        t.rx_buffer = in;
        spi_device_polling_transmit(spiDev, &t);
    }

  private:
    int8_t spiSCK, spiMISO, spiMOSI;
    spi_device_handle_t spiDev = nullptr;
};

#endif /* ESP_HAL_S3_H */
