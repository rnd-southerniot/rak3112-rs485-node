---
name: rak-lorawan-knowledge
description: Query the firmware-knowledge MCP server (via siot-mcp-gateway) for proven RAK LoRaWAN OTAA AS923-1 + ChirpStack + auto-provisioning + FUOTA guidance. Use when debugging Phase 5 join/uplink (e.g. the "-5 after TxDone" data-uplink issue), wiring up OTAA provisioning, or choosing AS923 sub-band/channel-plan for the rak3112-rs485-node.
---

# rak-lorawan-knowledge

Consults the `firmware-knowledge` upstream behind `siot-mcp-gateway` — a knowledge base whose
LoRaWAN material was validated **end-to-end with a real OTAA join + confirmed uplink** (AS923-1,
ChirpStack v4). It is RAK4630/nRF52840-authored, but the OTAA / AS923 / ChirpStack /
provisioning content transfers to this RAK3112/ESP32-S3 node. Use it to ground Phase 5 work in
something proven rather than re-deriving the channel plan or provisioning flow.

Context: [docs/MCP_GATEWAY.md](../../../docs/MCP_GATEWAY.md) · Phase 5 in [CLAUDE.md](../../../CLAUDE.md) §5.

## Pre-flight

Run [mcp-gateway-health](../mcp-gateway-health/SKILL.md). If blocked (token/VM), stop and report.

## How to call

The gateway is a proxy: surface `call_upstream_tool` via `ToolSearch` (`select:call_upstream_tool`),
then call upstream tools with `server_name:"firmware-knowledge"`:

```
call_upstream_tool(server_name="firmware-knowledge", tool_name="<tool>", arguments={...})
```

`firmware-knowledge` tools: `list_docs`, `get_doc` (arg: `path`), `search` (arg: `query`),
`get_provisioning_guide`, `get_sop_guide`, `get_gate_status`, `get_pin_map`.

## Recipes

- **OTAA provisioning flow:** `get_provisioning_guide` → the CRM→ChirpStack→firmware workflow
  (DevEUI/AppKey generation, NVS injection). Map to this repo's `.env` provisioning (Phase 7).
- **AS923 channel plan / sub-band (OQ-4):** `get_doc path="docs/04-lorawan-chirpstack-fuota-plan.md"`
  for the OTAA AS923-1 + ChirpStack v4 plan; cross-check against the CRM region config.
- **Bring-up / join checklist:** `get_doc path="docs/03-bringup-checklist.md"`.
- **Debugging the Phase 5 "-5 after TxDone":** `search query="uplink"` / `search query="TxDone"`,
  and read the provisioning + bring-up docs for the confirmed-working join/uplink sequence and
  RX-window / class-A timing expectations. Compare against the RadioLib SX1262 path to find
  where this node diverges.

## Caveats

- Knowledge is RAK4630-centric — treat radio-stack specifics (RadioLib vs the RAK BSP) as
  reference, not drop-in. The LoRaWAN/region/provisioning concepts transfer; pin/BSP details
  may not.
- Server-side **device state** (a specific DevEUI's join/uplink frames on ChirpStack) is NOT
  available through this gateway — that's the CRM at `10.10.8.140`, reached separately. This
  skill is for *knowledge*, not live device telemetry.

## Output

A short synthesis tying the retrieved guidance to the current Phase 5 symptom and the next
concrete firmware action — with the source doc path(s) cited.
