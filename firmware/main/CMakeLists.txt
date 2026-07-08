idf_component_register(
    SRCS "app_main.c" "provisioning.c" "prov_console.cpp" "profile_store.c" "status_led.c"
         "modbus_slave_sim.c"
    INCLUDE_DIRS "." "include"
    REQUIRES driver esp_system esp_timer esp_driver_gpio esp_driver_uart nvs_flash console
             app_update esp_partition siot-lorawan-node rs485 modbus meter payload device_profile
             espressif__led_strip
)
