#!/usr/bin/env python3
"""Validate device profiles against schema.json + cross-checks.

Runs structural checks with no third-party deps; if `jsonschema` is installed it
also does full JSON-Schema validation. Exit 0 = all good, non-zero = failures.

Checks beyond the schema:
  - every payload field references an existing measurand key
  - payload.device_byte == device.type_byte
  - device type_byte is unique across all profiles (unified registry)
  - payload fits AS923 DR3 (total_len <= 53) and matches max(offset+size)+header
  - scales are non-zero
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCHEMA = HERE / "schema" / "profile.schema.json"
PROFILES = sorted((HERE / "profiles").glob("*.json"))
HEADER_LEN = 3
DR3_MAX = 53
ENC_SIZE = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4}


def main() -> int:
    errors: list[str] = []
    type_bytes: dict[int, str] = {}

    try:
        import jsonschema  # type: ignore

        schema = json.loads(SCHEMA.read_text())
        validator = jsonschema.Draft202012Validator(schema)
    except ImportError:
        validator = None
        print("note: jsonschema not installed — running structural checks only")

    if not PROFILES:
        print("ERROR: no profiles found")
        return 2

    for path in PROFILES:
        p = json.loads(path.read_text())
        name = path.name

        if validator is not None:
            for e in validator.iter_errors(p):
                errors.append(f"{name}: schema: {e.message} (at {list(e.path)})")

        keys = {m["key"] for m in p.get("measurands", [])}
        dev = p.get("device", {})
        pl = p.get("payload", {})

        tb = dev.get("type_byte")
        if tb in type_bytes:
            errors.append(f"{name}: type_byte {tb} collides with {type_bytes[tb]}")
        elif tb is not None:
            type_bytes[tb] = name

        if pl.get("device_byte") != tb:
            errors.append(f"{name}: payload.device_byte {pl.get('device_byte')} != device.type_byte {tb}")

        max_end = HEADER_LEN
        for f in pl.get("fields", []):
            if f["key"] not in keys:
                errors.append(f"{name}: payload field '{f['key']}' has no matching measurand")
            if f.get("scale", 0) == 0:
                errors.append(f"{name}: payload field '{f['key']}' has zero scale")
            size = ENC_SIZE.get(f["encoding"], 0)
            max_end = max(max_end, HEADER_LEN + f["offset"] + size)

        total = pl.get("total_len", 0)
        if total > DR3_MAX:
            errors.append(f"{name}: total_len {total} exceeds AS923 DR3 cap {DR3_MAX}")
        if total != max_end:
            errors.append(f"{name}: total_len {total} != computed body end {max_end}")

        for m in p.get("measurands", []):
            if m.get("scale", 0) == 0:
                errors.append(f"{name}: measurand '{m['key']}' has zero scale")

        print(f"checked {name}: {len(p.get('measurands', []))} measurands, "
              f"{len(pl.get('fields', []))} payload fields, total_len {total}")

    if errors:
        print("\nFAILURES:")
        for e in errors:
            print(f"  - {e}")
        return 1
    print(f"\nOK — {len(PROFILES)} profiles valid; type_bytes {sorted(type_bytes)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
