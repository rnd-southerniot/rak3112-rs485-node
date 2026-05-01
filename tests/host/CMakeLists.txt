cmake_minimum_required(VERSION 3.20)
project(rak3112_rs485_node_host_tests C)

enable_testing()

# Real tests land in Phase 4 (RS-485 framing) and Phase 6 (Modbus parser).
# Phase 1 gates only on layout: this file's presence is the entry-criteria signal.
