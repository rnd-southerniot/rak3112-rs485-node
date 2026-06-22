# PIN_MAP.md — RAK3112 RS-485 Node (V1.1)

> **Mirror of** `firmware/main/include/gpio_remap.h` (the authoritative source).
> **Hardware authority:** ADR-001-pin-map in `rnd-southerniot/rak3112-rs485-node-hw`
> (tag `adr-001-locked`, commit `a0b002c`).
> Any edit here must match a `gpio_remap.h` edit in the same commit (firmware CLAUDE.md §11).

## ESP32-S3 GPIO assignments

| GPIO | Schematic net | Macro | Function | First driven | Notes |
|---:|---|---|---|---|---|
| 9 | `GPIO9` | `PIN_I2C1_SDA_RESERVED` | V1.2 I²C SDA (reserved) | Phase 3 (held floating) | Routed-but-unterminated on V1.1; held high-Z (no pull, no drive) per ADR-001 EC-4/EC-9 |
| 40 | `GPIO40` | `PIN_I2C1_SCL_RESERVED` | V1.2 I²C SCL (reserved) | Phase 3 (held floating) | Same as GPIO9; becomes I²C init on V1.2 (#1) |
| 38 | `GPIO38` | `PIN_WS2812_DIN` | WS2812 RGB DIN (U11) | Phase 3 (optional) | Addressable RGB status indicator |
| 43 | `GPIO43_TX` | `PIN_RS485_TX` | RS-485 UART TX → U9 D (via R34) | Phase 4 | Console is USB-CDC, so UART is free for field bus |
| 44 | `GPIO44_RX` | `PIN_RS485_RX` | RS-485 UART RX ← U9 R | Phase 4 | — |
| 21 | `GPIO21` | `PIN_RS485_DE_RE` | RS-485 DE/RE | Phase 4 | **STANDARD** polarity (measured 2026-06-20): HIGH = TRANSMIT, LOW (idle) = RECEIVE. ADR-001 EC-5a's "inverted" was wrong — see ADR-002 rev2 |
| 0 | `BOOT` | `PIN_BOOT_SW2` | Boot-mode strap / SW2 | — (never driven) | HIGH at reset = normal SPI boot |

## Strapping / module-internal — DO NOT DRIVE (project CLAUDE.md §3 #2)

| GPIO | Function | State on V1.1 |
|---:|---|---|
| 0 | Boot mode select | Externally pulled HIGH; SW2 pulls LOW for download |
| 3 | JTAG signal source | Module-internal |
| 45 | VDD_SPI voltage select (1.8/3.3 V) | Module-internal — wrong level bricks flash/PSRAM access |
| 46 | ROM print at boot | Module-internal / NC |

## Console & power

| Item | Detail |
|---|---|
| Console | Native USB-CDC (USB_D− pin 2 / USB_D+ pin 3 → USB-C `USB1`), 115200 |
| Logic power | USB-C 5 V → RT6160 buck-boost → 3V3 |
| **H1 jumper** | **Must be INSTALLED** (series 3V3 enable). Removed = "device not detected" |
| SX1262 (module-internal, **bench-confirmed 2026-06-20**) | NSS=GPIO7, SCK=GPIO5, MOSI=GPIO6, MISO=GPIO3, NRESET=GPIO8, BUSY=GPIO48, DIO1=GPIO47, ANT_SW=GPIO4; RF-switch via SX1262 DIO2, TCXO via DIO3 (SPI-internal). **NOT** the edge GPIO10–13/SPI_* pins — those are a separate user SPI (ADR-001 mislabel, corrected). |
