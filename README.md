# rak3112-rs485-node

Industrial RS-485 ⇄ LoRaWAN AS923 sensor/gateway node.

- **Module:** RAK3112-9-SM-I (RAKwireless WisDuo, ESP32-S3 + Semtech SX1262)
- **Status:** Phase 1 scaffold rev4 (see [`CLAUDE.md`](./CLAUDE.md))
- **Build path:** ESP-IDF v5.5.4 via `idf.py` (single canonical path; PlatformIO dropped at Phase 1 Attempt 2 — see CLAUDE.md §1 footnote ²)
- **Owner:** rnd-southerniot

**Topology (P7):** the LoRaWAN radio stack is the shared component [`siot-lorawan-node`](https://github.com/rnd-southerniot/siot-lorawan-node) (git dep in `firmware/main/idf_component.yml`); the CRM build/flash/provision service lives in the hub [`siot-node-firmware-automation`](https://github.com/rnd-southerniot/siot-node-firmware-automation) (this repo's `api/`+`tools/` are the deployed source until the hub cutover). Sibling product: `senseflow-eink-node`.

## Quick start

```bash
. ~/esp/esp-idf-v5.5.4/export.sh
cd firmware
idf.py set-target esp32s3
idf.py build
```

See [`CLAUDE.md`](./CLAUDE.md) for the full execution contract, phase plan, guardrails, and open questions. Hardware revision pinning lives in [`HARDWARE_REV.md`](./HARDWARE_REV.md).

## Hardware

Read-only schematic snapshot in [`hardware/schematic/v1.0/`](./hardware/schematic/v1.0/). Canonical hardware repo: `rnd-southerniot/rak3112-rs485-node-hw` (planned — not yet created).
