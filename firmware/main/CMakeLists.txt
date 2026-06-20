idf_component_register(
    SRCS "app_main.c" "lora.cpp"
    INCLUDE_DIRS "." "include"
    REQUIRES driver esp_timer esp_driver_gpio nvs_flash jgromes__radiolib
)
