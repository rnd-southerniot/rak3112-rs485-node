# Careflow RS-485 Scanner / Profiling Station (Pi 5)

Turns an **unknown** RS-485 (Modbus RTU) device into a reusable Careflow **device profile**, then
hands it straight to the existing profile-driven flash/provision/decoder pipeline — so the same model
onboards instantly next time.

It is an *extension* of the Careflow product, not a new pipeline. The reusable package remains a single
`device-profiles/profiles/<model>.json`; the blob, ChirpStack decoder, and CRM catalog are regenerated
from it by the committed generators. This station only adds the missing **upstream** step: probe an
unknown device → propose a profile (operator confirms) → package + verify.

## How it works

```
operator ─▶ Pi kiosk (FastAPI + browser)
                │  drives, over USB, the node's scan-* console
                ▼
        Careflow node  ──RS-485──▶  unknown Modbus device
                │  (Modbus master; sweeps registers)
                ▼
   sweep → infer (structure only) → operator labels → emit profile.json
                │  reuse validate_profiles / profile_to_blob / _to_decoder / _to_catalog
                ▼
     flash · provision (creds + profile blob) · install decoder · verify decoded uplink (dev ChirpStack)
```

- **Probe through the node** (not a separate USB-RS485 dongle): the node is the Modbus master, so the
  profile is validated on the exact hardware it will run on. The node exposes read-only `scan-*`
  commands (`CONFIG_APP_SCAN_CONSOLE`, default off — a build variant).
- **Human-in-the-loop:** inference proposes *structure* (register, type, word order) with confidence;
  it never guesses *meaning*. The operator names each measurand before packaging.
- **Dev-only:** `CS_BASE` is pinned to the dev ChirpStack (`10.10.8.140`); a `DEV_GUARD` refuses any
  other base. Production is never touched (two-stack rule).

## Run

```bash
cd pi-scanner
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/uvicorn app.main:app --host 127.0.0.1 --port 8080
# open http://127.0.0.1:8080  → pick the "mock" port for a hardware-free walkthrough
```

Tests (mock node — no hardware): `cd pi-scanner && pytest` (also runs in CI, job `pi-scanner-tests`).

Kiosk / boot-to-app deployment: see [`deploy/`](deploy/).

## Module map (`app/`)

| File | Role |
|---|---|
| `node_console.py` | drive the node's `scan-*` console over USB-Serial-JTAG (pyserial); pure line parsers |
| `sweep_engine.py` | `discover_units` (baud×parity×id) + `sweep_registers` (FC03/FC04, exact present/absent map) |
| `inference.py` | propose types + word order (float32 plausibility); **semantics left blank** |
| `profile_emitter.py` | `type_byte` alloc, ADR-005 payload packing (≤53 B), **shell out** to the committed generators |
| `onboarding.py` | flash · provision · install decoder · verify uplink — glue over the existing service/tools |
| `orchestrator.py` | async state machine (idle→searching→preparing→flashing→probing→profiling→success/failed) |
| `main.py` | FastAPI REST + `/ws` + static `web/` kiosk |
| `mock_backend.py` | in-app MFM384 mock + mock onboarding for the hardware-free "mock" port |

## Firmware scanner variant

Flash the node with `CONFIG_APP_SCAN_CONSOLE=y` so the `scan-*` commands register (in the unprovisioned
idle branch — UART1 free, field path untouched):

```bash
cd firmware
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.scanner" build flash
```

## Phase gates

| Phase | What | Gate |
|---|---|---|
| P0 | firmware `scan-*` console | builds both variants; `scan-*` well-formed vs a known device; `prov-show` unchanged |
| P1 | node console + sweep engine | discover finds 9600/N/unit; sweep matches MFM384 present set |
| P2 | inference | word order CDAB + types match the committed profile ≥90%; semantics blank |
| P3 | emitter | `type_byte=5`, ≤53 B, `validate_profiles.py` passes, blob serializes |
| P4 | orchestrator + UI | mock dry-run walks every state; candidate editable; meter blocks Confirm >53 B |
| P5 | onboarding | **bench:** unknown device → flash → provision → decoder → **decoded uplink on dev ChirpStack** |

**P5 bench gate (run with hardware + a genuinely unknown RS-485 device):**
1. Flash the scanner variant; connect the device to CN1.
2. In the kiosk, Start on the node's port → the sweep proposes a candidate.
3. Label the measurands you want, keep the payload ≤53 B, Confirm.
4. Confirm a decoded uplink lands on dev ChirpStack (`10.10.8.140`) with the new `device_byte`, and
   that `device-profiles/profiles/<id>.json` is now in the repo (blob/decoder/catalog regenerated).

Production ChirpStack and the deployed field firmware are never touched.
