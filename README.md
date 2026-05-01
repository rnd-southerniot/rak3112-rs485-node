# rak3112-rs485-node

Industrial RS-485 ⇄ LoRaWAN AS923 sensor/gateway node.

- **Module:** RAK3112-9-SM-I (RAKwireless WisDuo, ESP32-S3 + Semtech SX1262)
- **Status:** Phase 1 scaffold (see [`CLAUDE.md`](./CLAUDE.md))
- **Owner:** rnd-southerniot

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
