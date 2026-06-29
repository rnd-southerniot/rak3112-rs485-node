# Profile-driven runtime core (ADR-006). Pure C — also built host-side in tests/host.
# REQUIRES payload only for the shared telemetry.h (schema version + header length).
idf_component_register(
    SRCS "device_profile.c"
    INCLUDE_DIRS "include"
    REQUIRES payload
)
