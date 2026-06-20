# ADR-003: LoRaWAN stack = RadioLib (native ESP-IDF)

## Status
ACCEPTED-pending-sign-off · Phase 5 · retires OQ-3

## Context

Phase 5 needs a LoRaWAN stack to OTAA-join AS923 (ChirpStack `10.10.8.140`, Bangladesh) on the
RAK3112's **SX1262**, under **ESP-IDF v5.5.4** (no Arduino-IDE, no RUI3). The SX1262↔ESP32-S3
wiring is module-internal and **bench-confirmed in 5a** (see RUNBOOK Phase 5a): NSS=7, SCK=5,
MOSI=6, MISO=3, NRESET=8, BUSY=48, DIO1=47; RF-switch via SX1262 DIO2, TCXO via DIO3.

## Decision

**Use RadioLib (`jgromes/RadioLib`)** as a native ESP-IDF component for both the SX1262 PHY and
the LoRaWAN MAC.

- **Native ESP-IDF**, no Arduino shim: RadioLib ships an internal `EspHal` and an official
  `examples/NonArduino/ESP-IDF`; added via one line in `idf_component.yml` from the Espressif
  Component Registry (`jgromes/radiolib`). **Pin the version** (latest stable v7.7.1, 2026-05-31)
  per toolchain pin-discipline; bumps are a separate PR + ADR.
- Native **SX1262** driver + **AS923** band + **OTAA** Class A. LoRaWAN **RP002 1.0.4** (the safe
  AS923 default; matches ChirpStack v4).
- Radio init (from the confirmed pins): `Module(NSS=7, DIO1=47, NRESET=8, BUSY=48)`, SPI on
  SCK=5/MISO=3/MOSI=6, `setDio2AsRfSwitch(true)`, TCXO via `setTCXO(<V>)`.
- **C++ component with a thin C-callable wrapper** — RadioLib is C++; `app_main.c` stays C. The
  `lora` component compiles C++ and exposes a small C API (`lora_init`, `lora_join`, `lora_send`).

## Consequences

- One managed dependency; no bespoke SX1262 driver or MAC to maintain.
- C++ enters the firmware build (one component) behind a C boundary.
- The 5a SPI probe (GetStatus/register R/W) is superseded by RadioLib's driver — kept in RUNBOOK
  as the pin-confirmation record only.

## Open items (resolve before 5b TX)

1. **TCXO voltage — CONFIRM.** Sources disagree: Zephyr `rak3112` DTS = **1.8 V**; RAK's RadioLib
   example = **1.6 V**. Wrong TCXO voltage → no XOSC → join fails (not a wiring risk). Confirm the
   TCXO rail from the V1.1 schematic / RAK datasheet before the first TX.
2. **GPIO4 (ANT_SW) strategy.** Either let the SX1262 drive RF switching via DIO2 (RAK's approach,
   MCU does not toggle GPIO4) or drive GPIO4 from the MCU (Zephyr's `antenna-enable-gpios`).
   Confirm which the V1.1 board expects; app code must not repurpose GPIO4.
3. **AS923 dwell-time / sub-band** → ADR-004 (OQ-4), validated against the ChirpStack region config.
4. **Antenna mandatory before TX** — 50 Ω antenna or dummy load on the module (ADR-001 EC-7).

## Alternatives considered

- **MCCI LMIC** — rejected: Arduino-bound (no native idf.py), no LoRaWAN 1.1, SX126x is a late,
  narrowly-tested add-on to an SX127x-rooted radio layer.
- **LoRaMac-node (Semtech)** — rejected: cleanest spec impl but frozen (last release 2022-12),
  zero ESP-IDF port — would mean writing the whole HAL/BSP.
- **RAK SX126x-Arduino (`beegee-tokyo`)** — rejected: Arduino-only; the one documented ESP-IDF
  port (issue #122) failed (BUSY/OTAA); MAC stuck at 1.0.2.

## References
- RadioLib: https://components.espressif.com/components/jgromes/radiolib ·
  https://github.com/jgromes/RadioLib/tree/master/examples/NonArduino/ESP-IDF · AS923 band enum;
  AS923 dwell-time issue jgromes/RadioLib#1180.
- RAK3112 pins: RAK3112 datasheet; RAK forum (carlrowan); Zephyr `boards/rakwireless/rak3112`.
- Hardware: ADR-001 (SX1262 pin map corrected post-5a); firmware RUNBOOK Phase 5a.

## Sign-off
Signed-off-by: <pending> · 2026-06-20
