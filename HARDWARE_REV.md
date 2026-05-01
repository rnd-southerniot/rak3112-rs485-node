# Hardware revision pin

This firmware repo is paired with a hardware revision tracked in a separate hardware repo. Update this file every time the firmware repo is intentionally re-paired with a new hardware tag.

## Machine-parseable pin (for §5 EC-8 smoke gate)

The CLAUDE.md §5 EC-8 smoke gate parses the three lines below via `grep -qE '^pinned_sha:\s+[a-f0-9]{40}$' HARDWARE_REV.md` and `awk '/^pinned_sha:/ {print $2}'`. **Do not reformat without updating the gate.**

```
pinned_sha:  8554115b8dadee593b615d9b47baad67d8ac66d6
pinned_tag:  (interim — replaced by adr-001-locked tag SHA at EC-9 sign-off)
pinned_date: 2026-05-02
```

## Human-readable summary

| Field | Value |
|---|---|
| Hardware repo (canonical) | [`rnd-southerniot/rak3112-rs485-node-hw`](https://github.com/rnd-southerniot/rak3112-rs485-node-hw) (created 2026-05-02) |
| Local working tree | `~/Developer/projects/pcb-design/rak3112-rs485-node-hw/` |
| Pinned revision label | `V1.1` (EasyEDA Pro project, captured 2026-05-01; re-exported with corrected internal Version ATTR 2026-05-01) |
| Pinned tag | **interim — `main` tip SHA at EC-8b publish.** Replaced by `adr-001-locked` tag SHA at EC-9 sign-off. |
| Pinned commit SHA | `8554115b8dadee593b615d9b47baad67d8ac66d6` (full 40-char hex, no truncation per "aesthetic vs functional preference" lesson) |
| Date pinned | 2026-05-02 (Phase 2 EC-8b publish) |
| Firmware phase when pinned | Phase 2 EC-8b (interim pin; first formal `adr-001-locked` SHA pin lands at EC-9) |

## Pin lifecycle

1. **Phase 1 / pre-EC-8b:** placeholder (`Pinned tag: TBD`, `Pinned commit SHA: TBD`). Hardware repo did not exist at GitHub.
2. **Phase 2 EC-8b (now):** interim pin on hw-repo `main` tip SHA. Hardware repo exists at GitHub but ADR-001 is still `DRAFT` (not `adr-001-locked`).
3. **Phase 2 EC-9 (next):** replaced by `adr-001-locked` tag SHA after operator signs off ADR-001 in the hardware repo. The interim `main`-tip pin is superseded; the tag pin is the durable record.
4. **Future hardware revisions:** each new `adr-NNN-locked` tag (or schematic-version-archive tag if pinning to a schematic snapshot rather than an ADR) replaces the previous pin via a new `HARDWARE_REV.md` commit.

The `hardware/schematic/v1.1/` snapshot inside this firmware repo (Phase 1 mirror) is a convenience copy of the canonical V1.1 schematic now living at the hardware repo's `schematic/v1.1/`. The canonical content is in the hardware repo; this mirror exists so the firmware repo builds context-complete on a clean checkout. See firmware CLAUDE.md §2 layout block + Footnote 3 (cross-repo coupling).
