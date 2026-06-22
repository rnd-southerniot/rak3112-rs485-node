# Modbus RTU master (Phase 6). modbus_rtu.c is pure (also built host-side in tests/host).
# The on-target transaction layer (modbus_master.c, Phase 6b) drives it over the rs485 component
# and is added with its REQUIRES then.
idf_component_register(
    SRCS "modbus_rtu.c"
    INCLUDE_DIRS "include"
)
