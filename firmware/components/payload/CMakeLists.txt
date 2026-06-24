# Payload encoder (Phase 6c, ADR-005). Pure C — also built host-side in tests/host.
idf_component_register(
    SRCS "payload.c"
    INCLUDE_DIRS "include"
)
