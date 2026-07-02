# Build + flash rak3112 with NO local ESP-IDF (Docker IDF), and: sim-mfm384 is OTAA-join-gated

**Category:** gotcha
**Tags:** esp-idf, docker, esptool, flash, sim-mfm384, lorawan, join, bench, macos
**Date:** 2026-06-30

## Context
On the dev Mac used for SCOMM-CRM work, ESP-IDF is **not installed locally** (`~/esp/esp-idf-v5.5.4/`
absent, `idf.py` not on PATH) — so `scripts/flash.sh` aborts (`ERROR: ESP-IDF export not found`).
Docker **is** available. Two things cost time if forgotten: (1) how to build/flash without a local
toolchain, and (2) why a `sim-mfm384` board shows **no** simulated telemetry on the serial monitor.

## Detail

### Build via the canonical Docker IDF image (no local toolchain)
Same image the repo `Dockerfile` uses. Image is ~4.4 GB (`docker pull espressif/idf:v5.5.4` first).
```bash
docker run --rm -v "$PWD/firmware":/work/firmware -w /work/firmware espressif/idf:v5.5.4 \
  bash -c 'idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.sim-mfm384" build'
```
Outputs `firmware/build/{bootloader/bootloader.bin, partition_table/partition-table.bin,
rak3112_rs485_node.bin, flash_args}`. No `lora_credentials.h` needed — `lora.cpp` `__has_include`
falls back to the committed `.example` placeholder (all-zero creds; emits a `#pragma message` warning).
Container output files are root-owned but host-readable.

### Flash host-side (Docker on macOS CANNOT pass through USB serial)
Board enumerates at `/dev/cu.usbmodem1101`. **Confirm board ID first** (CLAUDE.md §3 #1):
```bash
esptool -p /dev/cu.usbmodem1101 chip_id   # expect ESP32-S3, MAC 3c:dc:75:6f:85:dc (project board, ESP32-S3R8)
```
Flash with explicit offsets, **no erase_flash / eraseAll:false** (§3 gate):
```bash
cd firmware && esptool --chip esp32s3 -p /dev/cu.usbmodem1101 -b 460800 --before default_reset --after hard_reset \
  write_flash 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/rak3112_rs485_node.bin
```
Host `esptool` is v5.2.0 at `/opt/homebrew/bin` (`esptool`, not deprecated `esptool.py`).

### Monitor without pyserial
No `pyserial`/`idf.py monitor` here. Use `screen /dev/cu.usbmodem1101 115200`, or a stdlib `termios`
raw reader (open `O_RDONLY|O_NONBLOCK`, set `B115200`, `select()` loop). Console is USB-Serial-JTAG.

### GOTCHA — sim-mfm384 telemetry is OTAA-join-gated (silent without creds)
`run_field_app()` — the **only** caller of `meter_sim_mfm384()` — is reached **only after a successful
join** (its comment: "Assumes lora is already joined"). `app_main.c:255-267` tries OTAA 5× then
`for(;;)` **halts** with `OTAA join failed after retries — halting`. So with placeholder all-zero creds
(no real `lora_credentials.h`, no gateway/registration) the serial shows: PSRAM OK → `SX1262 up (AS923)`
→ `join attempt 1..5/5` all `join failed (-1116)` → halt — and **ZERO simulated samples**. The
`MFM384 V=… -> N B` log line never prints.

To actually observe sim telemetry on hardware: real creds (`firmware/.env` / `lora_credentials.h`) +
device registered in ChirpStack + a gateway in RF range (the Phase-6-proven path). To see sim values
over serial WITHOUT LoRaWAN infra requires a firmware change (sample/log before/independent of join) —
a deliberate edit to the deployed `main`, not a config flag.

## Usage
Bench build/flash/monitor when no local IDF. Remember the join-gate before chasing "why no sim data".
The sim **payload/decode** leg needs no hardware at all — see `devops/demo-provision-simulated-mfm384.md`.

## Related
- `firmware/main/app_main.c` (run_field_app + join loop), `firmware/sdkconfig.defaults.sim-mfm384`
- `firmware/main/lora.cpp` (`__has_include` creds fallback), `scripts/flash.sh` (needs local IDF)
- `.planning/knowledge/devops/demo-provision-simulated-mfm384.md` (decode/provision legs)
- CLAUDE.md §3 (flash safety, board IDs), §6 Phase 6 state (sim-mfm384 E2E proven)
