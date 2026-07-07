---
name: install-chirpstack-decoder
description: Install a product's JS payload codec onto a ChirpStack device-profile so uplinks decode to engineering values (temp/pressure, Modbus registers). Find-or-creates a product-specific device-profile, installs the JS codec, and optionally repoints a device at it — idempotent, over gRPC-web. Use when a device's uplinks arrive raw/undecoded, or when onboarding a new product's device-profile. Prompts for dev vs production ChirpStack; never clobbers another product's codec.
---

# install-chirpstack-decoder

Installs a product's JS decoder onto a **product-specific** ChirpStack device-profile (not the shared
one — never clobber another product's Modbus/I2C codec), then optionally repoints a device at it.
Idempotent, gRPC-web. Driver: `tools/install_decoder.py`.

> **Two stacks — prompt first.** Dev `http://10.10.8.140:8080` vs production
> `https://chirpstack.siot.solutions`. Uses the shared gRPC-web client (`_shared/cs_grpcweb`) +
> `chirpstack-api`. Dev token lives in the `cs-dev-key` Apple Note → `~/.config/siot/chirpstack-dev.env`;
> production token in `~/.config/siot/chirpstack.env`. **Never write production without confirmation.**

## When to use

- A device's uplinks show up in ChirpStack **raw / with an empty decoded `object`**.
- Onboarding a new product: give it its own device-profile carrying the right codec.
- After [provision-node](../provision-node/SKILL.md), before declaring the decode verified.

## Prerequisites

- The product's decoder `.js` (careflow `tools/chirpstack_mfm384_decoder.js` /
  `device-profiles/chirpstack_fleet_decoder.js`; senseflow repo `device-profiles/chirpstack_senseflow_decoder.js`).
- `CS_BASE`, `CS_API_TOKEN` (or admin login), `CS_TENANT_ID` in the environment (source the stack's
  `~/.config/siot/chirpstack*.env`). Runs in the skill venv (`.claude/skills/.v` — has `chirpstack-api`).

## Workflow

```bash
source .claude/skills/.v/bin/activate
set -a && . ~/.config/siot/chirpstack-dev.env && set +a          # dev; or chirpstack.env for prod (confirm!)

# dry-run first — shows the create/update + repoint plan without writing
python3 tools/install_decoder.py --product senseflow --repoint-deveui <deveui> --dry-run

# apply
python3 tools/install_decoder.py --product senseflow --repoint-deveui <deveui>
```

It (1) find-or-creates a device-profile named `<product>-...-AS923` (AS923, OTAA 1.0.3, JS codec),
(2) verifies `runtime=JS` + script length, (3) optionally repoints the device — leaving any shared /
other-product device-profile untouched.

## Verify

Read the decoded object from the device event stream (`InternalService.StreamDeviceEvents` →
`LogItem.body` JSON has `object`) — a fresh uplink after the install should decode to the expected
fields (e.g. `temp_C` / `pressure_hPa`). The **first few** post-change uplinks may show an empty
`object` while ChirpStack's device-profile codec cache warms up — then it goes steady.

## Guardrails

- **Prompt for dev vs production**; production is Fahim's cluster — read-only unless explicitly told.
- **Never clobber another product's codec** — always a product-specific device-profile, then repoint.
- Verify offline first: run the decoder on the raw payload to confirm it produces the expected values
  before blaming the install.
