cmake_minimum_required(VERSION 3.20)
project(rak3112_rs485_node_host_tests C)

enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

# Pure-C firmware modules compiled natively on the host (no ESP-IDF).
set(FW_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../firmware)

# Phase 4: ring_buffer (RS-485 RX staging primitive).
add_executable(test_ring_buffer
    test_ring_buffer.c
    ${FW_ROOT}/components/ring_buffer/ring_buffer.c
)
target_include_directories(test_ring_buffer PRIVATE
    ${FW_ROOT}/components/ring_buffer/include
)
add_test(NAME ring_buffer COMMAND test_ring_buffer)

# Real tests land per phase: Phase 4 framing (ring_buffer, above), Phase 6 Modbus parser.
