# Hardware revision pin

This firmware repo is paired with a hardware revision tracked in a separate hardware repo. Update this file every time the firmware repo is intentionally re-paired with a new hardware tag.

| Field | Value |
|---|---|
| Hardware repo | `rnd-southerniot/rak3112-rs485-node-hw` (planned — not yet created) |
| Pinned revision label | `V1.1` (from EasyEDA Pro project filename, captured 2026-05-01) |
| Pinned tag | TBD (formal git tag pending hardware-repo creation) |
| Pinned commit SHA | TBD |
| Date pinned | 2026-05-01 (Phase 0 discovery / Phase 1 scaffold) |
| Firmware phase when pinned | Phase 1 (snapshot-only; first formal git-SHA pin lands in Phase 2 after ADR-001 sign-off) |

The current `hardware/schematic/v1.1/` snapshot in this repo was captured 2026-05-01 from `~/Developer/projects/pcb-design/rs485-node/` (EasyEDA Pro project `RAK3112 + RS485 P2P Node V1.1.epro`). When the hardware repo is created and tagged (e.g. `v1.1-schematic-rev1`), update this file with the resolved values and add a corresponding `hardware/schematic/v1.x/` snapshot for any subsequent revisions.
