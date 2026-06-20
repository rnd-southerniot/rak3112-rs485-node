#!/usr/bin/env bash
# scripts/flash.sh — canonical build + flash + monitor wrapper for rak3112-rs485-node.
#
# Console/flash transport is native USB-CDC (ESP32-S3). On macOS the board enumerates
# as /dev/tty.usbmodem<serial>. Override with PORT=... if auto-detect picks the wrong one.
#
# Usage:
#   scripts/flash.sh                 # auto-detect port, build + flash + monitor
#   PORT=/dev/tty.usbmodem1101 scripts/flash.sh
#   scripts/flash.sh build           # build only (no hardware needed)
#   scripts/flash.sh chip-id         # confirm target is ESP32-S3 (pre-flash safety check)
#
# Hardware-safety pre-flight (project CLAUDE.md §3, docs/RUNBOOK.md "Phase 3 bench checklist"):
#   - H1 jumper INSTALLED, USB-C connected, do not also power DC1 + bench supply together.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW_DIR="$REPO_ROOT/firmware"
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf-v5.5.4/export.sh}"

test -f "$IDF_EXPORT" || { echo "ERROR: ESP-IDF export not found: $IDF_EXPORT" >&2; exit 2; }
# shellcheck disable=SC1090
. "$IDF_EXPORT"

detect_port() {
    if [[ -n "${PORT:-}" ]]; then echo "$PORT"; return; fi
    local p
    p=$(ls /dev/tty.usbmodem* 2>/dev/null | head -1 || true)
    test -n "$p" || { echo "ERROR: no /dev/tty.usbmodem* found; set PORT=..." >&2; exit 2; }
    echo "$p"
}

cmd="${1:-flashmon}"
case "$cmd" in
    build)
        idf.py -C "$FW_DIR" build ;;
    chip-id)
        esptool.py --port "$(detect_port)" chip_id ;;
    flash)
        idf.py -C "$FW_DIR" -p "$(detect_port)" flash ;;
    monitor)
        idf.py -C "$FW_DIR" -p "$(detect_port)" monitor ;;
    flashmon)
        idf.py -C "$FW_DIR" -p "$(detect_port)" flash monitor ;;
    *)
        echo "Usage: scripts/flash.sh [build|chip-id|flash|monitor|flashmon]" >&2
        exit 2 ;;
esac
