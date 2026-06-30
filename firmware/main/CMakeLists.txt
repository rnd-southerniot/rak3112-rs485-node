idf_component_register(
    SRCS "app_main.c" "lora.cpp" "prov_console.cpp"
    INCLUDE_DIRS "." "include"
    REQUIRES driver esp_timer esp_driver_gpio esp_driver_uart nvs_flash console
             jgromes__radiolib rs485 modbus meter payload
)
