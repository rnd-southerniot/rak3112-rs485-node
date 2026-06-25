idf_component_register(
    SRCS "app_main.c" "lora.cpp" "provisioning.c" "ota.c"
    INCLUDE_DIRS "." "include"
    REQUIRES driver esp_system esp_timer esp_driver_gpio esp_driver_uart nvs_flash console
             app_update esp_partition jgromes__radiolib rs485 modbus meter payload
)
