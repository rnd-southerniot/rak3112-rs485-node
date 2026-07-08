"""profile_emitter.py — turn a confirmed candidate + operator input into a Careflow device profile.

Assembles a schema-conformant ``device-profiles/profiles/<id>.json``, allocates the next free
``type_byte``, packs an ADR-005 payload within the 53-byte AS923 DR3 budget, then **shells out to the
committed generators** (``validate_profiles.py`` / ``profile_to_blob.py`` / ``profile_to_decoder.py``
/ ``profiles_to_catalog.py``) — never reimplementing the blob/decoder/catalog logic.

Refuses to emit a measurand the operator has not named (no auto-naming): semantics come from the
human-in-the-loop step, structure from inference.
"""

from __future__ import annotations

import glob
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from typing import Optional

from .models import CandidateMeasurand, CandidateProfile, DType, OperatorInput, WordOrder

HEADER_LEN = 3
DR3_MAX = 53
ENC_SIZE = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4}
_CATEGORIES = {"energy-meter", "generator-controller", "wind-sensor", "other"}


@dataclass
class ArtifactSet:
    profile_id: str
    profile_path: str
    blob_hex: str
    decoder_path: str


class EmitError(RuntimeError):
    pass


# --- helpers -----------------------------------------------------------------------------------


def _slug(s: str) -> str:
    s = re.sub(r"[^a-z0-9]+", "-", s.strip().lower()).strip("-")
    return re.sub(r"-{2,}", "-", s)


def _key(s: str) -> str:
    return re.sub(r"_{2,}", "_", re.sub(r"[^a-z0-9_]+", "_", s.strip().lower())).strip("_")


def next_free_type_byte(profiles_dir: str) -> int:
    used = set()
    for path in glob.glob(os.path.join(profiles_dir, "*.json")):
        try:
            used.add(json.loads(open(path).read())["device"]["type_byte"])
        except (KeyError, ValueError, OSError):
            continue
    n = 1
    while n in used:
        n += 1
    if n > 255:
        raise EmitError("type_byte registry exhausted (>255)")
    return n


def apply_edits(candidate: CandidateProfile, op: OperatorInput) -> None:
    """Merge operator MeasurandEdits (keys/names/types/scale/include) onto the candidate in place."""
    idx = {(m.fc, m.addr): m for m in candidate.measurands}
    for e in op.measurand_edits:
        m = idx.get((e.fc, e.addr))
        if m is None:
            continue
        if e.key is not None:
            m.key = _key(e.key)
        if e.name is not None:
            m.name = e.name
        if e.unit is not None:
            m.unit = e.unit
        if e.dtype is not None:
            m.dtype = DType(e.dtype)
        if e.word_order is not None:
            m.word_order = WordOrder(e.word_order)
        if e.scale is not None:
            m.scale = e.scale
        if e.offset is not None:
            m.offset = e.offset
        if e.accumulating is not None:
            m.accumulating = e.accumulating
        if e.include_in_payload is not None:
            m.include_in_payload = e.include_in_payload


def _payload_encoding(m: CandidateMeasurand) -> str:
    if m.accumulating:
        return "u32"
    if m.sample_value is not None and m.sample_value < 0:
        return "i16"
    return "u16"


def pack_payload(measurands: list[CandidateMeasurand], budget: int = DR3_MAX - HEADER_LEN):
    """Greedily pack included+named measurands into ADR-005 fields within the byte budget.

    Returns (fields, total_len, dropped_keys). Fields are laid out contiguously; total_len is the
    ``max(3 + offset + size)`` the firmware/validator expect.
    """
    fields: list[dict] = []
    dropped: list[str] = []
    offset = 0
    for m in measurands:
        if not m.include_in_payload or not m.key:
            continue
        enc = _payload_encoding(m)
        size = ENC_SIZE[enc]
        if offset + size > budget:
            dropped.append(m.key)
            continue
        # payload scale = engineering→wire. Derive it from the measurand scale so the wire value keeps
        # full precision (wire = eng / measurand_scale = raw): a 0.1-A measurand → payload scale 10
        # (5.2 A → 52), a 0.01 scale → 100. Integer measurands stay scale 1. Never 0.
        pscale = max(1, round(1.0 / m.scale)) if 0 < m.scale <= 1 else 1
        fields.append({"key": m.key, "offset": offset, "encoding": enc, "scale": pscale})
        offset += size
    total_len = HEADER_LEN + offset
    return fields, total_len, dropped


def assemble_profile(
    candidate: CandidateProfile, op: OperatorInput, type_byte: int, profile_id: Optional[str] = None
) -> dict:
    """Build a schema-conformant profile dict. Raises EmitError if the operator named nothing."""
    apply_edits(candidate, op)
    named = [m for m in candidate.measurands if m.key]
    if not named:
        raise EmitError("no measurands named — the operator must label at least one before packaging")

    category = op.category if op.category in _CATEGORIES else "other"
    pid = profile_id or _slug(f"{op.manufacturer}-{op.model}") or f"scanned-dev-{type_byte}"

    measurands = []
    for m in named:
        entry = {
            "key": m.key,
            "name": m.name or m.key,
            "unit": m.unit,
            "function_code": m.fc,
            "register": m.addr,
            "type": m.dtype.value,
            "scale": m.scale if m.scale else 1.0,
        }
        if m.dtype in (DType.U32, DType.I32, DType.F32):
            entry["word_order"] = m.word_order.value
        if m.offset:
            entry["offset"] = m.offset
        if m.accumulating:
            entry["accumulating"] = True
        measurands.append(entry)

    fields, total_len, dropped = pack_payload(named)
    if not fields:
        raise EmitError("no payload fields — mark at least one named measurand include_in_payload")

    return {
        "profile_id": pid,
        "schema_version": 1,
        "device": {
            "manufacturer": op.manufacturer or "unknown",
            "model": op.model or "unknown",
            "category": category,
            "type_byte": type_byte,
        },
        "bus": {
            "baud": candidate.params.baud,
            "parity": candidate.params.parity,
            "data_bits": 8,
            "stop_bits": candidate.params.stop or 1,
        },
        "defaults": {"function_code": candidate.default_fc, "word_order": candidate.default_word.value},
        "scan": {
            "function_code": candidate.scan_fc,
            "register": candidate.scan_register,
            "quantity": 1,
            "match": "any-reply",
            "note": "scanner-discovered linkcheck register.",
        },
        "measurands": measurands,
        "payload": {"device_byte": type_byte, "total_len": total_len, "fields": fields},
        "provisioning": {
            "slave_id_source": "scan",
            "note": "Discovered by the node bus-scan at commissioning.",
        },
        "source": {
            "repo": "pi-scanner",
            "commit": "scanner-generated",
            "datasheet": op.label or "(operator label)",
        },
        "conflicts": [f"dropped from payload (budget): {', '.join(dropped)}"] if dropped else [],
    }


# --- shell-outs to the committed generators (never reimplement their logic) ---------------------


def _run(argv: list[str], repo_root: str) -> subprocess.CompletedProcess:
    return subprocess.run(argv, cwd=repo_root, capture_output=True, text=True)


def run_blob(profile_path: str, repo_root: str) -> str:
    """Serialize a profile JSON to its NVS blob hex via device-profiles/profile_to_blob.py."""
    script = os.path.join(repo_root, "device-profiles", "profile_to_blob.py")
    cp = _run([sys.executable, script, profile_path], repo_root)
    if cp.returncode != 0:
        raise EmitError(f"profile_to_blob failed: {cp.stderr or cp.stdout}")
    return cp.stdout.strip().split()[-1]  # last token is the hex blob


def write_and_generate(profile: dict, repo_root: str, run_catalog: bool = True) -> ArtifactSet:
    """Write profiles/<id>.json then regenerate blob + decoder + catalog via the committed tools.

    This is the production onboarding action — it mutates device-profiles/. Runs validate first and
    raises if it fails, so a bad profile never reaches the pipeline.
    """
    dp = os.path.join(repo_root, "device-profiles")
    pid = profile["profile_id"]
    profile_path = os.path.join(dp, "profiles", f"{pid}.json")
    with open(profile_path, "w") as fh:
        json.dump(profile, fh, indent=2)
        fh.write("\n")

    v = _run([sys.executable, os.path.join(dp, "validate_profiles.py")], repo_root)
    if v.returncode != 0:
        raise EmitError(f"validate_profiles failed for {pid}: {v.stdout}\n{v.stderr}")

    blob = run_blob(profile_path, repo_root)

    decoder_path = os.path.join(dp, "chirpstack_fleet_decoder.js")
    d = _run([sys.executable, os.path.join(dp, "profile_to_decoder.py"), "-o", decoder_path], repo_root)
    if d.returncode != 0:
        raise EmitError(f"profile_to_decoder failed: {d.stderr or d.stdout}")

    if run_catalog:
        c = _run([sys.executable, os.path.join(dp, "profiles_to_catalog.py")], repo_root)
        if c.returncode != 0:
            raise EmitError(f"profiles_to_catalog failed: {c.stderr or c.stdout}")

    return ArtifactSet(profile_id=pid, profile_path=profile_path, blob_hex=blob, decoder_path=decoder_path)
