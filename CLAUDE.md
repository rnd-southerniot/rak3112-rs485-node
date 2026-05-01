# rak3112-rs485-node — Execution Contract (firmware)

> **Status:** rev4 — Phase 1 Attempt 3 (post-PIO-drop). 2026-05-01.
> **Inherits from:** `~/.claude/CLAUDE.md` (global, M5 Pro profile).
> **Owner:** rnd-southerniot · `arif-southern`
> **Repo path:** `~/Developer/projects/firmware/rak3112-rs485-node/`
> **Mode:** **split-repo** — this is the *firmware* repo only. Hardware lives separately (see §0.1).

---

## 0. Identity

| Item | Value |
|---|---|
| Product | RS-485 ⇄ LoRaWAN AS923 industrial sensor/gateway node |
| Module | RAK3112-9-SM-I (RAKwireless WisDuo) |
| Silicon | **ESP32-S3** (Xtensa LX7 dual-core @ 240 MHz) + **Semtech SX1262** |
| Memory | 16 MB flash · 8 MB PSRAM (mode TBD — see OQ-2; PSRAM **disabled** in Phase 1) |
| RF band | 902–928 MHz hardware variant `-9`; firmware-selectable region; **target = AS923 (Bangladesh)** |
| Field bus | RS-485 (3PEAK TP8485E-SR, half-duplex, ≤250 kbps, ±18 kV ESD) |
| Aux UART | RS-232 (MAX3232EEAE+T) — population status TBD (OQ-1) |
| Power | DC barrel `DC1` (DC-005) **or** 2-pin terminal `P1` (WJ500V-5.08-2P) → Richtek RT6160AWSC buck-boost (2.2–5.5 V → 3 A @ 2.2 MHz) |
| Indicator | 1× WS2812 addressable RGB |
| User input | `SW1` (RESET), `SW2` (BOOT) — both TSA343G00D-250J2 |
| Field connector | `CN1` WJ2EDGR-5.08-3P (RS-485 A / B / GND) |
| Aux header | `H1` PZ254V-11-02P 1×2 (purpose TBD — likely UART debug, confirm in Phase 2) |

> **Silicon evidence (locked Phase 0):** RAK3112 schematic symbol exposes `GPIO0/1/2/4/9–14/17/18/21/33–46`, native `USB_D±`, `EN`, `BOOT`, `UART0_RX`, `UART1_TX`. Pin-numbering range and native USB are unambiguous ESP32-S3 fingerprints; ASR6601 / STM32WL ruled out.

### 0.1. Repository split (E1 decision — explicit, not inferred)

| Repo | Path (intended) | Owner | Scope |
|---|---|---|---|
| **Firmware (this repo)** | `~/Developer/projects/firmware/rak3112-rs485-node/` | `rnd-southerniot` | ESP-IDF source, build, CI, firmware ADRs, host tests |
| **Hardware** | `~/Developer/projects/pcb-design/rak3112-rs485-node-hw/` *(planned — not yet created)* | `rnd-southerniot` | Schematic, PCB, BOM, hardware ADRs |

- Cross-reference is **one-way**: firmware pins the hardware revision via `HARDWARE_REV.md` at the firmware-repo root, recording `{hardware_repo_url, tag, commit_sha, date_pinned, fw_phase_when_pinned}`.
- The `hardware/schematic/<rev>/` tree inside *this* firmware repo is a **read-only snapshot** for convenience (so the firmware repo builds context-complete on a clean checkout). Edits are forbidden here; canonical edits happen in the hardware repo and a new snapshot is dropped on each tagged hardware revision.
- A separate hardware-side `CLAUDE.md` is expected in the hardware repo. This firmware contract does **not** claim to inherit decisions from it; any cross-discipline decision (pin-mux, strap-pin assignments, RS-485 termination) is recorded as a firmware-side ADR that *cites* the corresponding hardware-side ADR by ID.

---

## 1. Toolchain (locked, version-pinned)

| Concern | Choice | Pinned version |
|---|---|---|
| **Single canonical build path** | ESP-IDF (CMake/Ninja via `idf.py`) — local **and** CI | **v5.5.4** (exact patch tag — see footnote ¹) |
| Target | `esp32s3` |
| Console | USB-CDC native (no external USB-UART bridge for prototype) |
| Flashing | `idf.py flash` over USB-CDC; `esptool.py` for recovery; `esptool erase_flash` **gated** (see §3) |
| Host tests | `cmake + ctest` (native) — first real tests land Phase 4 |
| Lint/format | `clang-format` (LLVM **18.x** via `brew install llvm@18`; **keg-only** — explicit path `/opt/homebrew/opt/llvm@18/bin/clang-format` required, do **not** rely on `$PATH`) — `.clang-format` checked in |
| Secrets scan | `gitleaks` (**v8.30.x**, currently v8.30.1) — pre-commit + CI |
| Pre-commit | `pre-commit` (**≥ 3.x** via `pipx`) — hooks: gitleaks, clang-format, end-of-file-fixer, trailing-whitespace |
| Python | `uv` for any tooling envs (per global §5) |

Floating `latest` is forbidden under global §13. CI workflow must reference each pinned version explicitly; any version bump is a separate PR with its own ADR.

> ¹ ESP-IDF directory `~/esp/esp-idf-v5.5.4/` is pinned to tag `v5.5.4`. Do **not** `git pull` it without first bumping this contract pin and re-running the Phase 1 entry-criteria block. The dev branch at `~/esp/esp-idf/` (currently `v6.1-dev-3824-g484e56869c`) is unrelated to this project and must not be sourced for `rak3112-rs485-node` builds.

> ² **PlatformIO was considered and dropped at Phase 1 Attempt 2 (2026-05-01).** The original rev3 dual-build-path plan paired ESP-IDF v5.5.4 with `platform = espressif32 @ ~6.7.0`, which silently bundles ESP-IDF v5.2.1 + CMake 3.16.4. ESP-IDF v5.5 raised the CMake floor to 3.20, so the second build path could not satisfy the project's `cmake_minimum_required`. After honest examination, the case for keeping PlatformIO at all turned out to be weak: hermetic CI is covered by `espressif/esp-idf-ci-action@v1`; component pinning by `idf_component.yml`; VS Code workflow by Espressif's first-party extension; "belt-and-suspenders" only catches drift if the two paths share a toolchain version, which they didn't. Single canonical build path is the more honest and lower-maintenance choice. Reconsider only via ADR if a future need surfaces. See `docs/RUNBOOK.md` Attempt 2 entry and "Pin discipline" lesson.

Environment assumed (from global `~/.claude/CLAUDE.md` §5–6):
ESP-IDF sourced explicitly via `. ~/esp/esp-idf-v5.5.4/export.sh` (do **not** use the `get_idf` alias if it points at `~/esp/esp-idf`); macOS arm64 / Homebrew `/opt/homebrew`.

---

## 2. Repository layout

```
rak3112-rs485-node/                       # firmware repo
├── CLAUDE.md                             # this contract
├── README.md                             # human entry point
├── HARDWARE_REV.md                       # pins HW repo SHA + tag
├── .gitignore
├── .gitleaks.toml
├── .clang-format
├── .pre-commit-config.yaml
├── .env.example                          # provisioning placeholders only
├── hardware/                             # read-only snapshot of HW repo
│   ├── README.md                         # "canonical source: rnd-southerniot/rak3112-rs485-node-hw"
│   └── schematic/
│       └── v1.0/                         # versioned snapshots; v1.1/ added on next HW tag
│           ├── 42686fd3-…pdf
│           └── ProPrj_RAK3312+ RS485_2026-05-01.epro   # filename retained for provenance — see §8 note
├── firmware/
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults                # single source of truth for ESP-IDF config
│   ├── platformio.ini
│   ├── partitions.csv                    # added in Phase 7 (OTA); not present in Phase 1
│   ├── main/
│   │   ├── CMakeLists.txt
│   │   └── app_main.c
│   └── components/                       # vendored / project-local components (empty in Phase 1)
├── docs/
│   ├── ARCHITECTURE.md                   # Phase 2+
│   ├── RUNBOOK.md                        # Phase 3+
│   └── adr/                              # firmware-side ADRs only
├── tests/
│   └── host/
│       └── CMakeLists.txt                # presence-only placeholder in Phase 1; real tests Phase 4+
└── .github/workflows/
    └── ci.yml
```

---

## 3. Guardrails (hard rules)

1. **No flash, no erase without an active phase gate.** `idf.py flash`, `idf.py erase-flash`, `esptool erase_flash` only inside an explicit phase whose smoke test calls for it. Confirm board ID before each flash; log to `docs/RUNBOOK.md`.

2. **Strap-pin discipline (ESP32-S3-specific, per ESP32-S3 datasheet §2.4 *Strapping Pins*).** The following are hardware-critical and must **never** be driven by application code or routed off-module without explicit hardware ADR sign-off:

   | Strap | Function | Default state on RAK3112 | Off-limits because |
   |---|---|---|---|
   | `GPIO0` | Boot mode select (1 = SPI boot, 0 = download via UART/USB) | pulled high externally for normal boot; `SW2` pulls low for download mode | wrong level at reset → device won't boot |
   | `GPIO3` | JTAG signal source (1 = USB-JTAG, 0 = pad-JTAG) | module-internal | misconfigure → no debug |
   | `GPIO45` | VDD_SPI voltage select (1 = 1.8 V, 0 = 3.3 V) for in-package flash/PSRAM | **module-internal — wrong level at reset will brick boot** | flash/PSRAM voltage mismatch destroys access |
   | `GPIO46` | ROM print at boot (1 = print disabled, 0 = enabled) | module-internal | not destructive but pollutes UART0 if misconfigured |

   **Rule:** application code does not configure these pins. Phase 2 ADR-001 confirms whether RAK3112 routes any of GPIO45/46 to the module edge; if so, the schematic must isolate them. The pin-map ADR will assign any board-level "HW_REV" / config-strap function to GPIO38/39 or another non-strap pin, **not** GPIO45/46 — this is the firmware-side preference; final call belongs to the hardware ADR.

3. **Region lock.** LoRaWAN region selected via Kconfig (`CONFIG_LORAWAN_REGION_AS923`), never hardcoded. AS923 sub-band confirmed against ChirpStack region config (OQ-4) before any join attempt.

4. **No secrets in tree.** AppEUI/JoinEUI / AppKey / DevEUI / WiFi PSK / API tokens via `.env` (gitignored) → loaded into NVS at provisioning time. `gitleaks` runs pre-commit and in CI; any finding fails the build.

5. **No `:latest` Docker tags, no `curl | bash` installs, no Node.js in the firmware path** (per global §13).

6. **Single canonical build path.** ESP-IDF v5.5.4 via `idf.py` is the only supported build path. Local development and CI use the same ESP-IDF version, same target (`esp32s3`), same `sdkconfig.defaults`. **Reproducibility guarantee:** a clean clone of `~/esp/esp-idf-v5.5.4` at tag `v5.5.4` plus a clean clone of this repo at any `phase-N-green` tag produces *functionally identical* `firmware.bin` (modulo embedded timestamp / `git describe` strings). No second framework — no PlatformIO, no Arduino-ESP32 BSP. If a contributor wants a different framework, the burden of proof is on them to show via ADR that the value justifies the maintenance cost of a dual path. (Rationale: see §1 footnote ², `docs/RUNBOOK.md` Attempt 2, and "Pin discipline" lesson.)

7. **Power sequencing on bench.** USB-CDC powers logic; do **not** simultaneously connect DC barrel jack `DC1` and bench supply on `P1` without verifying RT6160 input isolation. Bench checklist in `RUNBOOK.md` (Phase 3).

8. **RS-485 termination & bias.** 120 Ω termination + fail-safe bias resistor values to be confirmed against TP8485E datasheet and bus topology in Phase 4 ADR.

9. **Gate-execution discipline.** Smoke-gate commands MUST be executed *bare* — no `| tee`, `| tail`, `| head`, `| grep`, `| awk`, `| sed`, or similar pipes added at runtime. Pipes mask the upstream exit code (the pipe's exit is the *last* command's exit unless `set -o pipefail` is in force). The gate is the exit code; the operator (human or AI) does not get to wrap it. If forensic logging is needed, capture it **outside** the gate via the on-failure procedure in `docs/RUNBOOK.md` — not inline. Reference incident: Phase 1 Attempt 1 (2026-05-01) — `pio run` failure was masked by an operator-added `| tee /tmp/… | tail -3` even though the contract command was clean. See `docs/RUNBOOK.md` "Gate design principles".

---

## 4. Branches, commits, CI

- Branch naming: `feat/<slug>`, `fix/<issue>`, `phase/<id>` (e.g., `phase/p1-scaffold`)
- Conventional Commits with phase ID in body:
  ```
  feat(scaffold): ESP-IDF + PlatformIO baseline

  P1-01. First green dual-build for esp32s3.
  ```
- Pre-commit (local, before push): gitleaks · clang-format · end-of-file-fixer · trailing-whitespace · check-yaml · check-merge-conflict.
- CI matrix (`.github/workflows/ci.yml`), 4 jobs, all version-pinned per §1:
  - `pre-commit` — `pre-commit run --all-files`
  - `idf-build` — ESP-IDF **v5.5.4**, `idf.py build`, artifact = `build/*.bin`
  - `gitleaks` — `gitleaks detect --no-banner --redact` (defence-in-depth even though pre-commit also runs it)
  - `host-tests` — layout presence-check now; real `ctest` activation lands in Phase 4
- Green CI is a merge gate to `main`. No exceptions.

---

## 5. Phase plan (gated)

### Phase 0 — Discovery · **DONE 2026-05-01**

Confirmed silicon (ESP32-S3 + SX1262), BOM (5 ICs / 2 tact switches / 3 input connectors), toolchain (ESP-IDF v5.3.1 + PlatformIO platform-espressif32 ~6.7.0), repo split (firmware here, hardware separate). See §0–§2.

---

### Phase 1 — Scaffold · **NEXT (pending review of this rev2)**

**Goal.** Stand up the empty firmware repo with a hello-world that builds *green* under ESP-IDF v5.5.4, with version-pinned CI, pre-commit secrets scanning, and a placeholder `HARDWARE_REV.md` for cross-repo coupling.

**Entry criteria.**
- ESP-IDF v5.5.4 sourced via `. ~/esp/esp-idf-v5.5.4/export.sh`; `idf.py --version` reports a tag containing `v5.5.4`
- `gitleaks version` reports v8.30.x (currently v8.30.1)
- `/opt/homebrew/opt/llvm@18/bin/clang-format --version` reports LLVM 18.x (`brew install llvm@18` — keg-only; explicit path required, do not rely on `$PATH`)
- `pre-commit --version` ≥ 3.x (`pipx install pre-commit`)
- User has reviewed and approved this `CLAUDE.md` rev4

**Pre-scaffold isolation (per OQ-7 disposition (a)).**

```bash
cd ~/esp/esp-idf-v5.5.4/
git stash push -u -m "rak3112-rs485-node Phase 1 isolation 2026-05-01"
git stash list                            # expect: stash@{0} with the message above
git status                                # expect: clean working tree
. export.sh && idf.py --version           # expect: ESP-IDF v5.5.4 (no -dirty suffix)
```

If `idf.py --version` still shows `-dirty`, **stop** — investigate before scaffolding (untracked files not stashed, etc.).

**Steps.**

1. `git init`; first commit `chore(repo): initialize`. Files in this commit:
   - `CLAUDE.md`, `README.md`, `HARDWARE_REV.md` (placeholder — `pinned_sha: TBD`, `pinned_tag: TBD`, `status: hardware repo not yet created`)
   - `.gitignore` (ESP-IDF + PlatformIO build dirs, macOS clutter, `.env`, `*.bin`, `*.elf`, `*.map`, `build/`, `.pio/`, `.idea/`, `.vscode/` minus `settings.json`)
   - `.gitleaks.toml`, `.clang-format` (LLVM style + project tweaks), `.env.example` (per Step 1a below)

   **1a. `.env.example` content:**
   ```dotenv
   # .env.example — provisioning placeholders. Never commit real values.
   # Loaded into NVS at provisioning time (Phase 7).

   # Wi-Fi (optional, if used for provisioning portal)
   WIFI_SSID=<REPLACE_ME>
   WIFI_PASS=<REPLACE_ME>

   # LoRaWAN OTAA (Phase 5)
   LORAWAN_DEVEUI=<REPLACE_ME>      # 16 hex chars
   LORAWAN_JOINEUI=<REPLACE_ME>     # 16 hex chars (was AppEUI in 1.0.x)
   LORAWAN_APPKEY=<REPLACE_ME>      # 32 hex chars

   # RS-485 / Modbus (Phase 6)
   MODBUS_BAUD=9600
   MODBUS_PARITY=N
   MODBUS_STOP=1
   MODBUS_SLAVE_ID=1

   # Diagnostics
   LOG_LEVEL=INFO
   ```

2. Create `.pre-commit-config.yaml` with gitleaks + clang-format + end-of-file-fixer + trailing-whitespace hooks; run `pre-commit install` to register the git hook. Commit `chore(repo): pre-commit hooks`.

3. Copy `RAK3112 + RS485 P2P Node V1.1 Schematic.pdf` and `RAK3112 + RS485 P2P Node V1.1.epro` from `~/Developer/projects/pcb-design/rs485-node/` into `hardware/schematic/v1.1/`. Add `hardware/README.md` and `hardware/schematic/README.md` declaring these are read-only reference snapshots; canonical source is `rnd-southerniot/rak3112-rs485-node-hw` (when created). Commit `chore(hardware): import schematic v1.1 snapshot`.

4. Create `firmware/main/app_main.c` — heartbeat printf only, no GPIO, no peripherals, no PSRAM dependency:
   ```c
   #include <stdio.h>
   #include "freertos/FreeRTOS.h"
   #include "freertos/task.h"
   void app_main(void) {
       for (uint32_t i = 0;; ++i) {
           printf("rak3112-rs485-node alive: tick=%lu\n", (unsigned long)i);
           vTaskDelay(pdMS_TO_TICKS(1000));
       }
   }
   ```

5. Create `firmware/main/CMakeLists.txt` and `firmware/CMakeLists.txt`.

6. Create `firmware/sdkconfig.defaults`:
   ```
   CONFIG_IDF_TARGET="esp32s3"
   CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
   CONFIG_SPIRAM=n                       # Phase 1: PSRAM disabled — heartbeat doesn't need it.
                                         # Phase 2 ADR-001: enable + select Quad/Octal mode after datasheet check (OQ-2).
   CONFIG_ESP_CONSOLE_USB_CDC=y
   CONFIG_BOOTLOADER_LOG_LEVEL_INFO=y
   ```

7. *(Removed in rev4 — was "create platformio.ini". PlatformIO dropped per §1 footnote ² and §3 #6. The single-build-path direction obsoletes this step.)*

8. Create `tests/host/CMakeLists.txt` — placeholder, no targets defined yet:
   ```cmake
   cmake_minimum_required(VERSION 3.20)
   project(rak3112_rs485_node_host_tests C)
   enable_testing()
   # Real tests land in Phase 4 (RS-485 framing) and Phase 6 (Modbus parser).
   ```

9. Create `.github/workflows/ci.yml` with the five jobs from §4. Pin every tool version. Cache ESP-IDF + PlatformIO toolchains.

10. Commit each logical step with a Conventional Commit + phase ID (`P1-01` … `P1-09`).

11. Push branch `phase/p1-scaffold`. Open PR for self-review.

**Smoke test (PASS/FAIL gate).** Run *exactly* this on M5 Pro; every command must succeed. Commands run **bare** — no piping (per Guardrail §3 #9). Forensic logs captured per `docs/RUNBOOK.md` on-failure procedure, never inline.

```bash
cd ~/Developer/projects/firmware/rak3112-rs485-node

# 1. Pre-commit on the whole tree
pre-commit run --all-files                # expect: exit 0, all hooks Passed

# 2. ESP-IDF build (use the v5.5.4 export.sh explicitly; not the dev-branch ~/esp/esp-idf)
. ~/esp/esp-idf-v5.5.4/export.sh
cd firmware
idf.py set-target esp32s3
idf.py build                              # expect: exit 0, "Project build complete.", build/*.bin produced
cd ..

# 3. Secret scan (defence-in-depth — also covered by pre-commit)
gitleaks detect --no-banner --redact      # expect: exit 0, "no leaks found"

# 4. Host-test scaffolding present (no tests to run yet — gate on layout, not execution)
test -d tests/host && test -f tests/host/CMakeLists.txt   # expect: exit 0
```

**On PASS:**
1. Tag `phase-1-scaffold-green` with annotation containing exact toolchain versions (`idf.py --version`, `pio --version`, `gitleaks version`, `clang-format --version`, `pre-commit --version`).
2. Update §6 State block with PASS entry.
3. **Restore ESP-IDF stash** (post-scaffold restoration):
   ```bash
   cd ~/esp/esp-idf-v5.5.4/
   git stash pop                                  # expect: clean apply, no conflicts
   git status --short                             # expect: M esp_lcd_panel_io_parl.c, M usb_types_ch9.h
   . export.sh && idf.py --version                # expect: ESP-IDF v5.5.4-dirty (back to baseline)
   ```
   If `git stash pop` reports conflicts or `git status` shows different files, **stop** and report — do not advance to Phase 2.
4. Stop. Do not advance to Phase 2 without user review of artefacts.

**On FAIL:**
1. **Restore ESP-IDF stash FIRST** (don't leave the shared dev environment perturbed). Same `git stash pop` + verify as above.
2. Capture full build log to `docs/RUNBOOK.md` under "Phase 1 attempts".
3. Do **not** advance; do **not** flash hardware. Fix in a `fix/p1-*` branch and re-run from the pre-scaffold isolation step.

**Rollback:** `rm -rf ~/Developer/projects/firmware/rak3112-rs485-node/` (entire dir is reversible). The ESP-IDF stash restoration above ensures the shared `~/esp/esp-idf-v5.5.4/` is unchanged from pre-Phase-1 state.

---

### Phase 2 — HW bring-up prep · **scoped, not yet detailed**

**Entry prereq:** **OQ-7 resolved** — ESP-IDF patches vendored into `firmware/esp-idf-patches/` with attribution and intent documentation; build script applies them; CI mirrors. Verify by checking that a fresh clone of ESP-IDF v5.5.4 + applied patches produces the same patch-applied behaviour.

Render the schematic PDF (poppler installed), extract every ESP32-S3 pin / net assignment from the `.esch` files, write **ADR-001 (pin map)** covering: RS-485 TX/RX/DE/RE, RS-232 TX/RX, WS2812 data, button GPIO, LED enable, RT6160 EN, antenna IPEX, BOOT/RESET straps. Resolve **OQ-1** (RS-232 keep/DNP/console) and **OQ-2** (PSRAM enable + Quad vs Octal). Cross-reference the hardware-side ADR by ID. User signs off ADR-001 before Phase 3 flashes anything.

### Phase 3 — First flash + USB-CDC console · **scoped**

Flash hello-world to the actual board over USB-CDC. Verify boot, log output at 115200, no bootloop, no brown-out, current draw within RT6160 spec. Bench checklist + hardware-safety pre-flight in `RUNBOOK.md`.

### Phase 4 — RS-485 echo + DE/RE timing · **scoped**

Drive TP8485E from UART (per ADR-001), echo bytes back to a USB-RS485 adapter at 9600 8N1 then 115200. Characterise DE/RE turnaround on Saleae Logic 2. ADR-002 = bus electrical (termination, bias, fail-safe). Phase 4 also lands the **sdkconfig ↔ platformio.ini parity check** (OQ-6) as a CI step, plus the first real `ctest`-driven host unit tests for framing.

### Phase 5 — LoRaWAN OTAA join (AS923) · **scoped**

Pick stack (RadioLib vs LMIC vs LoRaMac-node port — OQ-3). OTAA join against the CRM ChirpStack at `10.10.8.140`. Confirm device shows up + uplink frame. ADR-003 = stack choice; ADR-004 = AS923 sub-band selection (OQ-4).

### Phase 6 — Field protocol (Modbus RTU master, configurable) · **scoped**

Modbus RTU master role, register set defined by JSON config in NVS, sample at configurable interval, payload-encode for LoRaWAN uplink. ADR-005 = payload schema.

### Phase 7 — Production hardening · **scoped**

Light-sleep between samples, task watchdog, brownout recovery, OTA-DFU (`partitions.csv` + dual-app slot), provisioning workflow. Current-draw target TBD.

---

## 6. State block (append-only)

| Phase | Date | Outcome | Notes |
|---|---|---|---|
| 0 — Discovery | 2026-05-01 | PASS | Silicon = ESP32-S3 + SX1262 (verified from `.esym` pin labels). BOM = 5 ICs, 2 tact switches, DC1+P1+H1 input connectors. Toolchain (rev3) = ESP-IDF v5.5.4 + PlatformIO platform-espressif32 ~6.7.0. Decision: split-repo. |
| 1 — Scaffold (Attempt 1) | 2026-05-01 | **FAIL** | Smoke gate step 3 (`pio run`): "Missing the 'src' folder with project sources". Operator-side `\| tee \| tail` masked exit code initially. ESP-IDF build (step 2) PASS with binary 183 328 B. ESP-IDF stash restored cleanly. See `docs/RUNBOOK.md` Attempt 1. |
| 1 — Scaffold (Attempt 2) | 2026-05-01 | **FAIL** | Branch `fix/p1-pio-srcdir`. P1-FIX-1 (src_dir=main + Guardrail #9) applied. Smoke gate step 3 (`pio run`) bare: `CMake Error … 3.20 or higher is required. You are running version 3.16.4`. Root cause: rev3 pinned ESP-IDF v5.5.4 + platform-espressif32 ~6.7.0 jointly inconsistent (PIO bundles ESP-IDF v5.2.1 + CMake 3.16). ESP-IDF build PASS, byte-identical to Attempt 1 binary. ESP-IDF stash restored. See `docs/RUNBOOK.md` Attempt 2 + Pin discipline lesson. |
| 1 — Scaffold (Attempt 3) | 2026-05-01 | **PASS · signed off** | Branch `fix/p1-drop-platformio`. rev4 contract: PlatformIO dropped entirely; single canonical build path (ESP-IDF v5.5.4); 4-step smoke gate. All four steps green: pre-commit (6 hooks), `idf.py build` (binary `rak3112_rs485_node.bin` 183 328 B, `IDF_VER=v5.5.4` clean, SHA-256 `a2f3d6044f9458aa38a492cad531c53071b2f829a33f5fd2635db72271fe5116`), gitleaks (11 commits / 243 KB / no leaks), layout check. ESP-IDF stash restored cleanly post-tag. Tagged `phase-1-scaffold-green` with toolchain version annotation. **User sign-off received 2026-05-01.** Phase 2 advance held pending explicit "begin Phase 2" signal. |

<!-- 2026-05-01: CLAUDE.md drafted, awaiting user review before Phase 1 execution. -->
<!-- 2026-05-01: rev2 — applied E2 (PSRAM=n), E3 (version pins), E4 (drop arduino-only PIO setting + partitions), E5 (pre-commit), E6 (drop ctest from Phase 1 gate), R1 (.env.example), R2 (strap-pin specifics), R4 (versioned snapshot), R5 (filename note expanded), R6 (parity check → Phase 4 / OQ-6). E1 applied as split-repo (reversal of prior monorepo direction — flagged). R3 declined: global ~/.claude/CLAUDE.md §11+§15 authoritatively says current MCP Pi Gateway IP is 192.168.20.150 (relocated from .15.150 on 2026-04-23). -->
<!-- 2026-05-01: rev3 — pin bumps approved per environmental audit. ESP-IDF v5.3.1 → v5.5.4 (already side-installed at ~/esp/esp-idf-v5.5.4/). gitleaks v8.21.x → v8.30.x (currently v8.30.1, already installed). clang-format LLVM 18 keg-only path documented. §8 MCP Pi gateway row strike. Phase 1 execution begins. -->
<!-- 2026-05-01: rev4 — PlatformIO dropped after Attempt 2 exposed the joint-pin inconsistency. Single canonical build path = ESP-IDF v5.5.4 via idf.py for both local and CI. §3 #6 rewritten to functional-equivalence (the byte-equivalent framing in earlier reviews was operator-introduced and not load-bearing in the contract). Guardrail #9 (gate-execution discipline) preserved. OQ-6 closed. New Pin-discipline lesson in RUNBOOK. CI matrix down to 4 jobs (no pio-build). -->
<!-- 2026-05-01: Phase 1 SIGN-OFF received. OQ-8 closed (LoRaWAN / Class A / AS923 / OTAA → ChirpStack 10.10.8.140 / RadioLib stack confirmed; ADR-003 still formalises in Phase 5). OQ-4 stays open. Hardware repo creation + GitHub remote both deferred per user sequencing argument (hardware repo created first, tagged, then firmware HARDWARE_REV.md pinned; cf. user message 2026-05-01). Schematic CLAUDE.md regeneration deferred to Phase 2 prep (option β). Phase 2 entry-criteria draft acknowledged but not yet locked into §5; will fold in when user signals "begin Phase 2". -->

---

## 7. Open questions (to be retired by ADR)

| ID | Question | Owner phase | Default position |
|---|---|---|---|
| OQ-1 | MAX3232 RS-232 transceiver — populate, DNP, or repurpose as console? | Phase 2 / ADR-001 | Default DNP for production unless schematic shows it on a different UART than RS-485 and is intended for field use. |
| OQ-2 | PSRAM — enable in Phase 2 after RAK3112 datasheet confirms Quad vs Octal mode. **Phase 1 default = disabled.** | Phase 2 / ADR-001 | Phase 1: `CONFIG_SPIRAM=n`. Phase 2: enable + correct mode. |
| OQ-3 | LoRaWAN stack — RadioLib vs LMIC vs LoRaMac-node port? | Phase 5 / ADR-003 | Default RadioLib (active maintenance, ESP-IDF-friendly, SX1262 native). |
| OQ-4 | AS923 sub-band for Bangladesh deployment | Phase 5 / ADR-004 | Confirm against `rnd-southerniot/siot-crm-review` ChirpStack region config. |
| OQ-5 | Antenna option for `-9-SM-I` variant — IPEX vs onboard PCB? | Phase 2 (visual inspection) | Defer to bench-side inspection. |
| ~~OQ-6~~ | ~~sdkconfig parity check between `idf.py` and `pio` build outputs.~~ | **CLOSED 2026-05-01** | Obsolete: PlatformIO dropped at Phase 1 Attempt 2 (rev4). With a single canonical build path, there's no second build to parity-check against. See §1 footnote ². |
| OQ-7 | ESP-IDF local patches in `~/esp/esp-idf-v5.5.4/` (stashed during Phase 1 to ensure pristine `v5.5.4` build) — vendor properly into `firmware/esp-idf-patches/` with attribution and intent documentation. **Phase 2 entry prereq** before any code touching USB or parallel-IO peripherals. | Phase 2 / ADR-001 prereq | (a) `usb_types_ch9.h` (IAD descriptor size 9→8): likely upstream bug fix per USB 2.0 ECN — vendor as `0001-fix-iad-desc-size.patch`, file ESP-IDF GitHub issue, submit upstream PR if confirmed. (b) `esp_lcd_panel_io_parl.c` (PARLIO sample edge POS→NEG): **intent unclear — investigate before vendoring.** If real fix, vendor like (a); if hardware-specific workaround, gate behind Kconfig and document hardware case. **Do not vendor an undocumented patch.** |
| ~~OQ-8~~ | ~~Schematic title says "RS485 P2P Node V1.1" — LoRa P2P or LoRaWAN?~~ | **CLOSED 2026-05-01 (Phase 1 sign-off)** | **Resolved: LoRaWAN, Class A, AS923, OTAA join to existing CRM ChirpStack at `10.10.8.140`.** Reasoning: existing Southern IoT infrastructure (deployed ChirpStack, AS923 region configured, dashboards/alerts/FUOTA pipeline standardised, OTAA + DevEUI/AppKey provisioning already in place) is the load-bearing argument; the schematic's "P2P Node" string in the title is firmware-side naming and does not constrain the radio path. OQ-3 default (RadioLib) confirmed by user at sign-off; formal close still tracks via ADR-003 in Phase 5. The "P2P" in firmware identifiers should be suppressed when Phase 5 lands. OQ-4 (AS923 sub-band selection) remains open until Phase 5 against CRM region config. |

---

## 8. Reference (read-only, do not modify)

- ChirpStack CRM (AS923): **`10.10.8.140`** — global §11. Load-bearing for Phase 5.
- Schematic source (V1.1, captured 2026-05-01): `~/Developer/projects/pcb-design/rs485-node/{RAK3112 + RS485 P2P Node V1.1 Schematic.pdf, RAK3112 + RS485 P2P Node V1.1.epro}`. The earlier UUID-named PDF and `ProPrj_RAK3312+…epro` files (with the RAK3312 typo) were superseded between Phase 0 discovery and Phase 1 scaffold execution; the new filenames carry the corrected `RAK3112` and the explicit `V1.1` revision tag.
- Hardware repo (planned): `~/Developer/projects/pcb-design/rak3112-rs485-node-hw/` · `rnd-southerniot/rak3112-rs485-node-hw`.

> **Footnote on schematic provenance.** The Phase 0 BOM extraction was performed against the earlier `ProPrj_RAK3312+ RS485_2026-05-01.epro` filename and confirmed the placed module as `RAK3112-9-SM-I` (BOM authoritative). The current filenames `RAK3112 + RS485 P2P Node V1.1.{epro,pdf}` correct the typo and add an explicit `V1.1` revision label. The "P2P Node" string in the title may indicate **LoRa P2P** rather than LoRaWAN — flagged as **OQ-8** for Phase 2 clarification (impacts stack choice in OQ-3 and the entire LoRaWAN narrative if "P2P" is the intended deployment model).

---

**End of contract rev2. Phase 1 will not begin until the user has read this document and replied with explicit approval (or further edits).**
