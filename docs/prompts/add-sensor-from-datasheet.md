# Add a new field sensor (real reader + simulator + payload) from datasheet + proven Arduino code

> **Team prompt.** Paste the section below into Claude Code **running inside this repo**, and attach
> (1) the device datasheet PDF and (2) the proven Arduino code that already reads the device. It
> generates the matched set we use for every field device: a real Modbus reader, a deterministic
> simulator, and the compact LoRaWAN payload (+ ChirpStack decoder) — in this project's conventions.
>
> The most important line is the **GROUND-TRUTH RULE**: the proven Arduino code wins on anything that
> touches the wire (register addresses, word order). It bakes in the Phase 6 lesson — a meter
> documented "ABCD" actually transmitted "CDAB"; only the raw bytes from working code revealed it.

---

You are working in the `rak3112-rs485-node` ESP-IDF firmware. We support each field device as a
matched set: a **real Modbus reader**, a **deterministic simulator**, and a **compact LoRaWAN
payload** (+ a ChirpStack decoder). Add support for a new device, matching our existing conventions.

## Inputs (attached)
1. **`<device>` datasheet** (PDF) — authority for measurand list, value ranges/units, resolution.
2. **Proven Arduino code** that already reads this device correctly over Modbus/RS-485 — authority
   for the wire-level details (register addresses, function code, data types, **byte/word order**,
   scaling, and bus params: baud / parity / stop / unit ID).

## GROUND-TRUTH RULE (read this twice)
The **Arduino code wins** on anything that touches the wire: register addresses, FC03 vs FC04,
data type (u16/i16/u32/float32), **word order (ABCD/CDAB/BADC/DCBA)**, scale/offset, baud/parity/unit.
The datasheet provides value ranges/units and is a cross-check only. **If they disagree, follow the
Arduino code and flag the conflict explicitly.** (We have been burned: a meter documented "ABCD"
actually transmitted "CDAB" — only the raw bytes from working code revealed it. Phase 6.)

## First, study our patterns (read these files)
- `firmware/components/meter/meter.c` and `.../include/meter.h` — the `<device>_sample_t` struct,
  `meter_read_<device>()` (real) and `meter_sim_<device>()` (sim). **Match this style exactly.**
  - Reads use `modbus_master_read(port, unit, FC, reg, qty, 500 /*ms*/, regs, &exc)` returning
    `MODBUS_OK`; float32 spans 2 regs decoded with `modbus_regs_to_f32(regs, MODBUS_WORD_ORDER_ABCD|CDAB)`.
  - `meter_sim_mfm384()` is the style exemplar for the simulator (deterministic, `tick`-driven,
    smooth `sin()` variation around nominal, counters accumulate).
- `firmware/components/payload/` + `include/telemetry.h` — ADR-005 compact payload: 3-byte header
  `[schema=0x01][device-type][flags]`; flags `TELEMETRY_FLAG_SIMULATED` / `TELEMETRY_FLAG_STALE`;
  fixed-point body; must fit ≤ **53 bytes** (AS923 DR3). `payload_encode_<device>(&s, flags, buf, n)`.
- `tools/chirpstack_mfm384_decoder.js` — the JS codec; new device = new `device` byte + decode branch.
- `docs/adr/ADR-005-payload-schema.md` — payload rationale.

## Step 1 — Extract & reconcile (output a table first)
From the Arduino code (primary) and datasheet (cross-check), produce one row per measurand:

| Measurand | Reg (dec / 4xxxx·3xxxx) | FC | Type | Word order | Scale·offset | Unit | Nominal | Min | Max | Source / conflict |
|---|---|---|---|---|---|---|---|---|---|---|

Also state the bus config (baud, parity, stop, unit ID) from the Arduino code, and **explicitly list
any Arduino-vs-datasheet discrepancies** with which one you chose and why.

## Step 2 — Generate the code (in our conventions)
1. **`<device>_sample_t`** in `meter.h` — engineering units (floats), one field per measurand we uplink.
2. **`esp_err_t meter_read_<device>(uart_port_t port, uint8_t unit, <device>_sample_t *out)`** —
   mirror the Arduino register addresses / FC / word order exactly; return `ESP_FAIL` on any
   `modbus_master_read` != `MODBUS_OK`. Comment each register with the datasheet §, the proven
   address, and "source: <arduino file>".
3. **`void meter_sim_<device>(uint32_t tick, <device>_sample_t *out)`** — pure, no I/O, deterministic
   from `tick`; each field a smooth function bounded to the datasheet's plausible range (gentle
   sinusoid around nominal; energy/counter fields accumulate via a `static`). Same shape as
   `meter_sim_mfm384`.

## Step 3 — Payload (ADR-005)
Propose the compact body: a **new device-type byte**, and per field a fixed-point type (u16/i16/u32)
+ named scale constant chosen to preserve the device's resolution while fitting ≤ 53 B with the
header. Provide `payload_encode_<device>()` (pure, host-testable) and the matching **ChirpStack JS
decoder branch** (mirror the scales).

## Conventions (must hold)
- C11, `stdint.h` types only; designated initializers; named scale constants (no magic numbers).
- Comments explain **why** and cite datasheet sections; `clang-format` (LLVM 18) clean.
- `sim` and `encode` are pure/host-testable; set `TELEMETRY_FLAG_SIMULATED` in sim, `TELEMETRY_FLAG_STALE`
  on a failed read. No floats in ISRs (these aren't ISR paths — fine).
- Don't invent register addresses or word order — derive them from the Arduino code; if the code is
  ambiguous, say so rather than guessing.

## Deliverables
1. The reconciliation **table** + a short **"assumptions & flagged discrepancies"** list.
2. The three code blocks (struct / `meter_read_*` / `meter_sim_*`).
3. The payload **encoder** + **decoder** branch.
4. **2–3 host-test example values** (input regs → decoded sample → payload hex → re-decoded JSON),
   so the round-trip can be checked without hardware.
