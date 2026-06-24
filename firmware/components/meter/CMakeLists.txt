# Field-device sampling + simulation (Phase 6c). Real reads use modbus over the rs485 transport.
idf_component_register(
    SRCS "meter.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart payload modbus rs485
)
