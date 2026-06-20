# ADR-002: RS-485 bus electrical + DE/RE control strategy

## Status
ACCEPTED-pending-sign-off  ·  Phase 4 · **rev2** (2026-06-20: DE/RE polarity corrected to
STANDARD after on-board measurement; the rev1 "inverted RTS" approach idled U9 in transmit
and the node never received. See "DE/RE polarity correction" below.)

## Context

Phase 4 brings up the RS-485 field bus: drive the **TP8485E-SR** (U9) half-duplex
transceiver from an ESP32-S3 UART and echo bytes to a USB-RS485 adapter at 9600 8N1
then 115200, characterising DE/RE turnaround on a logic analyzer.

Hardware facts are fixed by **hardware ADR-001 (adr-001-locked, hw-repo a0b002c)**:

- UART pins: **TX = GPIO43** → U9 `D` via R34 (1 kΩ series); **RX = GPIO44** ← U9 `R`.
  Console is native USB-Serial-JTAG, so UART0/1 on these pins is free for the field bus.
- DE/RE control: **GPIO21** → Q6 (LMBT4401LT1G NPN) base via R35 (10 kΩ) → U9 DE+RE
  (tied) via R36 (0 Ω). **STANDARD polarity — measured on the project board 2026-06-20:**
  - `GPIO21 HIGH` → U9 **TRANSMIT** (DE active)
  - `GPIO21 LOW` (idle) → U9 **RECEIVE**
  This is the *usual* ESP-IDF RS-485 convention (RTS/DE active-high = TX), so **no inversion**.
  > ADR-001 EC-5a described this as inverted (`GPIO21 HIGH = receive`). That is **wrong** —
  > a toggle test on the board (forced GPIO21 LOW → clean RX of the test pattern; HIGH → none)
  > proved standard polarity. A correction is filed against hardware ADR-001 EC-5a.
- Field connector CN1 (3-pin: A / B / GND). D6 (LRC399-04AT1G) ESD array on the A/B pair.

### Bus-electrical inventory (from V1.1 BOM, values confirmed 2026-06-20)

| Designator | Value | Role |
|---|---|---|
| R34 | 1 kΩ | GPIO43_TX → U9 `D` series protection |
| R35 | 10 kΩ | GPIO21 → Q6 base |
| R36 | 0 Ω | Q6 collector → DE+RE link |
| R38, R39 | 2.2 kΩ | **candidate fail-safe bias** on the A/B pair (A↑3V3 / B↓GND) — net not yet traced |
| R41, R42 | **10 Ω** | **A/B differential series protection** (with D6 ESD) — **NOT bias** |
| D6 | LRC399-04AT1G | ESD diode array on A/B |
| — | — | **No 120 Ω termination resistor exists in the BOM** |

> **Correction to ADR-001:** ADR-001 line 217 lists R41/R42 as "likely fail-safe bias —
> confirm in Phase 4 ADR." The BOM value (10 Ω) refutes that: 10 Ω is series protection,
> not bias. The fail-safe-bias role, if present on-board, is more likely R38/R39 (2.2 kΩ).
> Net connectivity of R38/R39 vs R41/R42 to be traced on the next schematic pass; the
> bench echo test empirically reveals idle-line behaviour regardless.

## Decision

### 1. UART + DE/RE control (firmware — definitive)

- **UART_NUM_1**, `TX=GPIO43`, `RX=GPIO44`, `RTS=GPIO21` (DE/RE), no CTS.
- **`UART_MODE_RS485_HALF_DUPLEX`** — the UART hardware asserts RTS (DE) before the first
  TX bit and deasserts it only **after the last stop bit** clears the shift register,
  giving deterministic, jitter-free turnaround that software GPIO toggling cannot match.
- **No RTS inversion** — `uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_INV_DISABLE)`. The
  default half-duplex RTS=DE is active-high: during TX, RTS → GPIO21 **HIGH** → U9 transmits;
  idle/RX → GPIO21 **LOW** → U9 receives. That matches the board's *measured* standard polarity.

Rationale for hardware mode over manual GPIO toggling: manual `gpio_set_level` around
`uart_write_bytes` + `uart_wait_tx_done` works but the deassert race (flipping to RX before
the last stop bit fully shifts out) truncates the final byte at high baud; the hardware
RS-485 mode eliminates that race.

### DE/RE polarity correction (rev2, 2026-06-20)

rev1 inverted RTS to follow ADR-001 EC-5a ("GPIO21 HIGH = receive"). On the bench the node
received **nothing** — the inversion idled U9 in *transmit*, so it never listened (and its
own driver contended with the partner, producing the garbled/empty captures during bring-up).
A toggle diagnostic (drive GPIO21 as a plain output, alternate HIGH/LOW while listening)
settled it: **GPIO21 LOW → clean RX of the 256-byte pattern; GPIO21 HIGH → nothing.** So
DE/RE is **standard** (HIGH=TX, LOW=RX). Removing the inversion fixed it: byte-identical echo
at 9600 **and** 115200. ADR-001 EC-5a is corrected hw-side; firmware no longer inverts RTS.

### 2. Bus electrical (bench + deployment)

- **Termination:** the board has **no on-board 120 Ω**. For the Phase 4 bench echo and for
  any real bus, a **120 Ω termination is added externally across A/B at each of the two bus
  ends** (standard RS-485). The bench test uses the USB-RS485 adapter's termination at one
  end + an external 120 Ω at the node end (or a short stub with one termination for the
  low-speed first pass).
- **Fail-safe bias:** confirm whether R38/R39 (2.2 kΩ) bias A→3V3 / B→GND. If on-board bias
  is absent or weak, idle-line indeterminacy can clock spurious RX bytes; the echo test's
  idle period is the detector. If needed, external bias is added at one bus end only.

### 3. Verification plan (Phase 4 smoke gate)

- Host `ctest`: pure-C `ring_buffer` (RX staging primitive) unit tests — first real host tests.
- HIL echo: 256-byte pattern looped at **9600 8N1** then **115200 8N1** → byte-identical echo,
  0 framing errors, idle line quiet (no spurious RX).
- Saleae Logic 2 on GPIO21 + A/B: DE-assert→first-bit and last-bit→DE-release within TP8485E
  spec; no bus contention; turnaround measured and recorded.

## Consequences

- Firmware is portable across baud rates via Kconfig/define; no hand-tuned turnaround delays.
- DE/RE polarity is load-bearing and now matches measured hardware: the single
  `uart_set_line_inverse(..., UART_SIGNAL_INV_DISABLE)` call is the one place to change if a
  future board revision alters the Q6 stage.
- External termination/bias is an operational requirement (documented in RUNBOOK bench setup);
  a deployed node at a bus end needs the 120 Ω fitted.

## Alternatives considered

- **Manual GPIO21 toggling around TX** — rejected: deassert race truncates the last byte at
  115200 unless padded with a guard delay that wastes bus time and is fragile across baud.
- **Inverted RTS (rev1, following ADR-001 EC-5a)** — rejected: held U9 in the wrong direction
  (idled in transmit), so the node never received; bench-proven wrong and reverted in rev2.
- **UART0 instead of UART1** — UART0 is free (console moved to USB-Serial-JTAG), but UART1 is
  used to keep UART0 conventionally reserved and avoid any ROM/bootloader coupling.

## References
- Hardware: ADR-001-pin-map (adr-001-locked, hw-repo a0b002c) — EC-5a DE/RE polarity, pin map.
- V1.1 BOM (`BOM_Board1_Schematic1_2026-05-01.xlsx`) — resistor values (confirmed 2026-06-20).
- TP8485E-SR datasheet (3PEAK) — DE/RE timing, fail-safe, termination guidance.
- ESP-IDF v5.5.4 UART driver — `uart_set_mode`, `uart_set_line_inverse`, `uart_set_pin`.

## Sign-off
Signed-off-by: <pending> · 2026-06-20
