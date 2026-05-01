#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    for (uint32_t i = 0;; ++i) {
        printf("rak3112-rs485-node alive: tick=%lu\n", (unsigned long)i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
