#!/usr/bin/env python3
"""profile_to_blob.py — serialize a device profile JSON into the firmware's NVS blob (ADR-006).

Emits the exact byte format consumed by dp_deserialize() (firmware/components/device_profile):
versioned, big-endian, CRC-16/MODBUS trailer, floats as IEEE-754 bit patterns. The CRM pushes the
hex output to a node with `prov-profile <hex>`.

  python3 profile_to_blob.py profiles/selec-mfm384.json          # -> hex blob
  python3 profile_to_blob.py profiles/selec-mfm384.json --command # -> "prov-profile <hex>"
  python3 profile_to_blob.py --ctest-vector                       # -> hex for the C cross-check

The byte layout and enum codings here MUST match device_profile.h/.c exactly. The C host test
asserts dp_serialize() of the same profile equals the --ctest-vector output (cross-language check).
"""
import argparse
import json
import struct
import sys

BLOB_VERSION = 1
DTYPE = {"u16": 0, "i16": 1, "u32": 2, "i32": 3, "float32": 4}
WORD = {"ABCD": 0, "CDAB": 1, "BADC": 2, "DCBA": 3}
ENC = {"u8": 0, "i8": 1, "u16": 2, "i16": 3, "u32": 4, "i32": 5}


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF


def build_blob(p: dict) -> bytes:
    """p: a dict with the normalized profile fields (see profile_from_json / ctest_vector)."""
    b = bytearray()
    b.append(BLOB_VERSION)
    b.append(p["device_byte"])
    b += struct.pack(">I", p["baud"])
    b.append(ord(p["parity"]))
    b.append(p["stop_bits"])
    b.append(p["default_fc"])
    b.append(WORD[p["default_word"]])
    b.append(p["scan_fc"])
    b += struct.pack(">H", p["scan_reg"])
    b += struct.pack(">H", p["scan_qty"])
    b.append(p["total_len"])
    b.append(len(p["meas"]))
    b.append(len(p["fields"]))
    for m in p["meas"]:
        b += struct.pack(">H", m["reg"])
        b.append(m["fc"])
        b.append(DTYPE[m["type"]])
        b.append(WORD[m["word"]])
        b += struct.pack(">f", m["scale"])
        b += struct.pack(">f", m["offset"])
    for f in p["fields"]:
        b.append(f["value_index"])
        b.append(f["offset"])
        b.append(ENC[f["enc"]])
        b += struct.pack(">f", f["scale"])
    b += struct.pack(">H", crc16_modbus(bytes(b)))
    return bytes(b)


def profile_from_json(doc: dict) -> dict:
    """Normalize a device-profiles/*.json document into build_blob()'s input."""
    bus, dft, scan, pl = doc["bus"], doc["defaults"], doc["scan"], doc["payload"]
    meas = doc["measurands"]
    key_index = {m["key"]: i for i, m in enumerate(meas)}
    return {
        "device_byte": doc["device"]["type_byte"],
        "baud": bus["baud"],
        "parity": bus["parity"],
        "stop_bits": bus["stop_bits"],
        "default_fc": dft["function_code"],
        "default_word": dft["word_order"],
        "scan_fc": scan["function_code"],
        "scan_reg": scan["register"],
        "scan_qty": scan["quantity"],
        "total_len": pl["total_len"],
        "meas": [
            {
                "reg": m["register"],
                "fc": m["function_code"],
                "type": m["type"],
                "word": m.get("word_order", dft["word_order"]),
                "scale": float(m["scale"]),
                "offset": float(m.get("offset", 0.0)),
            }
            for m in meas
        ],
        "fields": [
            {
                "value_index": key_index[f["key"]],
                "offset": f["offset"],
                "enc": f["encoding"],
                "scale": float(f["scale"]),
            }
            for f in pl["fields"]
        ],
    }


def ctest_vector() -> dict:
    """The exact 6-measurand MFM384-shaped profile used by tests/host/test_device_profile.c."""
    meas = [
        {"reg": r, "fc": 4, "type": "float32", "word": "CDAB", "scale": 1.0, "offset": 0.0}
        for r in (0, 2, 4, 44, 56, 58)
    ]
    fields = [
        {"value_index": 0, "offset": 0, "enc": "u16", "scale": 10.0},
        {"value_index": 1, "offset": 2, "enc": "u16", "scale": 10.0},
        {"value_index": 2, "offset": 4, "enc": "u16", "scale": 10.0},
        {"value_index": 3, "offset": 6, "enc": "i16", "scale": 10.0},
        {"value_index": 4, "offset": 8, "enc": "u16", "scale": 100.0},
        {"value_index": 5, "offset": 10, "enc": "u32", "scale": 100.0},
    ]
    return {
        "device_byte": 0x01, "baud": 9600, "parity": "N", "stop_bits": 1,
        "default_fc": 4, "default_word": "CDAB", "scan_fc": 3, "scan_reg": 6, "scan_qty": 1,
        "total_len": 17, "meas": meas, "fields": fields,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Serialize a device profile JSON to an NVS blob (hex).")
    ap.add_argument("profile", nargs="?", help="path to a device-profiles/*.json")
    ap.add_argument("--command", action="store_true", help="print 'prov-profile <hex>'")
    ap.add_argument("--ctest-vector", action="store_true", help="emit the C cross-check vector")
    args = ap.parse_args()

    if args.ctest_vector:
        blob = build_blob(ctest_vector())
    elif args.profile:
        blob = build_blob(profile_from_json(json.loads(open(args.profile).read())))
    else:
        ap.error("give a profile JSON path or --ctest-vector")
        return 2

    hexstr = blob.hex()
    print(f"prov-profile {hexstr}" if args.command else hexstr)
    print(f"# {len(blob)} bytes, crc16=0x{blob[-2] << 8 | blob[-1]:04x}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
