# ADR-004: AS923 sub-band = AS923-1 (Bangladesh)

## Status
ACCEPTED-pending-sign-off · Phase 5 · retires OQ-4

## Context

Phase 5 OTAA join must use the frequency plan the CRM/ChirpStack at `10.10.8.140` is
configured for. OQ-4 required confirming the AS923 sub-band against that region config rather
than hardcoding.

## Decision

**Use AS923-1** (the "AS923" plan with frequency offset 0).

Confirmed against the deployment:
- CRM `GET /chirpstack/status` → device profile `80b53d57-cb51-457b-bd75-016fcde4851e`
  (`siot-as923-otaa-classA`), application `9dcd1954-498e-4513-b58b-3a1f876025ae`, tenant
  `f307e431-…`. Our device `3cdc75fffe6f85dc` is registered against exactly this AS923 OTAA
  profile (provisioned via the CRM workflow — RUNBOOK Phase 5b).
- SCOMM CRM integration handoff §7: **"Region: AS923-1 (Bangladesh)."**

Frequency plan (AS923-1): default join channels **923.2 MHz** and **923.4 MHz**; RX2 **923.2 MHz
@ DR2 (SF10BW125)**; freq offset 0. **JoinEUI = `0000000000000000`** (CRM/ChirpStack default;
ChirpStack does not filter on JoinEUI by default).

RadioLib firmware (ADR-003) selects band **`AS923`** (offset 0 = AS923-1). If RadioLib's default
AS923 channel mask differs from the ChirpStack region channels, align the channel mask /
sub-band in the join config.

## Open items (validate during the join, Phase 5b)
- **Dwell time.** AS923 has a 400 ms uplink dwell-time flag that constrains DR/payload
  (RadioLib issue #1180). Validate the join + first uplink against the ChirpStack region
  `dwellTime` setting; if dwell-time is on, cap DR/payload accordingly.
- Confirm the join actually uses 923.2/923.4 (vs RadioLib defaults) by watching the ChirpStack
  gateway frames on the first join attempt.

## References
- CRM `GET /chirpstack/status` (live, 2026-06-20); device `3cdc75fffe6f85dc` registered.
- SCOMM CRM integration handoff note §7 (AS923-1, Bangladesh).
- ADR-003 (RadioLib); firmware RUNBOOK Phase 5b (provisioning record).
- ChirpStack region config: `rnd-southerniot/siot-crm-review` (chirpstack/ + provision-chirpstack.js).

## Sign-off
Signed-off-by: <pending> · 2026-06-20
