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

# Phase 6: modbus_rtu (RTU master framing — CRC16, request build, response/exception parse).
add_executable(test_modbus_rtu
    test_modbus_rtu.c
    ${FW_ROOT}/components/modbus/modbus_rtu.c
)
target_include_directories(test_modbus_rtu PRIVATE
    ${FW_ROOT}/components/modbus/include
)
add_test(NAME modbus_rtu COMMAND test_modbus_rtu)
