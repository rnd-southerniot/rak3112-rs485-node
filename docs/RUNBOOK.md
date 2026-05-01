# Runbook

Operational log for `rak3112-rs485-node` firmware. Append-only.

## Phase 1 attempts

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
