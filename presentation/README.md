# RAK3112 RS-485 Node — Workflow Presentation

A single-file, web-based presentation of the **full product workflow** for the
`rak3112-rs485-node`, aimed at **non-technical staff** (operations, sales, marketing).

It walks four stages, **weighted toward the CRM**:

1. **CRM project initialization** — onboard client **Careflow**, mint the device, and trigger
   network registration through the SCOMM CRM workflow.
2. **Firmware** — what the device does and how it's built & tested, in plain language.
3. **ChirpStack** — the device joins the network over the air.
4. **Live uplinks** — a real-time dashboard of decoded telemetry.

## How to present

Just open the file — **no install, no build, no internet required**:

```bash
open presentation/index.html      # macOS
# or double-click index.html in Finder / drag it into any browser
```

Controls (bottom bar):

| Control | Action |
|---|---|
| **Prev / Next** | Step between the four scenes |
| **Play / Pause** | Toggle auto-advance (starts on, CRM scene runs longest) |
| **Scene dots** | Jump straight to any stage |
| **Replay ⇄ Live** | Switch data source (see below) |
| Keyboard | `←` / `→` navigate · `Space` play/pause |

Best viewed full-screen (browser full-screen, e.g. `⌃⌘F`). Designed for a 1080p projector
and ordinary laptops.

## White-label naming

The deck is presented in **Careflow** customer branding. For the audience-facing view only:

| Shown on screen | Actual technology (engineering record) |
|---|---|
| **Careflow** node | RAK3112 module (ESP32-S3 + SX1262) |
| **SCOMM** network / radio | LoRaWAN (AS923) over the SX1262 LoRa radio |

These are display substitutions; the firmware, `CLAUDE.md`, and ADRs keep the real technical
names. ChirpStack (the network server) is shown under its own name.

Scene 1's CRM walkthrough fills a **minimal form per section** (the key fields submitted at each
step) and has **on-page controls** — `‹` / `⏸` / `›` next to the form title — so a presenter can
pause and step through the provisioning at their own pace. These mirror `tools/provision_node.py`.

## What's real vs. demo

Every telemetry number is **real data captured by this project** (see `CLAUDE.md` State block
and `docs/RUNBOOK.md`):

- **Wind (RS-FSJT-N01):** payload `0102000028`, fCnt 434 → **0.40 m/s** — a real sensor read.
- **Power meter (SELEC MFM384):** 230.6 / 232.2 / 227.2 V, 5.1 kW, 1000.25 kWh, 50.01 Hz, 0.95 PF
  — clearly badged **SIMULATED**, because these were produced by the on-device simulator to
  validate the codec (the physical meter wasn't on the bench). The badge comes from the payload's
  own `simulated` flag — we show it honestly.
- **Radio link:** 923.4 MHz (AS923-1), DR3 (SF9/BW125), RSSI −48 dBm, SNR +12.2 dB, gateway
  `ac1f09fffe1f340d`.
- The CRM stepper mirrors `tools/provision_node.py` exactly; the **DevEUI is a labeled demo
  value** and the **AppKey is never shown** (rendered as vaulted), matching the repo's
  secrets discipline.

The hex payloads in the dashboard are encoded live by JS that mirrors
`firmware/components/payload/payload.c` and decoded by the same logic as
`tools/chirpstack_mfm384_decoder.js`, so what's on screen is internally consistent.

## Live mode (optional, best-effort)

By default the deck runs in **Replay** — fully self-contained. The **Live** toggle is a
best-effort overlay:

1. `cp presentation/config.js.example presentation/config.js`
2. Edit `config.js` to point `uplinkUrl` at an internal HTTP endpoint that returns the latest
   decoded uplink as JSON, and set a `token` if needed.
3. On the internal network / VPN, click **Live**.

Caveats (also in `config.js.example`): a `file://` page can't reach ChirpStack's gRPC API
directly and will hit CORS — `uplinkUrl` must be a small HTTP sink you expose (ChirpStack HTTP
integration, MQTT→HTTP bridge, or a proxy in front of `10.10.8.140`). If anything fails
(offline, CORS, auth, infra down), the deck shows a brief banner and **stays on replay** — live
never blocks the presentation. `presentation/config.js` is **gitignored** (it may hold a token).

## Files

| File | Purpose |
|---|---|
| `index.html` | The entire presentation — HTML + CSS + JS + embedded replay data, zero dependencies |
| `config.js.example` | Template for optional live mode (copy to `config.js`, gitignored) |
| `README.md` | This file |
