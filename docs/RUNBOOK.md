# Runbook

Operational log for `rak3112-rs485-node` firmware. Append-only.

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
