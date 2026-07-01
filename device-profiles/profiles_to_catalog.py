#!/usr/bin/env python3
"""profiles_to_catalog.py — generate the top-level CRM sensor catalog from the wire profiles.

Single source of truth: device-profiles/profiles/*.json (the full wire profiles consumed by
profile_to_blob.py / profile_to_decoder.py / the firmware). This script derives the summary catalog
device-profiles/<profile_id>.json that the FastAPI service reads (api/services/profiles.py globs
device-profiles/*.json → GET /v1/sensors). Generating it means the catalog can never drift from the
profiles (previously two hand-maintained sets disagreed: MFM384 payloadBytes 19 vs 39).

  python3 profiles_to_catalog.py            # regenerate the catalog files
  python3 profiles_to_catalog.py --check    # fail if any catalog file is stale (for CI)

Catalog schema (must match api/models.py Sensor, minus the computed `flashable`):
  profileKey, displayName, manufacturer, model, deviceByte,
  modbus{baud,parity,stopBits,functionCode,wordOrder}, payloadBytes, isActive
`flashable` is intentionally NOT emitted — the API computes it from compiled_readers.json (D-01).
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROFILES_DIR = HERE / "profiles"


def catalog_entry(p: dict) -> dict:
    dev, bus, dft, pl = p["device"], p["bus"], p["defaults"], p["payload"]
    display = dev.get("display_name") or f"{dev['manufacturer']} {dev['model']}"
    return {
        "_generated_by": "device-profiles/profiles_to_catalog.py — do not hand-edit",
        "profileKey": p["profile_id"],
        "displayName": display,
        "manufacturer": dev["manufacturer"],
        "model": dev["model"],
        "deviceByte": dev["type_byte"],
        "modbus": {
            "baud": bus["baud"],
            "parity": bus["parity"],
            "stopBits": bus["stop_bits"],
            "functionCode": dft["function_code"],
            "wordOrder": dft["word_order"],
        },
        "payloadBytes": pl["total_len"],
        "isActive": dev.get("is_active", True),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate the CRM sensor catalog from wire profiles.")
    ap.add_argument("--check", action="store_true", help="fail if any catalog file is stale")
    args = ap.parse_args()

    stale = []
    written = []
    for pf in sorted(PROFILES_DIR.glob("*.json")):
        prof = json.loads(pf.read_text())
        entry = catalog_entry(prof)
        out = HERE / f"{prof['profile_id']}.json"
        text = json.dumps(entry, indent=2) + "\n"
        if args.check:
            if not out.exists() or out.read_text() != text:
                stale.append(out.name)
        else:
            out.write_text(text)
            written.append(out.name)

    if args.check:
        if stale:
            print("STALE catalog files (run profiles_to_catalog.py): " + ", ".join(stale),
                  file=sys.stderr)
            return 1
        print(f"catalog up to date ({len(list(PROFILES_DIR.glob('*.json')))} profiles)")
        return 0

    print(f"generated {len(written)} catalog files: {', '.join(written)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
