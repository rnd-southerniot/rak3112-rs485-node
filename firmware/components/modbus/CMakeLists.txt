# Modbus RTU master (Phase 6).
#  - modbus_rtu.c   : pure framing (also built host-side in tests/host)
#  - modbus_master.c: on-target transaction + bus scan over the Phase 4 rs485 transport
idf_component_register(
    SRCS "modbus_rtu.c" "modbus_master.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart rs485
    PRIV_REQUIRES esp_timer esp_common
)
