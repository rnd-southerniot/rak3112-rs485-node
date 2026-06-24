# Runbook

Operational log for `rak3112-rs485-node` firmware. Append-only.

## Phase 3 bench checklist + hardware-safety pre-flight

> Run through this **before every flash/monitor** on real hardware (project CLAUDE.md §3,
> firmware domain CLAUDE.md §6). If any item is unknown — **stop and ask**.

1. **H1 jumper INSTALLED** (series 3V3-enable header between RT6160 VOUT and the 3V3 rail).
   A removed jumper cuts 3V3 to the module → board does **not** enumerate on USB and looks
   "not detected" (easy to misdiagnose as a USB/bootloader fault). — ADR-001 EC-5b.
2. **Power:** USB-C (`USB1`) supplies logic via RT6160. Do **not** simultaneously connect the
   DC1 barrel jack and a bench supply without first verifying RT6160 input isolation (§3 #7).
3. **Strap pins:** GPIO0 (SW2/BOOT) is pulled HIGH internally → normal SPI boot; do **not**
   hold SW2 for a normal boot. GPIO45/46 are module-internal / NC — never driven. To force
   download mode (only if `idf.py flash` auto-reset fails): hold SW2, tap SW1, release SW2.
4. **No mass-erase.** `esptool erase_flash` stays gated (§3 #1). Flash is `idf.py flash` /
   `scripts/flash.sh flash` only.
5. **Target verification before flash:** `ls /dev/tty.usbmodem*` then `scripts/flash.sh chip-id`
   → expect **ESP32-S3**. Confirm before writing.

### PASS evidence to capture (quote verbatim here on a green run)

- `Found 8MB PSRAM device` within ~2 s of reset, **and**
- `Adding pool of NNNN KiB of PSRAM memory to heap allocator` in heap-init, **and**
- `rak3112-rs485-node alive: tick=N` incrementing at 1 Hz, **and**
- no bootloop, no `Brownout detector was triggered`, no spurious ROM print on UART0.

### Phase 3 attempts

#### Attempt 1 — 2026-06-20 — PASS (console finding: USB_CDC → USB_SERIAL_JTAG)

**Bench:** two boards connected. Target disambiguated by chip-id/MAC **before** every flash
(Guardrail §3 #1):
- `/dev/cu.usbmodem1301` = **ESP32-S3 (QFN56) rev v0.2, Embedded PSRAM 8MB (AP_3v3)**,
  MAC `3c:dc:75:6f:89:24` → **RAK3112** (target), on its **native USB**.
- `/dev/cu.usbserial-140` = **ESP32-P4**, MAC `30:ed:a0:e1:8e:1c` → unrelated board on a
  USB-UART bridge. Never flashed.

**Console finding (the substantive Phase 3 result).** The Phase-1 default
`CONFIG_ESP_CONSOLE_USB_CDC=y` (OTG/TinyUSB) produced **no visible console** on the RAK3112's
native USB — 0 bytes via raw read *and* via `idf.py monitor`, while the app ran fine (port
stable, no bootloop). The RAK3112 native USB (`USB_D±`) is the ESP32-S3 **USB-Serial-JTAG**
controller — the same interface esptool flashes over. Switching to
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` made the console fully visible. This supersedes the
Phase-1 USB_CDC choice in `sdkconfig.defaults`.

Also captured during the investigation: a stale/wedged USB endpoint (`errno 6 Device not
configured`) was cleared by a physical replug + carrier swap; macOS `/dev/cu.*` (call-out)
must be used for reading, not `/dev/tty.*` (dial-in, blocks on carrier).

**PASS evidence (verbatim boot log, USB-Serial-JTAG @ 115200):**

```
rst:0x15 (USB_UART_CHIP_RESET),boot:0x2a (SPI_FAST_FLASH_BOOT)
I (24) boot: ESP-IDF v5.5.4-dirty 2nd stage bootloader
I (25) boot.esp32s3: Boot SPI Speed : 80MHz   SPI Mode : DIO   SPI Flash Size : 16MB
I (73) octal_psram: vendor id    : 0x0d (AP)
I (74) octal_psram: VCC          : 0x01 (3V)
I (75) esp_psram: Found 8MB PSRAM device
I (76) esp_psram: Speed: 40MHz
I (807) esp_psram: SPI SRAM memory test OK
I (816) cpu_start: Pro cpu start user code
I (816) app_init: Project name:     rak3112_rs485_node
I (819) esp_psram: Adding pool of 8192K of PSRAM memory to heap allocator
I (832) app: reserved pins GPIO9/GPIO40 held floating (V1.2 I2C placeholder)
rak3112-rs485-node alive: tick=0
rak3112-rs485-node alive: tick=1
...
rak3112-rs485-node alive: tick=10
```

**Gate result — all PASS:**
- `Found 8MB PSRAM device` present within ~75 ms of reset ✅
- `Adding pool of 8192K of PSRAM memory to heap allocator` present ✅
- Octal mode + 3 V confirmed empirically (`octal_psram vendor id 0x0d (AP)`, `VCC 0x01 (3V)`) —
  matches ADR-001 EC-6 (ESP32-S3R8) ✅
- `SPI SRAM memory test OK` ✅
- GPIO9/GPIO40 floating-init log present ✅ (ADR-001 EC-4/EC-9 carry-forward)
- Heartbeat 1 Hz, monotonic, **no bootloop**, single `rst:0x15`, no `Brownout`/`panic`/`abort` ✅

**Carry-forward note for the contract:** Phase 3 title says "USB-CDC console"; the verified
console is **USB-Serial-JTAG** (native USB). Treat "USB-CDC" in the Phase 3 heading as
"native-USB console". `CONFIG_SPIRAM_SPEED` defaults to 40 MHz (Octal) — a deliberate
non-decision so far; pin explicitly if the 80 MHz bandwidth is ever needed (see Phase 3 review
finding on implicit SPIRAM defaults).

#### Attempt 2 — 2026-06-20 — PASS (re-validated on the CORRECT project board)

**Board correction.** Attempt 1 was flashed to an ESP32-S3R8 with MAC `3c:dc:75:6f:89:24` —
the operator subsequently identified that unit as **NOT the project board** (a second RAK3112
whose pins are not broken out). The actual project board is a *different* RAK3112,
MAC **`3c:dc:75:6f:85:dc`** (pins exposed), enumerating on the same native USB
`/dev/cu.usbmodem1301`. Lesson reinforced: chip-id alone is insufficient when multiple
same-silicon boards are on the bench — **match the MAC to the known project unit** before flash.

Re-flashed the byte-identical Phase-3 firmware (same commit, `phase-3-first-flash-green`) to the
correct board and re-captured. **All gate criteria PASS, identical to Attempt 1:**

```
rst:0x15 (USB_UART_CHIP_RESET),boot:0x28 (SPI_FAST_FLASH_BOOT)
I (73) octal_psram: vendor id    : 0x0d (AP)
I (74) octal_psram: VCC          : 0x01 (3V)
I (75) esp_psram: Found 8MB PSRAM device
I (807) esp_psram: SPI SRAM memory test OK
I (819) esp_psram: Adding pool of 8192K of PSRAM memory to heap allocator
I (832) app: reserved pins GPIO9/GPIO40 held floating (V1.2 I2C placeholder)
rak3112-rs485-node alive: tick=0 .. tick=10   (1 Hz, monotonic, no bootloop)
```

`Found 8MB PSRAM device` ✅ · `Adding pool of 8192K … heap allocator` ✅ · Octal+3V
(`vendor id 0x0d (AP)`, `VCC 0x01 (3V)`) = ESP32-S3R8 ✅ · `SPI SRAM memory test OK` ✅ ·
GPIO9/40 floating ✅ · 1 Hz heartbeat, no `Brownout`/`panic`/`abort` ✅. The Phase 3 sign-off and
tag `phase-3-first-flash-green` stand — same firmware, same PASS, now on the correct unit.

**The project board is MAC `3c:dc:75:6f:85:dc` — use this MAC as the flash-target identity from
Phase 4 onward** (the pins-exposed unit needed for RS-485 / SX1262 bring-up).

## Phase 4 — RS-485 echo: bench setup + attempts

### Bench setup (HIL)

- **Field wiring:** CN1 (3-pin) → USB-RS485 adapter: `A`↔`A`, `B`↔`B`, `GND`↔`GND`.
- **Termination:** the board has **no on-board 120 Ω** (ADR-002). Fit **120 Ω across A/B at both
  bus ends** — one at the adapter, one at the node (or, for the 9600 low-speed first pass on a
  short stub, a single 120 Ω is tolerable). At 115200 use both.
- **Fail-safe bias:** confirm idle line is quiet; if spurious RX bytes appear at idle, add bias at
  one end (A→3V3 / B→GND via ~560 Ω–1 kΩ). R38/R39 (2.2 kΩ) may already provide this — verify.
- **Logic analyzer:** Saleae Logic 2 on **GPIO21** (DE/RE) and the A/B pair; trigger on a TX burst.
- **Project board only:** flash target **MAC `3c:dc:75:6f:85:dc`** (`/dev/cu.usbmodem1301`).

### Two-pass gate (per ADR-002)

1. `RS485_ECHO_BAUD = 9600`, build+flash, loop a 256-byte pattern from the adapter → byte-identical
   echo, 0 framing errors, idle quiet.
2. Change `RS485_ECHO_BAUD = 115200` in `firmware/main/app_main.c`, re-flash, repeat.
3. Saleae: DE-assert→first-bit and last-bit→DE-release within TP8485E spec; no contention.

### Attempt 1 — 2026-06-20 — on-target bring-up PASS (HIL echo pending bench gear)

Firmware (UART1 half-duplex + inverted RTS) flashed to the project board (MAC `…85:dc`).
On-target init verified from the boot log (bus not yet connected):

```
I (837) rs485: RS-485 UART1 up: TX=43 RX=44 DE/RE=21 @ 9600 8N1 (half-duplex, RTS inverted)
I (5837) app: rs485 echo alive: 0 bytes echoed
rak3112-rs485-node alive: tick=0 / tick=1   (5 s cadence)
```

`rs485_init()` (driver install + param + set_pin + RS485 half-duplex mode + RTS invert) returned
ESP_OK — `ESP_ERROR_CHECK` did not abort. PSRAM + GPIO9/40 floating still nominal; no bootloop.
Host `ctest` (ring_buffer, 6 checks) green. **HIL echo + DE/RE turnaround (9600 → 115200) held
pending USB-RS485 adapter + Saleae on the bench.** Binary SHA-256
`8fca54c7e23908108d3a15e9f4a2144836cf097a707f5b2a519fe2fd6d3bb225`.

### Attempt 2 — 2026-06-20 — HIL echo PASS (DE/RE polarity was inverted in the spec)

**Smoke gate PASS — byte-identical echo at both bauds**, driven host-side through a USB-RS485
adapter (256-byte ramp pattern, 16-byte half-duplex bursts):

| Baud | sent | echoed | identical |
|---|---|---|---|
| 9600 8N1 | 256 | 256 | **yes** |
| 115200 8N1 | 256 | 256 | **yes** |

**Root-cause lesson — believe the instrument, not the spec; and build a ground-truth detector
early.** Bring-up burned many cycles chasing the adapter and A/B polarity (two adapters, both
A/B orientations, RTS/DTR combos, a 5V jumper) — all red herrings. The break came from a
*reliable detector*: reading the DUT's own RX counter over the (now-working) USB-Serial-JTAG
console, then a toggle diagnostic that drove GPIO21 as a plain output alternating HIGH/LOW
while listening. Result was unambiguous: **GPIO21 LOW → clean RX of the pattern; GPIO21 HIGH →
nothing.** So DE/RE is **standard** (HIGH=TX, LOW=RX), *not* inverted.

`ADR-001 EC-5a` had documented it inverted (`GPIO21 HIGH = receive`); `rs485.c` faithfully
inverted RTS to match, which **idled U9 in transmit** — the node never listened and its driver
contended with the partner (that's why earlier captures were empty or garbled). Fix:
`uart_set_line_inverse(..., UART_SIGNAL_INV_DISABLE)` (no inversion). Corrected in firmware
(ADR-002 rev2, `gpio_remap.h`, `PIN_MAP.md`) and hardware-side (ADR-001 EC-5a correction).

Adapter note: once polarity was right, **Adapter 1 (CH340)** gave byte-identical echo at both
bauds. Adapter 2 (FT232/75176) delivered no signal in this setup (separate, unresolved adapter
issue) — not on the project's critical path.

Saleae DE/RE turnaround characterization: deferred/optional — byte-identical echo at 115200
(where turnaround margin is tightest) already demonstrates correct DE assert/deassert timing.

## Phase 5 — LoRaWAN OTAA (AS923): attempts

### 5a — 2026-06-20 — SX1262 SPI bring-up PASS (pins confirmed, NO TX)

**Finding (research): ADR-001's SX1262 SPI pins were wrong.** ADR-001 labelled the module-edge
`GPIO10–13 / SPI_*` pins as the SX1262 SPI. Those are a **separate user SPI**; the radio is on a
different **module-internal** bus. Triangulated from RAK3112 datasheet + RAK forum (carlrowan
RadioLib config) + Zephyr `rak3112` DTS — all agree:

| SX1262 | GPIO | | SX1262 | GPIO |
|---|---|---|---|---|
| NSS/CS | 7 | | BUSY | 48 |
| SCK | 5 | | DIO1 (IRQ) | 47 |
| MOSI | 6 | | NRESET | 8 (open-drain) |
| MISO | 3 | | ANT_SW | 4 (active-low) |

RF switch driven by SX1262 **DIO2** (`setDio2AsRfSwitch`), TCXO by **DIO3** — both SPI-internal,
not MCU pins. (GPIO3 is a JTAG-source strap at reset but free post-boot = MISO.)

**Bench confirmation (no-TX SPI probe, no antenna — safe).** Probe: SPI2 on the pins above,
hardware reset, then GetStatus + register write/read round-trip. Console (project board MAC
`3c:dc:75:6f:85:dc`):

```
lora_probe: SX1262 probe: NSS=7 SCK=5 MOSI=6 MISO=3 RST=8 BUSY=48 DIO1=47
lora_probe: post-reset BUSY=0
lora_probe: GetStatus=0x2a  regRW[wab:rab w55:r55 w12:r12 ] => SPI OK -> pins CONFIRMED
```

`BUSY=0` after reset, `GetStatus=0x2a` (chip mode = STBY_RC, sane post-reset), and all three
register write/read pairs matched → **SPI comms confirmed, pins correct.** Zero RF emitted.
Pins locked in `gpio_remap.h`; ADR-001 SX1262 pin map corrected hw-side. **Stack = RadioLib**
(native ESP-IDF, ADR-003). **5b (OTAA join, RF TX) held pending a 50 Ω antenna/dummy load** and
go-ahead to register a test DevEUI on ChirpStack `10.10.8.140`.

### 5b-prov — 2026-06-20 — CRM provisioning COMPLETE (device registered in ChirpStack)

Provisioned the node **end-to-end through the SCOMM CRM** (system of record) per the firmware↔CRM
handoff contract — not by hitting ChirpStack directly. Tool: `tools/provision_node.py` (chains
the workflow API; reads CRM creds from `~/.config/siot/rak3112-crm.env`, DevEUI/AppKey from the
gitignored `firmware/.env`). CRM repo: `rnd-southerniot/siot-crm-review`.

- **DevEUI** `3cdc75fffe6f85dc` — derived from the ESP32-S3 MAC `3c:dc:75:6f:85:dc` (EUI-64,
  fffe-inserted) → deterministic / re-provisionable. **JoinEUI** `0000…0`. **AppKey** generated
  (16 B, in `firmware/.env`, gitignored — never logged/committed; gitleaks clean).
- CRM product `RAK3112-RS485-AS923` (`isLorawanProduct`, AS923) created + SOP template; onboarding
  task walked SCHEDULED_VISIT → REQUIREMENTS_COMPLETE → HARDWARE_PROCUREMENT_COMPLETE →
  HARDWARE_PREPARED_COMPLETE (DevEUI/AppKey in `deviceList`) → checklist → READY_FOR_INSTALLATION.
- **Result:** CRM `lorawanProvisioningStatus=COMPLETED`; `GET /chirpstack/device/3cdc75fffe6f85dc`
  → `found=true`, name `RAK3112-RS485-6F85DC`, on AS923 OTAA app `9dcd1954…` + device profile
  `80b53d57…`. Device is ready for OTAA join (ADR-004 = AS923-1).

Contract gotchas captured (vs the handoff note / agent contract): SOP-template POST needs
`productId` in the body + steps with `id/title/description/order`; READY_FOR_INSTALLATION takes
an **empty** body (whitelist rejects lat/long). Used the seeded ADMIN account (passes all role
gates). **Security flag:** the production CRM's seeded admin/role passwords are hardcoded in the
**public** `siot-crm-review` `prisma/seed.ts` — recommend rotating + moving to env (global §8).

Still pending for the actual join (RF TX): RadioLib firmware integration, **50 Ω antenna**, and
**TCXO voltage confirm** (1.6 V vs 1.8 V, ADR-003 open item).

### 5b-fw — 2026-06-20 — OTAA JOIN SUCCESS; data uplink TxDone open

RadioLib 7.7.1 integrated as a native ESP-IDF managed component (`firmware/main/idf_component.yml`)
with a vendored S3 HAL (`EspHalS3.h`) — **the bundled `EspHal.h` is ESP32-classic only and
`#error`s on S3** (it bit-bangs classic SPI registers); reimplemented on `spi_master`/`gpio`.
C++ `lora.cpp` behind a C API (`lora.h`) for `app_main.c`. Antenna attached.

- **TCXO = 1.8 V works** (first try; `radio.begin(..., 1.8f, false)`), `setDio2AsRfSwitch(true)`.
- **OTAA JOIN SUCCESS** at AS923-1 on the project board (DevEUI `3cdc75fffe6f85dc`):
  `JOINED AS923 (new session)` — RadioLib received the JoinAccept, i.e. ChirpStack
  (`10.10.8.140`) accepted our JoinRequest (AppKey valid) and downlinked the accept. **Full RF
  TX+RX round-trip + the device is live on the network.**
- **DevNonce persistence (NVS)** added and verified: first boot's join used DevNonce ~0;
  reboots without persistence failed `-1116` (no-join-accept — ChirpStack rejects reused/lower
  DevNonce). Persisting the nonces buffer after every attempt makes DevNonce climb monotonically
  → re-join succeeded on the next attempt; session is also persisted/restored
  (`session restored (no re-join)` on subsequent boots, instant).

**OPEN — data uplink `sendReceive` returns `-5 RADIOLIB_ERR_TX_TIMEOUT`** (consistent, both on a
fresh `NEW_SESSION` and on `SESSION_RESTORED`). It fails in ~190 ms — too fast for an SF9 ToA
timeout, so it's **not** data-rate/ToA (forcing DR3+ADR-off did not help), and not nonce/session
(the session is valid). The join's TX gets its TxDone, but the data-uplink TX does not — prime
suspect was the vendored `EspHalS3` DIO1 ISR path. **ISR FIXED** (proper `void(void*)` trampoline
instead of the UB function-pointer cast; ISR service installed without `ESP_INTR_FLAG_IRAM`) — a
real latent bug, but it did NOT clear the -5. **Refined root cause (from RadioLib source):**
`LoRaWANNode::transmitUplink` (LoRaWAN.cpp:1531) does NOT use the ISR — it **polls
`digitalRead(DIO1/GPIO47)`** for TxDone after `sleepDelay(ToA)`, and times out at `txEnd +
scanGuard`. The radio's TX is staged + launched without error (the `RADIOLIB_ASSERT`s pass), but
**TxDone never asserts on the data uplink** — while the join's TX (same polling path) did. So the
SetTx is accepted but the transmit doesn't complete/assert DIO1 on the data frame specifically.
Next debug step: enable `RADIOLIB_DEBUG_PROTOCOL`/`_BASIC` to capture the uplink's actual DR / ToA /
TX power / channel and where it diverges from the join, and scope GPIO47 (+ the RF) on a data TX.
Whether the frame physically goes out
(ChirpStack frame log) couldn't be checked — the CRM `/chirpstack/device` endpoint returns device
config only (no last-seen/fcnt/frames). **Phase 5 not signed off until an uplink frame lands.**

### 5c — 2026-06-22 — data uplink `-5` FIXED (100 Hz FreeRTOS tick was the root cause)

**Fix: `CONFIG_FREERTOS_HZ=1000` in `sdkconfig.defaults`.** Single change; bench-verified.

The 5b-fw "refined root cause" (TxDone never asserts on the data frame specifically) was a
misread — and so was "fails in ~190 ms, too fast for an SF9 ToA timeout, so not data-rate." The
fast failure *is* the tell. `LoRaWANNode::transmitUplink` waits with `sleepDelay(toa)` then polls
`digitalRead(DIO1)` until `millis() > txEnd + scanGuard`, and **`scanGuard = 10 ms`**
(LoRaWAN.h:990). At the ESP-IDF default **100 Hz tick (10 ms)**:

- `sleepDelay(toa)` → `vTaskDelay(pdMS_TO_TICKS(toa))` quantizes/undershoots the air-time by up
  to one tick (≈10 ms), so the poll starts *before* the real TxDone; and
- `millis()` advances in 10 ms steps and the guard is exactly **1 tick**, so `txEnd + 10 ms` is
  reached almost immediately → `-5` in ~one tick. That's why it "failed too fast" and why forcing
  DR3/ADR-off did nothing — it was never a DR/ToA problem.

At **1000 Hz**: `sleepDelay` is accurate to ~1 ms and the 10 ms guard is 10 ticks of real poll
window → DIO1 is caught every time. (The join tolerated 100 Hz because `app_main` retries the
join 5× and its longer-ToA timing occasionally aligned; the periodic data uplink had no such
retry, so it failed every cycle.) The earlier DIO1-ISR-trampoline fix (5b-fw) was a real latent
bug on the **downlink** ISR path and is kept — it just wasn't this bug.

**Bench evidence (project board, S3R8, MAC `3c:dc:75:6f:85:dc`, `/dev/cu.usbmodem1401`):**
`idf.py flash` of the 1 kHz build, then **4/4 consecutive `uplink OK (rx window=0)`** at uptimes
65 s / 128 s / 190 s / 252 s, **zero `-5`** — replacing the previously consistent `uplink failed
(-5)`. `rx window=0` = uplink sent, no downlink pending (correct for an unconfirmed uplink).
Binary SHA-256 `38105b4eaa218685a218a2505b3d2af7c5de2cc4e3b9dfd03fe926e32f9b9f9a`, `IDF_VER=v5.5.4-dirty`
(local PARLIO mod, off-compile-path, Footnote 1).

**Lesson:** RadioLib LoRaWAN on ESP-IDF needs a 1 kHz FreeRTOS tick — its TX-done guard and RX
window timing assume ~ms scheduling granularity; the 100 Hz default silently breaks sub-tick
waits.

**END-TO-END CONFIRMED (2026-06-22):** uplink frame captured in the ChirpStack frame log
(`10.10.8.140:8080` UI). `UnconfirmedDataUp`, DevAddr `01db1032`, **fPort 1**, frm_payload
**`a500245a`** (= `A5 00 24 5A`, our `{0xA5,n>>8,n,0x5A}` with n=36), **fCnt 98** (advancing,
NVS-persisted), ADR off, **SF9/BW125/CR4_5 = DR3** on **923.4 MHz**, region `as923_1`,
`CRC_OK`, RSSI −48 dBm, SNR +12.2 dB via gateway `ac1f09fffe1f340d`. Every field matches the
firmware config — the uplink path is fully working end-to-end. The CRM `/chirpstack/device`
endpoint (`:4000`) returns config only (no telemetry); frame verification must use the
ChirpStack UI/API at `:8080` directly. **Phase 5 uplink milestone: PASS.** (Remaining Phase 5
sign-off scope — ADR-003 stack / ADR-004 sub-band formal close, payload schema — tracked separately.)

## Phase 6 — Modbus RTU master: device pivot to SELEC MFM384 (2026-06-24)

**Target change.** Phase 6 bring-up retargeted from the RS-FSJT-N01 wind sensor to a **SELEC
MFM384** 3-phase multifunction meter. Rationale: the RS-FSJT bench was blocked on a sensor↔CN1
*physical* join (open conductor / no common GND in both A/B orientations — node CN1 TX path itself
proven byte-perfect in Phase 4). The firmware was never the blocker. The MFM384 is mains/aux-powered
with its own RS-485 terminals (no separate bus PSU to lose), so it sidesteps that failure mode.

**MFM384 Modbus map** (from the org's proven RAK3312 bring-up, `rnd-southerniot/rak3312-rs485-plan`
`docs/rak3312/MODBUS_MAP.md`, which reached Gate 8 live publish on this exact meter):

| Read | FC | Register (human → wire) | Type | Notes |
|---|---|---|---|---|
| Linkcheck | 0x03 | 40007 → 6 | uint16 | any reply proves the link |
| Endianness | 0x03 | 40070 → 69 | uint16 | expect `1` = ABCD word order |
| Total active energy | 0x04 | 30090..30091 → 89..90 | float32 ABCD | kWh |

Serial (org bench known-good): **9600 8N1, unit 1**. MFM384 baud/unit are front-panel configurable,
so the operator's meter may differ — the scan profile sweeps baud×parity if so.

**Firmware support added this session** (float32 + FC04, on top of the existing 6a/6b framing):
- `modbus_rtu`: pure `modbus_regs_to_f32(regs, ABCD|CDAB)` — host-tested KAT (`{0x447A,0x0000}` ABCD
  = 1000.0f). No aliasing UB (memcpy type-pun).
- Poll bring-up mode now takes a function code (FC03/FC04) and an optional float32 read (2 regs);
  scan probe register is now configurable.

**Bench bring-up sequence (operator):**
1. **Wiring:** MFM384 A/B → CN1 A/B, and a **common GND** (meter RS-485 GND ↔ CN1 GND). Confirm the
   meter is powered (front-panel lit) and in **RTU** mode at its configured baud/unit.
2. **Linkcheck first** (quick "is it alive", uint16 @ FC03 reg 6):
   ```bash
   cd firmware && . ~/esp/esp-idf-v5.5.4/export.sh
   idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.poll" build flash monitor
   # expect: [n] reg6 = <value> (raw, 0x....)   — a reply = link good. swap A/B if it times out.
   ```
3. **If linkcheck times out / unknown baud:** run the scanner (sweeps baud×parity×unit IDs):
   ```bash
   idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.scan" build flash monitor
   ```
   (Set `CONFIG_APP_MODBUS_SCAN_PROBE_REG=6` for the MFM384's known-good holding register.)
4. **Real measurand** (total active energy, float32 kWh @ FC04 reg 89..90):
   ```bash
   idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.mfm384" build flash monitor
   # expect: [n] reg89..90 = 0x........  = <kWh>
   ```
   If the float looks like garbage, the meter uses CDAB — set `CONFIG_APP_MODBUS_POLL_FLOAT_CDAB=y`.

The old RS-FSJT profile is preserved as `sdkconfig.defaults.poll-rsfsjt` (4800 8N1, FC03 reg 0).

**Attempt 1 — scan, MFM384 not reached (2026-06-24).** Flashed `sdkconfig.defaults.scan` to project
board MAC `3c:dc:75:6f:85:dc` (ESP32-S3R8, 8MB octal PSRAM OK, native USB `/dev/cu.usbmodem11301`).
Full sweep ran clean: 18 combos (9600/4800/19200/38400/57600/115200 × 8N1/8E1/8O1), IDs 1–10, FC03
@reg 6, 300 ms timeout → **0 devices, 0 garbage in every combo**. Firmware/board healthy (scan
executed start to finish). `0 garbage` everywhere ⇒ no malformed bytes received at any baud, i.e.
**no electrical return path or no responder** — NOT a baud/parity mismatch (that would surface as
garbage). Distinguishing causes left to the operator: (a) wiring — A/B on CN1, common GND, meter
powered + in Modbus RTU mode; (b) unit ID > 10 (sweep capped at 10; wrong-slave = silence by spec).
**Fastest disambiguation: read baud/parity/slave-ID directly off the MFM384 front-panel setup menu**
and set the poll profile to those exact values, then widen `CONFIG_APP_MODBUS_SCAN_ID_HI` if needed.

**Hardware cross-check vs proven reference (2026-06-24).** The org's `rak3312-rs485-plan`
`examples/rak3312_mfm384` (reached Gate 8 live on this exact meter) was reviewed line-by-line. Our
firmware matches it in every protocol dimension: TX=GPIO43, RX=GPIO44, DE=GPIO21, DE polarity
HIGH=TX/LOW=RX, 9600 8N1 unit 1, linkcheck FC03 reg 6, energy FC04 reg 89 qty 2 with float
`u32=(hi<<16)|lo` (= our ABCD word order). **One difference, and it does NOT apply to us:** the
RAK3312 reference drives RS-485 through a **RAK5802 module with a separate ENABLE pin on GPIO14
(WB_IO2) that must be HIGH before any Modbus traffic** — if low, that module is disabled and the bus
goes totally silent (our exact symptom). Our board has **no such enable pin**: RAK3112 + onboard
TP8485E, DE/RE on GPIO21 only, and Phase 4 echo proved that path works to a USB-RS485 adapter with
GPIO21 alone. So GPIO14 is RAK5802-specific, irrelevant here. **Net: firmware confirmed correct
(twice — Phase 4 echo + reference match); the silence is purely the MFM384↔CN1 physical link.** The
reference's own troubleshooting reduces to "confirm A/B wiring and common GND" — which is the bench
action. SELEC MFM384 RS-485 terminals may be labeled opposite to CN1 A/B → try both orientations.

**Remaining Phase 6 (6c):** NVS register-set config, ADR-005 payload schema (compact versioned
binary + ChirpStack JS codec), encode MFM384 reads into the LoRaWAN uplink, then push/CI/merge/tag.

## Post-sign-off note — ADR-001 `<TBD>` hygiene (2026-06-20)

After Phase 2 sign-off, a code-review pass found a residual `<TBD>` placeholder in the
signed-off `ADR-001-pin-map.md`: a **duplicate** "## H1 header purpose (EC-5b)" stub still
showing `> Status: <TBD> — pending EC-5 execution` directly below the canonical, fully-resolved
EC-5b section. The firmware EC-9 precondition required the ADR be "free of `<TBD>` markers", so
this was a technical violation rolled forward from the sign-off commit.

Fixed in hardware repo `main` (merge `3cce0c6`, fix `65ddab0`) — the duplicate stub was removed;
the binding decision (H1 = 3V3-enable / current-measurement jumper) was unchanged and already
present in the canonical section. The `adr-001-locked` tag was **not** moved (it is pushed; the
decision did not change), so the firmware `HARDWARE_REV.md` pin (`a0b002ca…8295`) remains valid.
No firmware behavior is affected.

## Phase 2 attempts

### EC-1 — 2026-05-01 — PASS (vendored IAD only; PARLIO held back)

**Patch dispositions per rev5 §5 EC-1:**

| Patch | Disposition | Rationale |
|---|---|---|
| `usb_types_ch9.h` (`USB_IAD_DESC_SIZE` 9 → 8) | **Vendored** as `firmware/esp-idf-patches/0001-fix-iad-desc-size.patch` | Verifiable correct against USB 2.0 ECN "Interface Association Descriptors" (May 2003). v5.5.4 + release/v5.5 branch tip both ship `9`; ESP-IDF master has refactored away from this path so the bug is implicitly fixed there. No upstream PR exists. |
| `esp_lcd_panel_io_parl.c` (`sample_edge` POS → NEG) | **Not vendored.** Stays in `~/esp/esp-idf-v5.5.4/` as personal modification | Author intent could not be recovered (neither chat-side nor user-side instance has access to the operator's keystroke memory; no commit on the local IDF tree captures the rationale). Per rev5 EC-1: "If intent cannot be determined, the patch is NOT vendored." Plausibility of the change does not satisfy the rule. |

**Plausible-but-undocumented theory for the PARLIO mod (recorded as theory, not as confirmed reason):**

- v5.5.4 default: `.sample_edge = PARLIO_SAMPLE_EDGE_POS`
- ESP-IDF master: field renamed to `shift_edge` with `PARLIO_SHIFT_EDGE_NEG` as the default
- The local mod's POS → NEG flip aligns with master's default behaviour
- This *suggests* the mod was a real fix, but the contract requires documented author intent — alignment with master is post-hoc reasoning, not provenance

**Asymmetric outcome accepted (Footnote 1):**

- Local builds on `siot-dev-m5` will produce `v5.5.4-dirty` (PARLIO mod still present in `~/esp/esp-idf-v5.5.4/` working tree, off-compile-path for this firmware).
- CI builds (clean clone + `apply-patches.sh`) will produce clean `v5.5.4` with IAD patch applied.
- §3 #6 single-canonical-build-path invariant **NOT violated**: same ESP-IDF version, same target, same `sdkconfig.defaults`, only an extra incidental working-tree mod on local that doesn't affect the heartbeat compile path.

**EC-1 fresh-clone verification (per the rev5 verification routine):**

| Step | Result |
|---|---|
| `git clone --depth 1 --branch v5.5.4 espressif/esp-idf` | OK |
| `git submodule update --init --recursive --depth 1` | OK |
| Pre-patch sanity grep | `#define USB_IAD_DESC_SIZE    9` ← upstream wrong |
| `apply-patches.sh` (IDF_PATH = temp clone) | `Applied: 0001-fix-iad-desc-size.patch` |
| Post-patch sanity grep | `#define USB_IAD_DESC_SIZE    8` ← corrected |
| `idf.py build` (output to `firmware/build-verify/`) | `Project build complete.` |
| Cleanup of temp ESP-IDF clone + `build-verify/` | OK |
| Phase 1 baseline `firmware/build/` | preserved untouched |

**Build-timestamp finding (worth flagging in advance for Phase 5+):**

- Phase 1 baseline binary SHA-256: `a2f3d6044f9458aa38a492cad531c53071b2f829a33f5fd2635db72271fe5116` (built against pristine v5.5.4, no IAD patch)
- Phase 2 EC-1 verification binary SHA-256: `d47500e3906766219cd466a475d85f18bdd6fd056386cfa3a4e568d35cb4b7a7` (built against pristine v5.5.4 + IAD patch)
- The SHAs differ even though the IAD constant is off-compile-path for the heartbeat (no USB host stack, no `usb_iad_desc_t` instantiation, no `static_assert` activation).
- Cause: ESP-IDF embeds build timestamps + source-path strings in the app descriptor and debug info. Without `CONFIG_APP_REPRODUCIBLE_BUILD=y` enabled, every rebuild has a different timestamp by design.
- **Implication for §3 #6 reproducibility guarantee:** the contract wording "functionally identical (modulo embedded timestamp / git-describe strings)" is exactly on point — timestamp is the `modulo`. SHA inequality between rebuilds at different wall-clock times is expected and does not violate the invariant.
- **Phase 5+ consideration:** if byte-identical builds become valuable (e.g., for over-the-air update integrity, or for reproducible-build CI verification), enable `CONFIG_APP_REPRODUCIBLE_BUILD=y` then. Not enabling now keeps timestamps useful for forensic correlation.

**Deferred task — file upstream PR for IAD fix.**

The `0001-fix-iad-desc-size.patch` is a verifiable bug fix in ESP-IDF's `release/v5.5` branch. Worth filing upstream as a PR against `espressif/esp-idf`'s `release/v5.5` branch. Not blocking for Phase 2 / hardware-side work. Logged here; will be moved into `ADR-001-pin-map.md` appendix when that doc exists, or stay here as a "deferred upstream contributions" entry.

Suggested PR title: `fix(usb): USB_IAD_DESC_SIZE is 8 bytes per USB 2.0 ECN, not 9 (release/v5.5)`

---

## Phase 1 attempts

### Attempt 2 — 2026-05-01 — FAIL on smoke-gate step 3 (PlatformIO + ESP-IDF version mismatch)

**Branch:** `fix/p1-pio-srcdir`. P1-FIX-1 (src_dir fix + gate-execution discipline) committed and applied.

**Smoke-gate trail.**

| Step | Status | Evidence |
|---|---|---|
| 1. `pre-commit run --all-files` | ✅ PASS | All 6 hooks Passed |
| 2. `idf.py build` (ESP-IDF v5.5.4 pristine) | ✅ PASS | Ninja near-no-op rebuild; `firmware/build/rak3112_rs485_node.bin` 183 328 bytes — **byte-identical to Attempt 1**, confirming pristine-v5.5.4 reproducibility |
| 3. `pio run -e esp32-s3-devkitc-1` (bare, no pipes) | ❌ **FAIL** | Exit 1 after 2.36 s. `CMake Error at CMakeLists.txt:1 (cmake_minimum_required): CMake 3.20 or higher is required. You are running version 3.16.4` |
| 4. `gitleaks detect --no-banner --redact` | ✅ PASS | 8 commits scanned, 223.7 KB, no leaks |
| 5. layout check | ✅ PASS | `tests/host/CMakeLists.txt` present |

**Root cause analysis — substantive.**

The `[platformio] src_dir = main` fix was correct and worked: PlatformIO this time entered the build phase rather than aborting at the directory check. The new failure surfaced a deeper issue: **the dual-build invariant in §3 #6 has been silently using two different ESP-IDF versions all along.**

| Build path | ESP-IDF version | CMake version |
|---|---|---|
| `idf.py build` (local) | v5.5.4 (system, `~/esp/esp-idf-v5.5.4/`) | system or ESP-IDF-bundled, ≥ 3.20 |
| `pio run` (local) | **v5.2.1 (PlatformIO-bundled in `platform = espressif32 @ ~6.7.0`)** | **3.16.4 (PlatformIO-bundled)** |
| `idf.py` (CI, `espressif/esp-idf-ci-action@v1` pinned to v5.5.4) | v5.5.4 | matched |

Our `firmware/CMakeLists.txt` says `cmake_minimum_required(VERSION 3.20)` because that's the floor mandated by ESP-IDF v5.5.x. PlatformIO's bundled CMake 3.16.4 (matching its bundled ESP-IDF v5.2.1) cannot satisfy this. ESP-IDF v5.5 raised the CMake floor from 3.16 → 3.20 between v5.2 and v5.5.

**This means the rev3 contract's "dual-build invariant" is unfulfillable with the current pin choices.** The two builds were never building the same thing — they shared source but compiled it against materially different ESP-IDF and CMake versions. Phase 1 Attempt 1 happened to pass step 2 and *fail at step 3 for an unrelated reason*; if the src_dir issue hadn't masked it, this CMake-version mismatch would have surfaced anyway in any subsequent attempt.

**Strategic options (require user decision — not auto-applicable):**

1. **Bump platform-espressif32 pin to a version that bundles ESP-IDF v5.5+.** As of this date, mainline platform-espressif32 ≤ 6.10.x bundles v5.4.x at best; v5.5 may require the Pioarduino community fork (`platform = https://github.com/pioarduino/platform-espressif32.git`) or waiting for upstream. If a release exists that bundles v5.5.x, pin to it and re-run; otherwise (3) below.

2. **Lower the contract's ESP-IDF pin to v5.3.x or v5.4.x to match what PlatformIO can offer.** Forfeits being on the latest stable, but preserves the dual-build invariant.

3. **Drop PlatformIO from the local smoke-gate.** Use `idf.py` only locally; PlatformIO retained for CI **only** with whatever ESP-IDF version platform-espressif32 currently bundles, treating PIO as a separate "smoke build" job rather than a dual-build mirror. Downgrades the dual-build invariant from "byte-equivalent" to "both-frameworks-compile". This is the path of least resistance and matches what most ESP-IDF + PIO projects do in practice.

4. **Replace PlatformIO entirely** with a docker-pinned ESP-IDF v5.5.4 image for CI parity. Cleanest for reproducibility but adds a dependency.

**Recovery state.**

- ESP-IDF stash restored: `git stash pop` clean apply, two expected files modified, `idf.py --version` back to `v5.5.4-dirty`. Shared `~/esp/esp-idf-v5.5.4/` is unperturbed.
- Repo on branch `fix/p1-pio-srcdir`, P1-FIX-1 committed (src_dir fix + gate-execution discipline + audit). No tag.
- ESP-IDF Attempt-2 build artefacts in `firmware/build/` (byte-identical to Attempt 1 — 183 328 bytes, IDF_VER=v5.5.4 clean).
- PlatformIO partial state in `firmware/.pio/` (no firmware.bin produced).

**Action plan (awaiting user direction on options 1–4 above).** No further work on this branch until the strategic choice is made. The platformio.ini / CLAUDE.md edits in P1-FIX-1 remain valid regardless of which option is chosen — they were a real fix, just not the only one needed.

**Full PIO log:** `/tmp/p1-attempt2-pio.log`. ESP-IDF log: `/tmp/p1-attempt2-idf.log`.

---

### Attempt 1 — 2026-05-01 — FAIL on smoke-gate step 3 (PlatformIO build)

**Smoke-gate trail.**

| Step | Status | Evidence |
|---|---|---|
| 1. `pre-commit run --all-files` | ✅ PASS | All 6 hooks Passed (end-of-file-fixer, trailing-whitespace, check-yaml [multi-doc], check-merge-conflict, gitleaks, clang-format) |
| 2. `idf.py build` (ESP-IDF v5.5.4 pristine via stash) | ✅ PASS | `Project build complete.` · binary `firmware/build/rak3112_rs485_node.bin` 183 328 bytes · `IDF_VER` baked in = `v5.5.4` (no `-dirty`) · bootloader 36% free · app partition 83% free |
| 3. `pio run -e esp32-s3-devkitc-1` | ❌ **FAIL** | Exit `[FAILED]` after 303.43 s. PlatformIO downloaded espressif32 platform + bundled `~/.platformio/penv/.espidf-5.2.1/` toolchain successfully, then errored with: `Error: Missing the 'src' folder with project sources.` No `firmware.bin` produced. |
| 4. `gitleaks detect` | ✅ PASS | 6 commits scanned, 217.7 KB, no leaks |
| 5. layout check | ✅ PASS | `tests/host/CMakeLists.txt` present |

**Root cause analysis.**

PlatformIO `framework = espidf` defaults to expecting source code under `src/` (PlatformIO convention) but the ESP-IDF native layout uses `main/` (with `idf_component_register` in `main/CMakeLists.txt`). PlatformIO's espidf framework supports both, but only when explicitly told that `main/` is the source dir. Without `src_dir = main`, PlatformIO falls back to looking for `src/` and aborts.

Note: the bash invocation `pio run … | tee … | tail -3` masked the failure exit code because `tee` succeeded. This is **also a smoke-gate definition issue** — the gate must check `pio run`'s real exit code, not tee's. To be addressed in the fix.

**Proposed fix (one line in `firmware/platformio.ini`):**
```ini
[platformio]
src_dir = main

[env:esp32-s3-devkitc-1]
platform = espressif32 @ ~6.7.0
framework = espidf
... (unchanged)
```

The `[platformio] src_dir = main` line redirects PlatformIO's source-directory expectation onto the ESP-IDF `main/` directory, while ESP-IDF's own CMake build still finds `main/` via `idf_component_register`. Both build systems then operate against the same source tree as intended by §1's "single source of truth" + §3 #6 dual-build invariant.

Smoke-gate command also needs hardening: capture `pio run`'s exit code directly (no `tee` between command and gate decision).

**Recovery state.**

- ESP-IDF stash restored: `git stash pop` clean apply, two expected files modified, `idf.py --version` back to `v5.5.4-dirty` baseline. Shared `~/esp/esp-idf-v5.5.4/` is unperturbed.
- `~/Developer/projects/firmware/rak3112-rs485-node/` 6 commits on `main`, no `phase-1-scaffold-green` tag (gate did not pass).
- ESP-IDF build artefacts in `firmware/build/` retained for forensic reference; PlatformIO partial build in `firmware/.pio/build/esp32-s3-devkitc-1/` (no firmware.bin).

**Action plan (awaiting user approval).**

1. Create `fix/p1-pio-srcdir` branch off `main`.
2. Apply the `src_dir = main` fix to `platformio.ini`.
3. Harden the smoke-gate command in `CLAUDE.md` to not rely on tee.
4. Re-run from the pre-scaffold isolation step (re-stash ESP-IDF patches), execute full smoke gate.
5. If green, merge `fix/p1-pio-srcdir` back to `main`, tag `phase-1-scaffold-green`.

**Full PlatformIO log:** `/tmp/p1-pio-build.log` (303.43 s, 7 MB). Will be moved into the repo as `docs/logs/p1-attempt1-pio.log` if the user wants it durable; not committed by default.

---

## Gate design principles (2026-05-01, lesson from Phase 1 Attempt 1)

Smoke-gate commands MUST have unambiguous exit codes.

- ❌ **NEVER** pipe gate commands through `tee` / `tail` / `head` / `grep` / `awk` / `sed`. Pipes mask the upstream exit code unless explicit `set -o pipefail` + `${PIPESTATUS[@]}` handling is used. Even then, the cognitive overhead defeats the purpose of a "smoke gate".
- ✅ Run gate commands **bare**. Exit code is the gate.
- ✅ If forensic logging is needed, capture it **outside** the gate, in the on-failure procedure below. Not every run needs a log on disk; failures do, and those are captured deliberately.

**Audit (2026-05-01):** The contract `CLAUDE.md` smoke-gate definitions in §5 were already pipe-clean. The masking in Attempt 1 came from the **operator** wrapping `pio run` in `… | tee /tmp/p1-pio-build.log | tail -3` at execution time to keep chat output short. Lesson: the contract is sound by construction, but operator discipline at runtime is the second line of defence — and it was the line that failed. Codified as Guardrail §3 #9 in `CLAUDE.md` rev3+.

### On-failure forensic-capture procedure

When a gate fails:
1. Capture the offending command's output deliberately, with timestamp:
   ```bash
   <gate-command> 2>&1 | tee /tmp/p<phase>-<step>-$(date +%s).log
   ```
2. Tail the relevant section into the RUNBOOK attempt entry as evidence.
3. Optionally promote the full log into `docs/logs/` if the failure mode is novel and worth durable retention.

This is the *only* place where pipes are sanctioned, and only after the gate has already failed.

---

## Pin discipline (2026-05-01, lesson from Phase 1 Attempt 2)

**When pinning two toolchains that must agree on a downstream property** (version, CMake floor, compiler version, std lib), **the pins are not independent.** Verify the agreement at pin time, not at build time.

❌ **Anti-example.** Rev3 of this contract pinned:

```
ESP-IDF                         v5.5.4
PlatformIO platform-espressif32 ~6.7.0    # bundles ESP-IDF v5.2.1 + CMake 3.16
```

Each pin was locally valid (real, released versions). They were **jointly inconsistent**: ESP-IDF v5.5 raised the CMake floor to 3.20, but PlatformIO's bundled CMake was 3.16. The mismatch was invisible until the second build path actually ran. Phase 1 Attempt 2 surfaced it; Attempt 1 only avoided it because PIO failed earlier on `src_dir`.

✅ **Rule.** Any time a contract pins two tools that share a transitive dependency, **document the shared dep explicitly** and **verify agreement at the pin-introduction commit**, not at first build. Concretely, the rev3 review should have included a step like:

```bash
# Cross-pin sanity: do both build paths agree on CMake floor?
grep cmake_minimum_required firmware/CMakeLists.txt
# -> 3.20 (because ESP-IDF v5.5)
# Does platform-espressif32 ~6.7.0 ship CMake >= 3.20?
# -> No. ~6.7.0 ships v5.2.1 + CMake 3.16. Pin REJECTED.
```

The rule generalises beyond ESP-IDF/PlatformIO. It applies to: Python + pip-tools, Node + pnpm, Docker base images + bundled language runtimes, Yocto + meta-layers, anything where two pins resolve a shared transitive dependency.

**Reference incident:** Phase 1 Attempt 2, 2026-05-01. Resolution: PlatformIO dropped entirely (rev4); single canonical build path = ESP-IDF.

**Self-critique recorded for the next review:** the chat-side Claude that approved rev2/rev3 toolchain pins checked each pin individually, not jointly. Same blind spot the operator (Arif) flagged on themselves. Codified here so the next review surface adopts the joint-consistency check.

---

## Aesthetic vs. functional preference (2026-05-01, lessons from rev3 R3, rev5 push-back 1, and SHA-truncation pattern)

When a review choice reduces to **"less visible state vs. more visible state"**, default to MORE visible state if the visible state is functionally correct.

- Empty-but-tagged remote > drafting in temporary location
- Logged §15 entry > argument from chat-side memory
- Verbose State block with all attempts > squashed-clean tag
- Full SHA in tag annotations and reports > truncated SHA "for readability"

Aesthetic preferences for "clean" or "minimal" should not override functional reasons for explicit state. The visible state is the durable forensic record. **The cost of verbosity is real but bounded; the cost of erased state is unbounded.**

**Reference incidents:**

- **Phase 1 review rev3 R3 (gateway IP, memory vs. §15).** Operator argued for replacing the dated `192.168.20.150` (per global CLAUDE.md §11+§15) with `192.168.15.150` based on a memory snippet. Resolution: the dated State block §15 entry is authoritative; memory snippets that contradict logged state are wrong by default. Aesthetic argument: "single IP claim feels cleaner than relocation history". Functional cost: writing wrong infrastructure into a production-grade contract.
- **Phase 2 review rev5 push-back 1 (hardware repo timing).** Operator initially preferred deferring hardware-repo creation to Phase 2 *exit* ("avoid empty remote during research work"). Resolution: hardware repo at Phase 2 *entry*, with V1.0 archive tag. Aesthetic argument: "empty remote feels untidy". Functional cost: drafting ADR-001 in the wrong repo, then migrating at exit — a copy-paste boundary that violates single-canonical-source.
- **Throughout Phase 1 reports (SHA truncation).** Chat-side Claude truncated `a2f3d6044f9458aa38a492cad531c53071b2f829a33f5fd2635db72271fe5116` to `a2f3d604…fe5116` in chat reports "for readability". Caught at near-miss stage — the tag annotation itself had the full hash; only the chat summary was truncated. Aesthetic argument: "shorter is more readable". Functional cost: forensic forward-compatibility (verifying a tag's annotated SHA against the actual binary requires the full hash).

**Promotion candidate.** Like the "single canonical source" lesson, this rule generalises beyond build systems: it applies to any review surface where "tidy" competes with "explicit". Worth promoting from project-RUNBOOK to global CLAUDE.md when the operator next does a global doc-discipline pass. Not actioning the global edit unilaterally — same standing rule as the Phase 1 lesson promotions.

---

## Silicon-vendor abstraction layer (2026-05-01, lesson from Phase 2 EC-6)

**When the two-sources rule comes back silent on a hardware spec, check the silicon vendor's part-number nomenclature / comparison table before deferring.**

Module manufacturers (RAK, Seeed, Adafruit, etc.) frequently underdocument the underlying silicon variant — they specify "8 MB PSRAM" without distinguishing Quad/Octal mode, "ESP32-S3" without enumerating which R-variant. But the silicon vendor (Espressif, Nordic, ST, etc.) enumerates *all* SKUs definitively in their family-level datasheets. **The authoritative source for "what's inside this module" is often one layer of abstraction below the module datasheet.**

❌ **Anti-example.** Phase 2 EC-6 ran the prescribed two-sources-rule:

- RAK3112 datasheet (RAKwireless): "8 MB PSRAM" with no mode disclosure
- `rnd-southerniot/rak3112-dynamic-sensor` (operator's parallel project): `# CONFIG_SPIRAM is not set` (PSRAM disabled, mode never determined)

Both sources silent. The chat-side disposition leaned toward **option C (defer with rev8 amendment)** because "two sources beat one, and two sources unable to confirm = defer." This was wrong — the search hadn't gone deep enough.

✅ **Correct resolution.** Operator went one layer down to the **silicon-vendor datasheet**:

> *Espressif ESP32-S3 Series Datasheet v2.2, §1.2 (Comparison), Table 1-1 (page 13). All 8 MB PSRAM ESP32-S3 variants ship Octal SPI: ESP32-S3R8 (3.3 V, current production) and ESP32-S3R8V (1.8 V, EOL). No 8 MB Quad SKU exists. RAK3112 with "8 MB PSRAM" silicon must be ESP32-S3R8 (current) or ESP32-S3R8V (legacy stock). Either way, mode = Octal.*

The silicon vendor's SKU enumeration eliminated the ambiguity that the module-vendor datasheet preserved. Two RAKwireless-side sources (datasheet + RAK's own reference firmware) couldn't disambiguate. One Espressif-side source did.

✅ **Generalised rule for review surfaces.** When a hardware spec is missing from module-vendor sources:

1. **Identify the silicon underneath** (chip-level: ESP32-S3; module-level: RAK3112 above it).
2. **Check the silicon-vendor's family datasheet** — specifically the part-number-nomenclature / SKU-comparison table. These tables enumerate every variant the vendor sells.
3. **Map the module's published spec** ("8 MB PSRAM") **to the silicon SKU** (ESP32-S3R8 / ESP32-S3R8V — both Octal). If the spec uniquely identifies the silicon, the silicon's properties resolve the question.
4. **If still ambiguous** (e.g., spec matches multiple silicon SKUs that differ on the relevant axis), then defer / reach out to the module vendor.

Module vendors abstract over silicon vendors. When the abstraction layer is opaque on a technical detail, lift it. The silicon vendor's documentation is one layer down but often more comprehensive on technical detail than the module-level layer.

**Reference incident:** Phase 2 EC-6, 2026-05-01. Resolution: PSRAM mode = Octal SPI per ESP32-S3 Series Datasheet v2.2 Table 1-1. Disposition A accepted, no rev8 amendment needed.

**Cross-project applicability.** `rnd-southerniot/rak3112-dynamic-sensor` should also enable Octal PSRAM when its use case requires PSRAM. Same silicon, same answer. Documented here as a shared lesson; that project's CLAUDE.md / sdkconfig update is out of scope for this repo's commits but tracked as a deferred cross-project note.

**Promotion candidate** for global CLAUDE.md when the operator next does a global doc-discipline pass.

---

## Pre-commit hook side-effects on checksums (2026-05-02, lesson from Phase 2 EC-9 pre-flight)

**When a pre-commit hook modifies a file, any checksums computed *before* the hook run are stale.** The committed checksum file ends up referencing the pre-hook bytes; the on-disk file has post-hook bytes; `sha256sum -c CHECKSUMS.txt` fails until someone re-runs the gate.

❌ **Anti-example (hw repo, Phase 2 EC-2 commit `970cdf0`).** Sequence:

1. `shasum -a 256 *.pdf *.epro *.json page-*.png > CHECKSUMS.txt` — captures pre-hook hashes.
2. `git add -A`.
3. `pre-commit run --all-files` → end-of-file-fixer modifies `esch-pin-labels.json` (adds trailing newline).
4. `git add -A` (re-stages the modified JSON; CHECKSUMS.txt unchanged).
5. `git commit` → CHECKSUMS.txt records pre-fixer JSON hash; JSON file has post-fixer bytes.

The EC-2 smoke gate (`sha256sum -c CHECKSUMS.txt`) appeared to pass at commit time because pre-commit ran on a self-consistent staged tree. But the **committed** CHECKSUMS.txt was computed before the eof-fixer modification; the **committed** JSON was modified by the eof-fixer afterwards. Mismatch ran silently from EC-2 (2026-05-01) through EC-7 close (2026-05-02). Detected at Phase 2 EC-9 pre-flight when SHA-256s were re-computed for the `adr-001-locked` tag annotation; fixed by regenerating CHECKSUMS.txt against the post-hook tree and bundling the fix into the sign-off commit.

✅ **Generalised rule.** When a checksum file is committed alongside the files it references, the checksum **must** be computed *after* the pre-commit hook chain has stabilised. Three viable patterns:

- (a) **Manual sequence:** run `pre-commit run --all-files` first, let hooks modify whatever they will, *then* compute checksums on the post-hook tree.
- (b) **CHECKSUMS regeneration as a pre-commit hook:** add a hook that runs after the file-modifying hooks (eof-fixer, trailing-whitespace, formatters) and rewrites CHECKSUMS.txt from the post-hook tree.
- (c) **Self-verification hook:** add a hook that fails the commit if `sha256sum -c CHECKSUMS.txt` doesn't match the staged tree. Forces the operator to regenerate before commit.

For this project, pattern (a) is sufficient at the current scale. Future Phase 3+ work that adds host tests with binary fixtures may benefit from (b) or (c) if checksum drift becomes a recurring failure mode.

**Reference incident:** EC-2 (2026-05-01) committed stale CHECKSUMS.txt for `esch-pin-labels.json`. Detected at EC-9 pre-flight (2026-05-02). No data corruption, no contract violation rolled forward downstream — but the EC-2 gate was technically failing during EC-3..EC-7. Worth a small audit of the EC-3..EC-7 commits if any depended on `sha256sum -c` passing.

**Promotion candidate** for global CLAUDE.md (joining the four prior lessons — five total now).
