# rak3112-rs485-node — Execution Contract (firmware)

> **Status:** rev8 — Phase 7 production-hardening plan drafted (7a–7e, gated); rev7 schematic-V1.1 alignment retained. 2026-06-25.
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
├── hardware/                             # read-only snapshot of HW repo (version-pinned per HARDWARE_REV.md)
│   ├── README.md                         # "canonical source: rnd-southerniot/rak3112-rs485-node-hw"
│   └── schematic/
│       └── v<X.Y>/                       # versioned snapshots; populated on each HW tag
│           ├── *.pdf                     # rendered schematic export
│           ├── *.epro                    # EasyEDA Pro project file
│           ├── page-*.png                # 300-DPI per-page renders
│           ├── esch-pin-labels.json      # structured pin/net extraction (source-of-truth, EC-2)
│           └── CHECKSUMS.txt             # SHA-256 of every other file in the dir
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

### Phase 2 — HW bring-up prep · **ENTRY CRITERIA LOCKED 2026-05-01 (rev5)**

**Goal.** Produce a signed-off `ADR-001-pin-map.md` in the **hardware repo** (`rnd-southerniot/rak3112-rs485-node-hw`), covering every ESP32-S3 GPIO / net assignment in the V1.1 schematic, sufficient to drive Phase 3 first-flash without guesswork. Resolve OQ-1, OQ-2, OQ-5. Vendor ESP-IDF patches per OQ-7 (asymmetric outcome acceptable — see Footnote 1).

**Entry criteria.** All must hold before Phase 2 *advances* (i.e., before tag `phase-2-pinmap-locked` is created). Numbered in execution order. Each criterion has its own bare smoke gate (per Guardrail §3 #9). Cross-repo gates verify *the coupling*, not the other repo's content (per Footnote 3).

> **Path conventions (rev6 clarification).** When a smoke gate is labeled "**firmware repo**", paths are relative to the firmware repo's root — e.g., `firmware/build/...`, and the snapshot-mirror tree at `hardware/schematic/v1.1/...` (the firmware-repo's mirror; see §2 layout). When labeled "**hardware repo**", paths are relative to the hardware repo's root — e.g., `schematic/v1.1/...`, `adr/...`, `photos/...`. **The hardware repo does NOT have a top-level `hardware/` directory** — that prefix only exists inside the firmware repo, where it identifies the read-only snapshot of canonical hardware content. Earlier rev5 wording inadvertently used the firmware-repo prefix in hardware-repo gate paths; rev6 corrects this.

#### EC-1. OQ-7 resolved — ESP-IDF patches handled

- `usb_types_ch9.h` patch: investigated, attributed (upstream PR # if exists, local origin if not), vendored to `firmware/esp-idf-patches/0001-fix-iad-desc-size.patch` with header documenting context. Applied via project-side `firmware/scripts/apply-patches.sh`.
- `esp_lcd_panel_io_parl.c` patch: intent investigated. **If intent cannot be determined, the patch is NOT vendored** — stays in local ESP-IDF as personal modification, documented in `docs/RUNBOOK.md` under "ESP-IDF local modifications (not project-bound)". Don't vendor an undocumented patch.
- See Footnote 1 for the asymmetric-outcome case.

**`apply-patches.sh` specification (EC-1 deliverable):**

```bash
#!/usr/bin/env bash
# firmware/scripts/apply-patches.sh
# Applies project-vendored ESP-IDF patches to $IDF_PATH.
# Idempotent: safe to re-run (skips already-applied via git apply --reverse --check).
# --dry-run flag verifies all patches apply cleanly without touching the tree.
set -euo pipefail

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-v5.5.4}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCHES_DIR="$SCRIPT_DIR/../esp-idf-patches"

DRY_RUN=false
[[ "${1:-}" == "--dry-run" ]] && DRY_RUN=true

# Preconditions — clear errors over confusing apply failures
test -d "$IDF_PATH/.git" || {
    echo "ERROR: IDF_PATH ($IDF_PATH) is not a git repository" >&2
    exit 2
}
test -d "$PATCHES_DIR" || {
    echo "ERROR: PATCHES_DIR ($PATCHES_DIR) does not exist" >&2
    exit 2
}

# Deterministic ordering across locales (load-bearing for >= 2 patches)
for patch in $(printf '%s\n' "$PATCHES_DIR"/*.patch | LC_ALL=C sort); do
    [[ -f "$patch" ]] || continue
    if git -C "$IDF_PATH" apply --check "$patch" 2>/dev/null; then
        if $DRY_RUN; then
            echo "Would apply: $(basename "$patch")"
        else
            git -C "$IDF_PATH" apply "$patch"
            echo "Applied: $(basename "$patch")"
        fi
    elif git -C "$IDF_PATH" apply --reverse --check "$patch" 2>/dev/null; then
        echo "Already applied: $(basename "$patch")"
    else
        echo "FAIL: $(basename "$patch") does not apply cleanly to $IDF_PATH" >&2
        exit 1
    fi
done
```

**Verification routine (fresh-clone, dev tree never touched):**

```bash
# EC-1 verification — proves patch series is self-contained against pristine v5.5.4.
# Verification is destructive-but-isolated: temp clone is one-shot,
# ~/esp/esp-idf-v5.5.4/ is never modified, no stash dance needed.

ESP_IDF_VERIFY_ROOT="$(mktemp -d)"
ESP_IDF_VERIFY_PATH="$ESP_IDF_VERIFY_ROOT/esp-idf-v5.5.4"

# On any failure, retain the temp dir for forensic inspection.
# rm -rf only runs on the success path below.
trap 'echo "Verification artifacts retained at: $ESP_IDF_VERIFY_ROOT" >&2' ERR

git clone --depth 1 --branch v5.5.4 \
    https://github.com/espressif/esp-idf.git "$ESP_IDF_VERIFY_PATH"
( cd "$ESP_IDF_VERIFY_PATH" && git submodule update --init --recursive )

IDF_PATH="$ESP_IDF_VERIFY_PATH" firmware/scripts/apply-patches.sh
IDF_PATH="$ESP_IDF_VERIFY_PATH" "$ESP_IDF_VERIFY_PATH/tools/idf.py" -C firmware build

trap - ERR
rm -rf "$ESP_IDF_VERIFY_ROOT"   # success path only
```

**Rollback (EC-1 specific).** If `apply-patches.sh` itself becomes the failure point, `git revert` on the patch-add commit does NOT restore a working build path — the script would still be broken or missing. Fall back to building against `~/esp/esp-idf-v5.5.4/` working tree as it stood pre-Phase-2 (with whichever local mods exist), and re-evaluate whether vendoring is the right approach. Document the failure mode in `docs/RUNBOOK.md` under "Phase 2 attempts".

**Smoke gate (firmware repo, bare):**

```bash
test -f firmware/esp-idf-patches/0001-fix-iad-desc-size.patch
head -10 firmware/esp-idf-patches/0001-fix-iad-desc-size.patch | grep -q "^Subject:"
test -x firmware/scripts/apply-patches.sh
firmware/scripts/apply-patches.sh --dry-run
. ~/esp/esp-idf-v5.5.4/export.sh
idf.py -C firmware build
```

#### EC-2. Schematic rendered, JSON-extracted, and cross-checked

- PDF rendered to PNG (per-page, 300 DPI) via poppler: `pdftoppm -r 300 schematic.pdf page -png`.
- Both PDF and PNG committed to **hardware repo** at `schematic/v1.1/`.
- SHA-256 of PDF recorded in `schematic/v1.1/CHECKSUMS.txt` (full hashes — no truncation).
- Pin-label extraction from `.esch` JSON files (source-of-truth) dumped to `schematic/v1.1/esch-pin-labels.json`.
- Diff PNG-derived pin labels vs. `.esch` JSON labels. Discrepancies flagged in ADR-001. **JSON wins on conflicts** (PDF can be export-stale).

**Smoke gate (hardware repo):**

```bash
ls schematic/v1.1/*.pdf >/dev/null         # at least one PDF (filename glob — robust to filename changes)
ls schematic/v1.1/*.epro >/dev/null        # at least one EasyEDA Pro project file
ls schematic/v1.1/page-*.png >/dev/null    # at least one rendered page
test -f schematic/v1.1/esch-pin-labels.json
test -f schematic/v1.1/CHECKSUMS.txt
sha256sum -c schematic/v1.1/CHECKSUMS.txt
jq empty schematic/v1.1/esch-pin-labels.json
```

#### EC-3. Schematic ↔ BOM consistency check (severity-gated)

> **Execution order:** EC-3 must complete with **zero MAJOR mismatches** before EC-4 (pin-map extraction) begins. MINOR mismatches go to ADR-001 appendix without blocking.

| Severity | Examples | Treatment |
|---|---|---|
| **MAJOR** | Component reference designator in schematic but missing from BOM (or vice versa); U-prefix (active part) absent from one side; net connectivity that implies a component not listed in BOM | **Blocks EC-4.** Must resolve before pin-map extraction begins. |
| **MINOR** | Footprint code variation (0603 vs 0402); value tolerance disagreement; manufacturer-part-number alternates; descriptive value vs exact spec | **Flagged in ADR-001 appendix**, doesn't block. |

- BOM (extracted from `.epro` `project.json` BOM tree in Phase 0) cross-referenced against schematic component instances.
- For each schematic ref designator: confirm presence in BOM with matching value/footprint.
- For each BOM line: confirm presence in schematic.
- Mismatches classified per severity table above.
- Pass: **zero MAJOR mismatches**. Minor count ≥ 0, all logged.

**Smoke gate (hardware repo):**

```bash
test -f adr/ADR-001-pin-map-DRAFT.md
grep -q "^## Schematic-BOM consistency" adr/ADR-001-pin-map-DRAFT.md
grep -qE "^MAJOR mismatches: 0$" adr/ADR-001-pin-map-DRAFT.md
```

#### EC-4. Pin-map raw extraction

- Every ESP32-S3 GPIO referenced in the schematic extracted into `adr/ADR-001-pin-map-DRAFT.md` (in the hardware repo, per EC-8).
- Format: table with columns `Pin # | GPIO | Schematic net name | Connected to | Strap pin? | Boot-state requirement | Notes`.
- Source: `.esch` JSON (preferred) with PNG visual confirmation.
- No firmware code references this draft yet — research artifact only.

**Smoke gate (hardware repo):**

```bash
test -f adr/ADR-001-pin-map-DRAFT.md
grep -q "^| Pin # | GPIO | Schematic net" adr/ADR-001-pin-map-DRAFT.md
test "$(grep -cE '^\| [0-9]+ \| GPIO' adr/ADR-001-pin-map-DRAFT.md)" -gt 0
```

#### EC-5. OQ-1 resolved — RS-232 disposition + H1 purpose closure

- **EC-5a (RS-232 / MAX3232):** Schematic review confirms whether MAX3232 shares UART pins with RS-485 transceiver. Decision documented in ADR-001: DNP / repurpose-as-console / keep-on-separate-UART. Default position carried over: **DNP for production** unless schematic shows independent UART.
- **EC-5b (H1 header — PZ254V-11-02P 2-pin):** §0 lists this as "purpose TBD — likely UART debug". Schematic + BOM review confirms net connections. Decision documented in ADR-001: UART debug / power input / aux IO / DNP. **§0 row updated** in this firmware contract to reflect resolved purpose (rev6 amendment, post-Phase-2-sign-off).

**Smoke gate (hardware repo):**

```bash
grep -q "^## OQ-1: RS-232 disposition" adr/ADR-001-pin-map-DRAFT.md
grep -q "^## H1 header purpose" adr/ADR-001-pin-map-DRAFT.md
```

#### EC-6. OQ-2 resolved — PSRAM mode

- RAK3112 datasheet section on flash/PSRAM read in full.
- **Cross-check:** RAK's published reference firmware for RAK3112 (Arduino-ESP32 BSP examples) inspected for the mode they configure. Two sources beat one.
- Mode (Quad/Octal) confirmed and documented in ADR-001.
- `firmware/sdkconfig.defaults` updated: `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_MODE_OCT=y` or `CONFIG_SPIRAM_MODE_QUAD=y`.
- **No mini-flash for verification** — Phase 2 maintains no-hardware-touch discipline.
- ADR-001 records **expected ESP32-S3 bootloader log signature** for PSRAM init in chosen mode (e.g., octal: `Found 8MB PSRAM device` + `Adding pool of … bytes of internal memory`; quad: equivalent line per ESP-IDF source). Phase 3 first-flash gate uses signature for fast PSRAM verification ("boot log contains success line within 2 s of reset"), avoiding a debug cycle if mode is wrong.
- Phase 2 verification: `idf.py build` succeeds (config syntax valid). Functional verification deferred to Phase 3.

**Smoke gate (firmware + hardware repos):**

```bash
grep -E "^CONFIG_SPIRAM_MODE_(OCT|QUAD)=y$" firmware/sdkconfig.defaults
. ~/esp/esp-idf-v5.5.4/export.sh
idf.py -C firmware build
grep -q "^## OQ-2: PSRAM mode" adr/ADR-001-pin-map-DRAFT.md
grep -q "^### Expected boot-log signature" adr/ADR-001-pin-map-DRAFT.md
```

#### EC-7. OQ-5 resolved — antenna variant

- Visual inspection of physical RAK3112 module on bench.
- Variant determined: IPEX MHF4 connector / onboard PCB RF pads / RF-pad variant per `-9-SM-I` decoding.
- Documented in ADR-001 with photograph in `photos/module-antenna-2026-MM-DD.jpg` (in hardware repo).
- Phase 5 LoRa antenna routing implications flagged in ADR-001.

**Smoke gate (hardware repo):**

```bash
ls photos/module-antenna-*.jpg >/dev/null
grep -q "^## OQ-5: antenna variant" adr/ADR-001-pin-map-DRAFT.md
```

#### EC-8. Hardware repo created (with slim CLAUDE.md)

- `rnd-southerniot/rak3112-rs485-node-hw` repo created on GitHub (private).
- Initial push contains:
  - V1.1 schematic reference: PDF, PNG renders, `.esch` JSON dumps, BOM extract, `CHECKSUMS.txt`.
  - **Slim hardware-repo `CLAUDE.md`**: execution contract covering ADR drafting discipline, sign-off rules, tag conventions (`adr-NNN-locked`, `schematic-vX.Y-archive`), and the hardware-side smoke gate definitions.
- Tagged `v1.1-archive` as immutable V1.1 reference point.
- Firmware repo gains `HARDWARE_REV.md` pinning hardware repo's `v1.1-archive` tag SHA (full 40-char hex, no truncation).
- ADR-001 drafts directly in hardware repo's `adr/` directory — **no temporary staging** in firmware repo.
- **Note:** V1.1 schematic CLAUDE.md regeneration (option β from Phase 1 closure) is NOT a Phase 2 deliverable. The slim CLAUDE.md from this EC is sufficient for Phase 2 ADR-001 work; full V1.1 rework is a separate future workstream.

`HW_REPO_PATH` resolution priority (used by firmware-side gates):

1. Environment variable `HW_REPO_PATH` if set
2. Config file `~/.config/siot/rak3112-rs485-node.env` (per-machine)
3. Default: `~/Developer/projects/pcb-design/rak3112-rs485-node-hw`

**Smoke gate (firmware repo, verifying *the coupling* only — per Footnote 3):**

```bash
test -f HARDWARE_REV.md
grep -qE '^pinned_sha:\s+[a-f0-9]{40}$' HARDWARE_REV.md
HW_SHA=$(awk '/^pinned_sha:/ {print $2}' HARDWARE_REV.md)
HW_REPO_PATH="${HW_REPO_PATH:-$HOME/Developer/projects/pcb-design/rak3112-rs485-node-hw}"
test -d "$HW_REPO_PATH/.git"
git -C "$HW_REPO_PATH" cat-file -e "$HW_SHA"
git -C "$HW_REPO_PATH" tag --list | grep -q "^v1.1-archive$"
```

**Smoke gate (hardware repo, verifying its own content):**

```bash
test -f CLAUDE.md
grep -q "^## ADR drafting discipline" CLAUDE.md
grep -q "^## Tag conventions" CLAUDE.md
git tag --list | grep -q "^v1.1-archive$"
```

#### EC-9. Phase 2 deliverable + sign-off

- ADR-001 finalized (all DRAFT markers removed; renamed `ADR-001-pin-map.md`).
- Sign-off recorded in ADR-001 footer: date + approver name (Arif).
- **Hardware repo:** tag `adr-001-locked` created on the sign-off commit.
- **Firmware repo:** tag `phase-2-pinmap-locked` created. `HARDWARE_REV.md` updated to pin the new `adr-001-locked` tag SHA (replacing the `v1.1-archive` pin from EC-8).
- §6 State block in firmware repo updated with Phase 2 outcome (all attempts dated, **no squashing** — per "aesthetic vs functional preference" lesson).

**Smoke gate (final, both repos):**

```bash
# Hardware repo
test -f adr/ADR-001-pin-map.md   # not -DRAFT
grep -qE "^Signed-off-by: Arif " adr/ADR-001-pin-map.md
git tag --list | grep -q "^adr-001-locked$"

# Firmware repo
git tag --list | grep -q "^phase-2-pinmap-locked$"
HW_SHA=$(awk '/^pinned_sha:/ {print $2}' HARDWARE_REV.md)
test "$(git -C "$HW_REPO_PATH" rev-parse adr-001-locked^{commit})" = "$HW_SHA" \
  || test "$(git -C "$HW_REPO_PATH" rev-parse adr-001-locked)" = "$HW_SHA"
```

---

**Footnote 1 — OQ-7 asymmetric outcome (accepted in advance).**

If the PARLIO patch's intent cannot be determined and is therefore NOT vendored:

- Local builds on `siot-dev-m5` will produce `v5.5.4-dirty` (because `~/esp/esp-idf-v5.5.4/` retains the personal mod).
- CI builds (clean clone + IAD patch only via `apply-patches.sh`) will produce clean `v5.5.4`.
- Acceptable asymmetry. Recorded explicitly so neither operator nor reviewer is surprised.
- Asymmetry documented in `docs/RUNBOOK.md` under "ESP-IDF local modifications (not project-bound)".
- §3 #6 single-canonical-build-path invariant **NOT violated** — both local and CI use the same ESP-IDF version, target, and `sdkconfig.defaults`; only local has an extra incidental modification off the project's compile path.

**Footnote 2 — Tag naming convention (locked rev5).**

| Pattern | Repo | Use |
|---|---|---|
| `phase-N-<name>-<status>` | firmware repo only | Firmware phase boundaries (e.g., `phase-1-scaffold-green`, `phase-2-pinmap-locked`) |
| `adr-NNN-locked` | hardware repo only | ADR sign-off in hardware repo (e.g., `adr-001-locked`) |
| `schematic-vX.Y-archive` | hardware repo only | Immutable schematic version reference (e.g., `v1.1-archive`) |

**Firmware phase counters do NOT cross into hardware repo tag namespace, and vice versa.** Maintains split-repo decoupling per Footnote 3.

**Footnote 3 — Cross-repo coupling.**

The firmware repo couples to the hardware repo only via `HARDWARE_REV.md`:

```
pinned_sha:  <40-char hex SHA>
pinned_tag:  <tag name>
pinned_date: <ISO 8601>
```

Firmware repo's gates verify *the coupling* (file format, SHA resolves in pinned hardware repo). Firmware repo's gates do NOT verify hardware repo content (that's the hardware repo's contract responsibility). Same principle in reverse: hardware repo doesn't verify firmware content.

---

**Phase 2 closure conditions.**

**On PASS:**
- ADR-001 user-signed in hardware repo
- Tag `adr-001-locked` in hardware repo
- Tag `phase-2-pinmap-locked` in firmware repo (annotation includes toolchain versions + full SHA-256 of any new build artefacts, no truncation per "aesthetic vs functional preference" lesson)
- `HARDWARE_REV.md` updated to pin `adr-001-locked`
- §6 State block updated with full Phase 2 history (all attempts, no squash)
- `docs/RUNBOOK.md` updated with any new lessons captured during Phase 2
- Advance to Phase 3 held pending explicit user sign-off (separate review)

**On FAIL:**
- Stop. Log to `docs/RUNBOOK.md` under "Phase 2 attempts".
- Do not advance. **Do not flash hardware.**
- Fix in `fix/p2-*` branch (firmware) or `fix/adr-001-*` branch (hardware).

**Rollback** (per-criterion, see EC-1 for patch-script-specific rollback):
- EC-2 schematic extraction: `git revert` schematic-import commit (hardware repo)
- EC-3/4/5/6/7 ADR drafts: edit in place; never published until sign-off
- EC-8 hardware repo: repo creation is one-way; tags/content can be `git reset --hard` to a prior state if needed before sign-off
- EC-9 final tags: `git tag -d` on local, do not push deletion if a remote already saw the tag

### Phase 3 — First flash + USB-CDC console · **scoped**

Flash hello-world to the actual board over USB-CDC. Verify boot, log output at 115200, no bootloop, no brown-out, current draw within RT6160 spec. Bench checklist + hardware-safety pre-flight in `RUNBOOK.md`.

### Phase 4 — RS-485 echo + DE/RE timing · **scoped**

Drive TP8485E from UART (per ADR-001), echo bytes back to a USB-RS485 adapter at 9600 8N1 then 115200. Characterise DE/RE turnaround on Saleae Logic 2. ADR-002 = bus electrical (termination, bias, fail-safe). Phase 4 also lands the **sdkconfig ↔ platformio.ini parity check** (OQ-6) as a CI step, plus the first real `ctest`-driven host unit tests for framing.

### Phase 5 — LoRaWAN OTAA join (AS923) · **scoped**

Pick stack (RadioLib vs LMIC vs LoRaMac-node port — OQ-3). OTAA join against the CRM ChirpStack at `10.10.8.140`. Confirm device shows up + uplink frame. ADR-003 = stack choice; ADR-004 = AS923 sub-band selection (OQ-4).

### Phase 6 — Field protocol (Modbus RTU master, configurable) · **scoped**

Modbus RTU master role, register set defined by JSON config in NVS, sample at configurable interval, payload-encode for LoRaWAN uplink. ADR-005 = payload schema.

### Phase 7 — Production hardening · **PLAN DRAFTED 2026-06-25 (rev8) · execution held per sub-phase readiness**

**Goal.** Take the bench-proven Phase 6 telemetry node to a field-deployable state along four axes: deterministic fault recovery (task watchdog + brownout), low average current via light-sleep duty-cycling, safe in-field updates (dual-slot OTA + rollback), and credential/config provisioning from NVS instead of compiled-in secrets.

**Entry criteria (must hold before any Phase 7 sub-phase advances).**
- `phase-6-modbus-green` tag exists on `main` (✓ 2026-06-24) and a clean `idf.py -C firmware build` reproduces the Phase 6 default binary as the regression baseline.
- ESP-IDF v5.5.4 sourced via `. ~/esp/esp-idf-v5.5.4/export.sh`; `idf.py --version` contains `v5.5.4`.
- Branch `phase/p7-production-hardening` checked out (✓ 2026-06-25), off clean `main` (`87332ca`).
- **Hardware-safety pre-flight for the power/OTA gates** (global §3, firmware §6): project board MAC `3c:dc:75:6f:85:dc` on bench; an **inline current meter / current-readout bench supply** available for 7b; H1 jumper + native-USB port + chip-id confirmed before any flash.
- Phase 7 open questions defaulted at entry: **OQ-9** (sleep strategy), **OQ-10** (OTA transport + partition scheme), **OQ-11** (provisioning transport), **OQ-12** (current-draw target).
- User "begin Phase 7" signal received (✓ 2026-06-25).

**Guardrail reminders specific to Phase 7.**
- **OTA partition change = full flash erase.** Confirm in writing + back up the running image (`esptool read_flash`) before the first 7c flash (global §2 #1, firmware §6.6). `esptool erase_flash` stays gated (§3 #1).
- **Watchdog feed in the main loop only** — never in an ISR or a blocking wait (firmware §1.2/§1.3). A disabled watchdog must be a single documented commit with a re-enable plan.
- **No compiled-in secrets after 7d.** DevEUI/AppKey move to NVS; `firmware/main/lora_credentials.h` stays gitignored and becomes a fallback only.

#### 7a — Resilience: Task Watchdog + brownout + reset-cause log
- Subscribe the field-app task to the **Task Watchdog Timer** (`esp_task_wdt`), feed once per main-loop iteration; panic handler names the offending task.
- Confirm/raise the **brownout detector** threshold for the RT6160 3V3 rail; verify no spurious BOD resets under normal draw.
- Print **reset reason** (`esp_reset_reason()`) + boot count at startup (seed of the firmware §8 fault log).
- **Smoke gate (HIL).** A debug-only Kconfig path that stops feeding the WDT triggers a TWDT reset within the configured timeout (boot log shows `Task watchdog got triggered` + the task name, then `rst:…(…WDT)`); with the debug path off, the node runs ≥ 10 min of normal field-app loop with **zero** spurious WDT/BOD resets; reset-cause line present each boot.

#### 7b — Power: light-sleep duty-cycling between samples
- Between sample intervals enter **light-sleep** (CPU + unused peripherals gated; RTC-timer wake). Ensure SX1262 is in sleep and the TP8485E DE/RE idles in the low-power (receive) state across sleep; the RadioLib session is already NVS-persisted (Phase 5), so no re-join per wake.
- Establish the **OQ-12 average-current target** and measure actual draw at the configured interval.
- **Smoke gate (HIL, requires current measurement).** Measured **average current** at the configured interval ≤ the OQ-12 target on the bench supply; uplinks continue to arrive in ChirpStack across sleep cycles (fCnt advances monotonically, no session loss); wake-to-uplink jitter within budget.

#### 7c — OTA: dual-slot partitions + update with rollback
- Add `firmware/partitions.csv` (16 MB: `nvs`, `otadata`, `factory`/`ota_0`, `ota_1`, `phy_init`), set `CONFIG_PARTITION_TABLE_CUSTOM`.
- Implement the OTA apply path (transport per **OQ-10**) with **rollback**: `esp_ota_*` mark-valid on a healthy boot, automatic rollback to the previous slot on boot failure.
- **Hardware safety.** Back up the running image first; the partition-table change forces an erase — confirm in writing before flashing.
- **Smoke gate (HIL).** Flashed device reports running partition `factory`/`ota_0`; an OTA of a new build boots from `ota_1`, marks valid, survives reset; a deliberately-broken image **rolls back** to the previous slot and the node still joins + uplinks.

#### 7d — Provisioning: NVS-stored credentials & runtime config
- Move DevEUI/JoinEUI/AppKey **and** Modbus runtime config (baud/parity/slave/interval/device-type) into **NVS**, loaded at boot; compiled `lora_credentials.h` becomes a last-resort fallback only.
- Provisioning transport per **OQ-11**; ships a `tools/` writer that injects the values the SCOMM CRM mints (`tools/provision_node.py` already produces DevEUI/AppKey) into NVS.
- **Smoke gate.** A factory image with empty NVS waits for provisioning (clear log, no bogus join); after writing creds to NVS the node joins AS923 using the **NVS** values (not compiled); changing Modbus config in NVS changes runtime behaviour **without a rebuild**.

#### 7e — Integration soak + sign-off
- Combined run — watchdog + light-sleep + NVS creds on an OTA-capable image — over a bounded soak: **no resets, current within budget, uplinks landing in ChirpStack**.
- **Resource budgets** re-checked (firmware §9): flash ≤ 60 %, RAM ≤ 70 %; binary size logged in CI.
- **Smoke gate (final).** Soak of ≥ 30 min / ≥ N uplinks with zero unplanned resets; budgets green; CI green (pre-commit / idf-build pristine / gitleaks / host-tests).

**ADRs opened by Phase 7:** ADR-006 (power / sleep strategy — OQ-9/12), ADR-007 (partition layout + OTA transport + rollback — OQ-10), ADR-008 (provisioning workflow + NVS schema — OQ-11).

**On PASS (whole phase):** tag `phase-7-production-green` (annotation: toolchain versions + full binary SHA-256, no truncation); merge to `main` via PR (the local push-guard forces the PR flow); update §6 State block (all sub-phase attempts, no squash); RUNBOOK lessons. Field-deployment readiness review held separately.

**On FAIL (any sub-phase):** stop, log to `docs/RUNBOOK.md` "Phase 7 attempts", do **not** advance, do **not** ship. Fix in `fix/p7-*` off `phase/p7-production-hardening`.

**Rollback:** per sub-phase — 7a/7b/7d are config/source reverts (`git revert`). **7c is the high-risk one** — restore the backed-up image and the Phase 6 single-app partition layout via `esptool write_flash` of the backup, then re-evaluate (mirrors the Phase 2 EC-1 "script-is-the-failure-point" rollback discipline).

---

## 6. State block (append-only)

| Phase | Date | Outcome | Notes |
|---|---|---|---|
| 0 — Discovery | 2026-05-01 | PASS | Silicon = ESP32-S3 + SX1262 (verified from `.esym` pin labels). BOM = 5 ICs, 2 tact switches, DC1+P1+H1 input connectors. Toolchain (rev3) = ESP-IDF v5.5.4 + PlatformIO platform-espressif32 ~6.7.0. Decision: split-repo. |
| 1 — Scaffold (Attempt 1) | 2026-05-01 | **FAIL** | Smoke gate step 3 (`pio run`): "Missing the 'src' folder with project sources". Operator-side `\| tee \| tail` masked exit code initially. ESP-IDF build (step 2) PASS with binary 183 328 B. ESP-IDF stash restored cleanly. See `docs/RUNBOOK.md` Attempt 1. |
| 1 — Scaffold (Attempt 2) | 2026-05-01 | **FAIL** | Branch `fix/p1-pio-srcdir`. P1-FIX-1 (src_dir=main + Guardrail #9) applied. Smoke gate step 3 (`pio run`) bare: `CMake Error … 3.20 or higher is required. You are running version 3.16.4`. Root cause: rev3 pinned ESP-IDF v5.5.4 + platform-espressif32 ~6.7.0 jointly inconsistent (PIO bundles ESP-IDF v5.2.1 + CMake 3.16). ESP-IDF build PASS, byte-identical to Attempt 1 binary. ESP-IDF stash restored. See `docs/RUNBOOK.md` Attempt 2 + Pin discipline lesson. |
| 1 — Scaffold (Attempt 3) | 2026-05-01 | **PASS · signed off** | Branch `fix/p1-drop-platformio`. rev4 contract: PlatformIO dropped entirely; single canonical build path (ESP-IDF v5.5.4); 4-step smoke gate. All four steps green: pre-commit (6 hooks), `idf.py build` (binary `rak3112_rs485_node.bin` 183 328 B, `IDF_VER=v5.5.4` clean, SHA-256 `a2f3d6044f9458aa38a492cad531c53071b2f829a33f5fd2635db72271fe5116`), gitleaks (11 commits / 243 KB / no leaks), layout check. ESP-IDF stash restored cleanly post-tag. Tagged `phase-1-scaffold-green` with toolchain version annotation. **User sign-off received 2026-05-01.** Phase 2 advance held pending explicit "begin Phase 2" signal. |
| 2 — HW bring-up prep | 2026-05-01 → 2026-05-02 | **PASS · signed off** | Branch `phase/p2-execution`. Contract evolved rev4 → rev7 during execution (rev5 entry-criteria lock; rev6 path-prefix correction; rev7 V1.1 version alignment). 9 entry criteria all closed: EC-1 IAD patch vendored / PARLIO held back (firmware `76fecf5`, asymmetric outcome accepted per Footnote 1); EC-2 V1.1 schematic archive (hw `970cdf0`); EC-3 BOM consistency PASS with one MINOR-override (WJ500V residue, hw `1e644ca`); EC-4 44-pin map locked, all 4 strap pins correct, GPIO40/9 routed-but-unterminated for V1.2 expansion (hw `9344310`); EC-5 RS-232 DNP + H1 = 3V3 jumper (hw `555a4cc`); EC-6 Octal SPI PSRAM via Espressif S3 datasheet Table 1-1, sdkconfig updated (firmware `184844f`, hw `ba199c5`); EC-7 dual-option antenna (IPEX MHF4 + PCB trace) confirmed by bench photo (hw `d2b0aad`). EC-8a local hw repo init (hw `f82702d`); EC-8b publish + topics + interim coupling pin (firmware `2bb574b`; hw URL `rak3112-rs485-node-hw`). EC-9 sign-off: ADR-001 ACCEPTED at hw `a0b002c`, tagged `adr-001-locked` (tag object `a9b731db74151936a78ccaaf84df5840b9f2bee2`), Signed-off-by Arif <rnd@southerniot.net>. HARDWARE_REV.md durable pin replaces interim. Tagged `phase-2-pinmap-locked` with toolchain versions + full binary SHA-256. Five lessons captured during Phase 2 (gate-execution discipline, pin discipline, single canonical source, aesthetic-vs-functional, silicon-vendor abstraction layer, pre-commit-hook-checksum side-effects). Carries forward to Phase 3 entry criteria: GPIO9 + GPIO40 must be configured as deliberate-floating at boot on V1.1 boards (no internal pull, no drive); transitions to I²C-peripheral init when V1.2 boards exist (tracked in `rak3112-rs485-node-hw#1`). Advance to Phase 3 held pending explicit "begin Phase 3" signal. |
| 3 — First flash + console (Attempt 1) | 2026-06-20 | **PASS · signed off** | CI green (run `27866142319`), merged to `main` `459108e`, tagged `phase-3-first-flash-green`, **user sign-off received 2026-06-20**. Phase 4 advance held pending explicit "begin Phase 4" signal. Pre-flash hardware safety: two boards on bench, target disambiguated by chip-id/MAC — RAK3112 = ESP32-S3R8 (MAC `3c:dc:75:6f:89:24`) on native USB `/dev/cu.usbmodem1301`; unrelated ESP32-P4 (MAC `30:ed:a0:e1:8e:1c`) on `/dev/cu.usbserial-140`, never flashed. Code (commits `7db2f2f` pins/`gpio_remap.h`+`PIN_MAP.md`, `33de22e` GPIO9/40 floating init, `7ffb848` `flash.sh`, `3ae976a` RUNBOOK checklist + ADR-001 `<TBD>` hygiene note). **Console finding (`fbc1661`):** Phase-1 `CONFIG_ESP_CONSOLE_USB_CDC` (OTG/TinyUSB) produced no visible console on the RAK3112 native USB; native USB = ESP32-S3 USB-Serial-JTAG → switched to `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`. Smoke gate PASS (boot log in RUNBOOK Phase 3 Attempt 1): `Found 8MB PSRAM device` + `Adding pool of 8192K of PSRAM memory to heap allocator`; Octal+3V empirically confirmed (`octal_psram vendor id 0x0d (AP)`, `VCC 0x01 (3V)`) = ESP32-S3R8 per ADR-001 EC-6; `SPI SRAM memory test OK`; GPIO9/40 floating-init log present; heartbeat 1 Hz monotonic, no bootloop/brownout. Binary SHA-256 `45e4ef9a205f41e1bf761fd4ffaaede239ff1b132b6bb20fce64e42058fe399f`, `IDF_VER=v5.5.4-dirty` (local PARLIO mod, off-compile-path, Footnote 1). Tag `phase-3-first-flash-green` + merge to `main` + Phase 4 advance held pending explicit user sign-off. |
| 4 — RS-485 echo + DE/RE + host tests | 2026-06-20 | **PASS · signed off** | Branch `phase/p4-rs485-echo`, CI green (run `27869949406`), merged to `main` `1a2a748`, tagged `phase-4-rs485-green`, **user sign-off 2026-06-20**. HIL echo **byte-identical at 9600 AND 115200** (256/256) on project board MAC `3c:dc:75:6f:85:dc` via USB-RS485 adapter (Adapter 1 / CH340). Deliverables: `components/rs485` (UART1 half-duplex), `components/ring_buffer` + 6 host `ctest` (CI host-tests job now runs ctest), ADR-002. **KEY FINDING — DE/RE polarity:** ADR-001 EC-5a documented it inverted (`GPIO21 HIGH = receive`); bench toggle test proved **standard** (`GPIO21 HIGH = TX, LOW = RX`). The rev1 inverted-RTS firmware idled U9 in transmit so the node never received — many bring-up cycles lost to chasing adapters/A-B before a ground-truth detector (DUT RX counter + GPIO toggle) found it. Fix `ce8af5d`: `uart_set_line_inverse(..., UART_SIGNAL_INV_DISABLE)`; corrected gpio_remap.h/PIN_MAP.md/ADR-002 rev2/RUNBOOK Attempt 2 + hardware ADR-001 EC-5a (hw `95608de`, tag unmoved). Binary SHA-256 `d9e69962239935f496085e00cc046c55b76f5c1316ab71b638d91b28c60e7911b`. Lesson (RUNBOOK): build a ground-truth detector before swapping hardware. Adapter 2 (FT232/75176) delivered no signal in this setup (off critical path). No on-board 120 Ω termination (ADR-002) — external at bus ends for deployment. Phase 5 advance held pending explicit "begin Phase 5" signal. |
| 5 — LoRaWAN OTAA AS923 join + uplink | 2026-06-20 → 2026-06-22 | **PASS · signed off** | Branch `phase/p5-lorawan-join`, CI green (run `27961398800`: idf-build pristine / pre-commit / gitleaks / host-tests), merged to `main` `ed799c3`, tagged `phase-5-lorawan-green`, **user sign-off 2026-06-22**. Stack = **RadioLib 7.7.1** (managed component) + vendored **`EspHalS3.h`** S3 HAL (upstream EspHal is ESP32-classic-only, `#error`s on S3); `lora.cpp`/`lora.h` C API. **OTAA JOIN** AS923-1 (TCXO 1.8 V, DIO2 RF-switch) with DevNonce/session NVS persistence (DevNonce climbs monotonically; restored session skips re-join). Node provisioned via SCOMM CRM (`tools/provision_node.py`) into ChirpStack `10.10.8.140`. **KEY FINDING — data-uplink `-5 RADIOLIB_ERR_TX_TIMEOUT`:** root cause was the ESP-IDF default **100 Hz FreeRTOS tick** — `LoRaWANNode::transmitUplink` waits with `sleepDelay(toa)` then polls `digitalRead(DIO1)` only until `txEnd + scanGuard` (`scanGuard=10 ms` = 1 tick at 100 Hz); `vTaskDelay` quantization undershoots the air-time so the guard expires before TxDone → `-5`. Prior session misread it as DR/ToA (DR3/ADR-off didn't help) and as the DIO1 ISR (that trampoline fix `a2d8b92` was real but only fixes the **downlink** path). Fix `82d185d`: **`CONFIG_FREERTOS_HZ=1000`**. Bench HIL **4/4 consecutive `uplink OK`, zero `-5`** on project board MAC `3c:dc:75:6f:85:dc` (now `/dev/cu.usbmodem1401`). **END-TO-END CONFIRMED** in ChirpStack frame log: `UnconfirmedDataUp`, fPort 1, frm_payload `a500245a`, fCnt 98, ADR off, **SF9/BW125/CR4_5 = DR3** @ 923.4 MHz, `as923_1`, CRC_OK, RSSI −48 / SNR +12.2 via gw `ac1f09fffe1f340d`. **CI fix `e28fe51`:** pristine build had been red since RadioLib landed because `lora.cpp` included the gitignored `lora_credentials.h` (AppKey secret, generated from `firmware/.env`); now `__has_include` falls back to committed `lora_credentials.h.example` (placeholder, all-zero) → builds anywhere, secrets stay out of tree (verified by building with the real header moved aside). Also `4c37934`: `siot-mcp-gateway` (MCP proxy `10.10.8.113`) attached at project scope (`.mcp.json`, env-indirected token) + `firmware-knowledge` skills + `docs/MCP_GATEWAY.md`; CLAUDE.md drift fixed (SSH key path, token-in-file wording). ADRs: **ADR-003** (RadioLib stack, OQ-3 closed), **ADR-004** (AS923-1 sub-band, OQ-4 closed). Binary SHA-256 `38105b4eaa218685a218a2505b3d2af7c5de2cc4e3b9dfd03fe926e32f9b9f9a` (bench-verified 1 kHz build), `IDF_VER=v5.5.4-dirty` (local PARLIO mod, off-compile-path, Footnote 1). Lessons (RUNBOOK 5c): RadioLib LoRaWAN on ESP-IDF needs a 1 kHz tick; never push with red CI (caught at merge). Next = Phase 6 (Modbus RTU master + ADR-005 payload schema). |
| 6 — Modbus RTU master + telemetry → LoRaWAN (MFM384 / RS-FSJT) | 2026-06-20 → 2026-06-24 | **PASS · signed off** | Branch `phase/p6-modbus` (PR #1), CI green (run `28111619969`: pre-commit / idf-build pristine 4m55s / gitleaks / host-tests), merged to `main` `4a52614`, tagged `phase-6-modbus-green`, **user sign-off 2026-06-24**. **6a** (`efcebab`): pure RTU framing `modbus_rtu.{c,h}` (CRC16, build/parse, exceptions) + float32 `modbus_regs_to_f32` ABCD/CDAB; host ctest. **6b** (`2dfe864`,`a31e737`): on-target `modbus_master` (read txn + bus-scan + 1 Hz poll), `rs485` parity field; bench profiles. **Device pivot** (`1954673`): original RS-FSJT bring-up was blocked on a sensor↔CN1 *physical* join (not firmware); retargeted bring-up to a **SELEC MFM384** meter. **KEY FINDING — RS-485 silence = DE/RE wiring fault:** the meter returned 0 bytes at every baud/parity; firmware exonerated **5 ways** (Phase 4 echo + RAK3312 ref + ETS source match + **ETS `ModbusMaster` code run verbatim on our board, built via arduino-esp32 3.3.10 on IDF 5.5.4** + finally our own FW after the fix). Operator found a **DE/RE connection fault** (`146403f`) — the entire blocker; a direction-pin wiring fault presents identically to an unwired/unpowered slave (0 garbage at every baud). **MFM384 locked facts:** FC04 input registers, ETS voltage-first map (reg0=V1N…reg58=Total kWh), **CDAB** word order (NOT the RAK3312 "ABCD" note — proven by raw bytes), 9600 8N1 unit 1; V1N read ~50.5 V on a bench source. **RS-FSJT:** FC03 reg0, 4800 8N1, **scale raw/10 = m/s confirmed** by blow test (raw 33–58 ⇒ 3.3–5.8 m/s; `da19b28`). **6c** (`ea2ec53`): `components/payload` (pure, host-tested — exact-byte + signed + saturation) encodes the **ADR-005** compact versioned binary (3-byte header + fixed-point; MFM384 19 B, RS-FSJT 5 B, ≤ DR3 53 B); `components/meter` (real reads + deterministic sims); `app_main` field-app path (sample→encode→`lora_send`) with Kconfig sim toggle + device/unit/baud/interval; `tools/chirpstack_mfm384_decoder.js` (v4 codec); bench profiles `sim-mfm384`/`sim-rsfsjt`/`field-rsfsjt`. **END-TO-END CONFIRMED in ChirpStack** (`10.10.8.140`, as923_1, DR3): **RS-FSJT real** fCnt 434 payload `0102000028` → wind 0.40 m/s (`simulated:false`); **MFM384 simulated** fCnt 440 payload `010101…03B8` → V≈230.6/232.2/227.2, 5.1 kW, 1000.25 kWh (climbing), 50.01 Hz, 0.95 PF (`simulated:true`). Host ctest 3/3; on-target builds green (default real-MFM384, sim-mfm384, sim-rsfsjt, field-rsfsjt). Binary SHA-256 `661367b268697588741553349bf75a75658806ecc4b78227356df1df1198bbe6` (default build), `IDF_VER=v5.5.4-dirty` (local PARLIO mod, off-compile-path, Footnote 1). Lessons (RUNBOOK Phase 6): a DE/RE wiring fault = total-silence at every baud — suspect the direction pin early + run a 2nd proven master to exonerate firmware. **Follow-up (not blocking):** real-**MFM384 uplink** leg unexercised (meter not on home bench; read path proven) — run when the meter returns. |
| 7 — Production hardening (planning) | 2026-06-25 | **PLAN** | Branch `phase/p7-production-hardening` off clean `main` (`87332ca`); user "begin Phase 7" signal received. Phase 7 expanded from stub to a 5-sub-phase **gated plan** (rev8): **7a** watchdog + brownout + reset-cause log · **7b** light-sleep duty-cycle + average-current gate · **7c** dual-slot OTA + rollback · **7d** NVS-provisioned creds/config (compiled `lora_credentials.h` → fallback only) · **7e** integration soak + sign-off — each with its own HIL smoke gate + on-PASS/FAIL/rollback. OQ-9..12 opened (sleep strategy / OTA transport / provisioning transport / current target); ADR-006/007/008 reserved. Phase-7 guardrails added: watchdog-feed-in-main-loop-only, no-compiled-secrets-after-7d, OTA-partition-change = full-erase (gated + confirm-in-writing + image backup). **Execution held per sub-phase readiness — no firmware written or flashed yet.** (Presentation deck committed separately on `feat/presentation-dashboard`.) |

<!-- 2026-05-01: CLAUDE.md drafted, awaiting user review before Phase 1 execution. -->
<!-- 2026-05-01: rev2 — applied E2 (PSRAM=n), E3 (version pins), E4 (drop arduino-only PIO setting + partitions), E5 (pre-commit), E6 (drop ctest from Phase 1 gate), R1 (.env.example), R2 (strap-pin specifics), R4 (versioned snapshot), R5 (filename note expanded), R6 (parity check → Phase 4 / OQ-6). E1 applied as split-repo (reversal of prior monorepo direction — flagged). R3 declined: global ~/.claude/CLAUDE.md §11+§15 authoritatively says current MCP Pi Gateway IP is 192.168.20.150 (relocated from .15.150 on 2026-04-23). -->
<!-- 2026-05-01: rev3 — pin bumps approved per environmental audit. ESP-IDF v5.3.1 → v5.5.4 (already side-installed at ~/esp/esp-idf-v5.5.4/). gitleaks v8.21.x → v8.30.x (currently v8.30.1, already installed). clang-format LLVM 18 keg-only path documented. §8 MCP Pi gateway row strike. Phase 1 execution begins. -->
<!-- 2026-05-01: rev4 — PlatformIO dropped after Attempt 2 exposed the joint-pin inconsistency. Single canonical build path = ESP-IDF v5.5.4 via idf.py for both local and CI. §3 #6 rewritten to functional-equivalence (the byte-equivalent framing in earlier reviews was operator-introduced and not load-bearing in the contract). Guardrail #9 (gate-execution discipline) preserved. OQ-6 closed. New Pin-discipline lesson in RUNBOOK. CI matrix down to 4 jobs (no pio-build). -->
<!-- 2026-05-01: Phase 1 SIGN-OFF received. OQ-8 closed (LoRaWAN / Class A / AS923 / OTAA → ChirpStack 10.10.8.140 / RadioLib stack confirmed; ADR-003 still formalises in Phase 5). OQ-4 stays open. Hardware repo creation + GitHub remote both deferred per user sequencing argument (hardware repo created first, tagged, then firmware HARDWARE_REV.md pinned; cf. user message 2026-05-01). Schematic CLAUDE.md regeneration deferred to Phase 2 prep (option β). Phase 2 entry-criteria draft acknowledged but not yet locked into §5; will fold in when user signals "begin Phase 2". -->
<!-- 2026-05-01: rev5 — Phase 2 entry criteria locked (EC-1..EC-9). Renumbered per execution order (BOM consistency check now EC-3 before pin-map extraction EC-4). Five operator-facing revisions folded in vs the user's draft: (1) EC-1 verification uses fresh temp clone with trap-on-ERR forensics + cleanup-only-on-success — dev tree at ~/esp/esp-idf-v5.5.4/ never touched during verification; (2) apply-patches.sh dry-run output corrected ("Would apply" vs "Applied"); (3) apply-patches.sh adds IDF_PATH and PATCHES_DIR validity preconditions with clear errors; (4) apply-patches.sh uses BASH_SOURCE for SCRIPT_DIR resolution (symlink-safe); (5) apply-patches.sh uses LC_ALL=C sort for deterministic patch ordering across locales (load-bearing for >= 2 patches). Hardware repo gets a slim CLAUDE.md as part of EC-8 (cross-repo discipline: each repo owns its own gate). New Footnote 3 (cross-repo coupling) and Footnote 2 (tag naming convention) lock the split-repo discipline. New "aesthetic vs functional preference" lesson written to docs/RUNBOOK.md in companion commit (see git log fd36fce). -->
<!-- 2026-05-01: rev6 — path-conventions correction. EC-2..EC-9 "Smoke gate (hardware repo):" code blocks and body content references had the firmware-repo's `hardware/` prefix carried over inadvertently when paths were transcribed into hardware-repo gates. Hardware repo's actual layout is `schematic/`, `adr/`, `photos/` at root (no `hardware/` prefix); the prefix only exists inside the firmware repo where it identifies the canonical-content snapshot mirror. Rev6 drops the prefix from all hardware-repo gate paths and adds an explicit "Path conventions" preamble at the top of §5 Phase 2's Entry criteria block. EC-8a (local hw repo init at ~/Developer/projects/pcb-design/rak3112-rs485-node-hw/, commit f82702d) was already structured per the rev6 layout; rev6 makes the contract match what rev5's spec actually implied. Authored under operator delegation per the chat-side-author pattern (rev6 "I author" mirroring rev5 "you author"). Subsequent rev6 contract changes default back to operator authorship unless explicitly delegated. -->
<!-- 2026-05-01: rev7 — schematic version label aligned to V1.1 across the contract. EC-2 file existence inspection of the .epro on disk during Phase 2 EC-2 execution surfaced THREE separate version labels for the same content: filename "V1.1" (corrected when typo was fixed); .esch internal Version attribute "V1.0" → updated to "V1.1" via EasyEDA Pro re-export on 2026-05-01; firmware-repo Phase 1 snapshot mirror at hardware/schematic/v1.1/. Rev5/rev6 had used `schematic/v1.0/` for the Phase 2 hardware repo, inconsistent with all other identifiers. Rev7 aligns the contract to V1.1 across the board: schematic/v1.0/ → schematic/v1.1/ (12 occurrences across §2 layout, §5 EC-2 body+gate, §5 preamble); v1.0-archive → v1.1-archive (5 occurrences across §5 EC-8/EC-9 + Footnote 2 example). Also folded in: §2 layout diagram updated to use generic version-agnostic placeholders (v<X.Y>/, *.pdf, *.epro etc.) so future schematic-version bumps don't require editing the layout diagram; §5 EC-2 smoke gate's `test -f schematic.pdf` placeholder filename replaced with `ls *.pdf >/dev/null` glob (robust to filename changes — the actual current filename is `RAK3112 + RS485 P2P Node V1.1.pdf` with spaces, not the generic placeholder). Hardware repo content (schematic/v1.1/ archive, commit 970cdf0 on hw repo main) was already structured per the rev7 layout; rev7 makes the contract match what's already on disk. Authored under explicit operator delegation following rev6's chat-side-author pattern. -->
<!-- 2026-05-02: Phase 2 SIGN-OFF received. ADR-001 ACCEPTED at hw a0b002c, tagged adr-001-locked. EC-9 disposition for GPIO40/GPIO9: routed-but-unterminated in V1.1 (deliberate); V1.2 forward action tracked in rak3112-rs485-node-hw#1 (4-pin I²C expansion header). Phase 3 entry criterion: configure GPIO9 + GPIO40 as deliberate-floating at boot on V1.1 boards. HARDWARE_REV.md durable pin = adr-001-locked SHA. Five lessons in RUNBOOK; promotion candidates for global CLAUDE.md: gate-execution discipline, pin discipline, single canonical source, aesthetic-vs-functional preference, silicon-vendor abstraction layer, pre-commit-hook-checksum side-effects (= six total now; the rev3 R3 / rev5 push-back-1 / SHA-truncation incidents merged into the aesthetic-vs-functional lesson). Phase 3 advance held pending explicit "begin Phase 3" signal. -->
<!-- 2026-06-20: ADR-001 post-sign-off hygiene — code review found a residual <TBD> placeholder (a duplicate "## H1 header purpose (EC-5b)" stub still showing "Status: <TBD>" below the canonical resolved section), a technical violation of the EC-9 "free of <TBD> markers" precondition. Removed in hw repo main (merge 3cce0c6 / fix 65ddab0). adr-001-locked tag NOT moved (decision unchanged); HARDWARE_REV.md pin a0b002ca…8295 remains valid. See docs/RUNBOOK.md "Post-sign-off note — ADR-001 <TBD> hygiene". Phase 3 (first flash) execution begun on branch phase/p3-first-flash. -->
<!-- 2026-06-20: Phase 3 scaffolding added (branch phase/p3-first-flash): firmware/main/include/gpio_remap.h (single pin source of truth, seeded from ADR-001), docs/PIN_MAP.md (mirror), GPIO9/40 deliberate-floating init in app_main.c (ADR-001 EC-4/EC-9 carry-forward), scripts/flash.sh (canonical USB-CDC build/flash/monitor wrapper), docs/RUNBOOK.md Phase 3 bench checklist. Green build verified pre-flash; live first-flash held pending bench hardware-safety confirmation (H1 jumper + port + chip-id). -->
<!-- 2026-06-20: Phase 3 BOARD CORRECTION. Attempt 1 (and the State-block Phase 3 row above) flashed/verified on ESP32-S3R8 MAC 3c:dc:75:6f:89:24 — operator later identified that unit as NOT the project board (a second RAK3112 with pins not exposed). The actual project board is a different RAK3112, MAC 3c:dc:75:6f:85:dc (pins exposed), same native USB /dev/cu.usbmodem1301. Re-flashed byte-identical phase-3-first-flash-green firmware to the correct board and re-captured: identical full PASS (Found 8MB PSRAM device, Adding pool of 8192K, octal_psram vendor 0x0d AP / VCC 3V, SPI SRAM memory test OK, GPIO9/40 floating, 1 Hz heartbeat, no bootloop/brownout). Tag + sign-off stand (same firmware, same PASS). PROJECT FLASH-TARGET IDENTITY from Phase 4 onward = MAC 3c:dc:75:6f:85:dc. Lesson: with multiple same-silicon boards on the bench, match MAC to the known project unit, not just chip type. See docs/RUNBOOK.md Phase 3 Attempt 2. -->
<!-- 2026-06-25: rev8 — Phase 7 plan drafted on phase/p7-production-hardening (no code yet). Phase 7 stub → full gated plan: entry criteria + sub-phases 7a-7e each with a HIL smoke gate + on-PASS/FAIL/rollback; Phase-7-specific guardrails (watchdog-feed-in-main-loop-only, no-compiled-secrets-after-7d, OTA-partition-change=full-erase-gated-with-image-backup). OQ-9..12 added; ADR-006/007/008 reserved. Execution held per sub-phase readiness — blocking dependencies before live work: bench current-measurement setup (7b gate) and written confirmation before the first OTA erase (7c). Branch off clean main 87332ca; presentation deck parked on feat/presentation-dashboard (separate concern). -->

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
| OQ-7 | ESP-IDF local patches in `~/esp/esp-idf-v5.5.4/` (stashed during Phase 1 to ensure pristine `v5.5.4` build) — vendor properly into `firmware/esp-idf-patches/` with attribution and intent documentation. **Phase 2 entry criterion EC-1** (rev5). | Phase 2 / ADR-001 prereq · see §5 EC-1 for full disposition + verification routine | (a) `usb_types_ch9.h` (IAD descriptor size 9→8): likely upstream bug fix per USB 2.0 ECN — vendor as `0001-fix-iad-desc-size.patch`, file ESP-IDF GitHub issue, submit upstream PR if confirmed. (b) `esp_lcd_panel_io_parl.c` (PARLIO sample edge POS→NEG): **intent unclear — investigate before vendoring.** If intent cannot be determined, the patch is NOT vendored — stays in local ESP-IDF as personal modification, documented in `docs/RUNBOOK.md` under "ESP-IDF local modifications (not project-bound)". **Asymmetric outcome accepted in advance** (see §5 Footnote 1): local builds may produce `v5.5.4-dirty` while CI produces clean `v5.5.4`; both compile against the same project sdkconfig and same ESP-IDF version, so §3 #6 single-canonical-build-path invariant is not violated. |
| ~~OQ-8~~ | ~~Schematic title says "RS485 P2P Node V1.1" — LoRa P2P or LoRaWAN?~~ | **CLOSED 2026-05-01 (Phase 1 sign-off)** | **Resolved: LoRaWAN, Class A, AS923, OTAA join to existing CRM ChirpStack at `10.10.8.140`.** Reasoning: existing Southern IoT infrastructure (deployed ChirpStack, AS923 region configured, dashboards/alerts/FUOTA pipeline standardised, OTAA + DevEUI/AppKey provisioning already in place) is the load-bearing argument; the schematic's "P2P Node" string in the title is firmware-side naming and does not constrain the radio path. OQ-3 default (RadioLib) confirmed by user at sign-off; formal close still tracks via ADR-003 in Phase 5. The "P2P" in firmware identifiers should be suppressed when Phase 5 lands. OQ-4 (AS923 sub-band selection) remains open until Phase 5 against CRM region config. |
| OQ-9 | Light-sleep vs deep-sleep between samples? Deep-sleep loses RAM + the LoRa session (re-join each wake = costly airtime/energy). | Phase 7 / ADR-006 | Default **light-sleep** to preserve the NVS-restored RadioLib session and sub-second wake; revisit deep-sleep only if the OQ-12 budget can't be met at the chosen interval. |
| OQ-10 | OTA transport + partition scheme — WiFi/HTTPS OTA vs serial vs LoRaWAN **FUOTA**? | Phase 7 / ADR-007 | Default **WiFi/HTTPS OTA** + dual-slot rollback now; FUOTA (the standardised field path, see OQ-8 note) scoped as a follow-on once dual-slot + rollback are proven. |
| OQ-11 | Provisioning transport — serial/console NVS writer vs BLE vs WiFi SoftAP portal? | Phase 7 / ADR-008 | Default **serial/console NVS writer** (`tools/`) for bench, consuming CRM-minted DevEUI/AppKey; WiFi SoftAP portal (`.env.example` already reserves `WIFI_SSID/PASS`) scoped for field. |
| OQ-12 | Average-current target for the duty-cycled node (sets the 7b gate). | Phase 7 / ADR-006 | **TBD at entry** — propose ≤ 10 mA average at the 60 s interval as a starting target; confirm against RT6160 quiescent draw + intended battery/runtime budget before locking the 7b gate. |

---

## 8. Reference (read-only, do not modify)

- ChirpStack CRM (AS923): **`10.10.8.140`** — global §11. Load-bearing for Phase 5.
- Schematic source (V1.1, captured 2026-05-01): `~/Developer/projects/pcb-design/rs485-node/{RAK3112 + RS485 P2P Node V1.1 Schematic.pdf, RAK3112 + RS485 P2P Node V1.1.epro}`. The earlier UUID-named PDF and `ProPrj_RAK3312+…epro` files (with the RAK3312 typo) were superseded between Phase 0 discovery and Phase 1 scaffold execution; the new filenames carry the corrected `RAK3112` and the explicit `V1.1` revision tag.
- Hardware repo (planned): `~/Developer/projects/pcb-design/rak3112-rs485-node-hw/` · `rnd-southerniot/rak3112-rs485-node-hw`.

> **Footnote on schematic provenance.** The Phase 0 BOM extraction was performed against the earlier `ProPrj_RAK3312+ RS485_2026-05-01.epro` filename and confirmed the placed module as `RAK3112-9-SM-I` (BOM authoritative). The current filenames `RAK3112 + RS485 P2P Node V1.1.{epro,pdf}` correct the typo and add an explicit `V1.1` revision label. The "P2P Node" string in the title may indicate **LoRa P2P** rather than LoRaWAN — flagged as **OQ-8** for Phase 2 clarification (impacts stack choice in OQ-3 and the entire LoRaWAN narrative if "P2P" is the intended deployment model).

---

**End of contract rev2. Phase 1 will not begin until the user has read this document and replied with explicit approval (or further edits).**
