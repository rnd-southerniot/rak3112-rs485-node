"""P3 gate: the emitter allocates type_byte=5, packs <=53B, and produces a profile that

validate_profiles.py accepts and profile_to_blob.py serializes (the firmware-side "prov-profile OK"
proxy). Also checks the refuse-to-auto-name guard and the payload budget.

The one filesystem-touching test writes a throwaway profile into the real profiles dir, validates,
then removes it in a finally — the repo is left clean (no catalog/decoder regeneration).
"""

from __future__ import annotations

import os
import subprocess
import sys

import pytest

from app.inference import infer_profile
from app.models import DType, MeasurandEdit, OperatorInput
from app.profile_emitter import (
    EmitError,
    assemble_profile,
    next_free_type_byte,
    pack_payload,
    run_blob,
)
from app.sweep_engine import discover_units, sweep_registers
from tests.mock_node import MockNode

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PROFILES_DIR = os.path.join(REPO, "device-profiles", "profiles")


def _candidate():
    node = MockNode()
    params = discover_units(node, id_lo=1, id_hi=4)
    rmap = sweep_registers(node, params, ranges=((4, 0, 71), (3, 0, 15)), stride=8)
    return infer_profile(rmap)


def _operator():
    return OperatorInput(
        manufacturer="ACME",
        model="TestMeter-9000",
        category="energy-meter",
        label="ACME TM9000",
        measurand_edits=[
            MeasurandEdit(fc=4, addr=0, key="v1n", name="Phase-1 L-N voltage", unit="V"),
            MeasurandEdit(fc=4, addr=56, key="freq_hz", name="Frequency", unit="Hz"),
            MeasurandEdit(fc=4, addr=58, key="total_kwh", name="Total energy", unit="kWh",
                          accumulating=True),
        ],
    )


def test_next_free_type_byte():
    import json

    used = {
        json.load(open(os.path.join(PROFILES_DIR, f)))["device"]["type_byte"]
        for f in os.listdir(PROFILES_DIR)
        if f.endswith(".json")
    }
    tb = next_free_type_byte(PROFILES_DIR)
    assert tb not in used and tb == max(used) + 1  # allocates the next byte above the highest


def test_refuses_to_emit_without_names():
    cand = _candidate()
    with pytest.raises(EmitError):
        assemble_profile(cand, OperatorInput(manufacturer="ACME", model="x"), type_byte=5)


def test_payload_budget_enforced():
    cand = _candidate()
    # name ALL candidates so the packer must enforce the 53-byte cap (drops the overflow)
    op = OperatorInput(
        measurand_edits=[MeasurandEdit(fc=m.fc, addr=m.addr, key=f"m{m.addr}") for m in cand.measurands]
    )
    from app.profile_emitter import apply_edits

    apply_edits(cand, op)
    fields, total_len, dropped = pack_payload([m for m in cand.measurands if m.key])
    assert total_len <= 53
    assert len(dropped) > 0  # 30 u16 measurands cannot all fit in 50 body bytes


def test_emitted_profile_validates_and_serializes():
    cand = _candidate()
    tb = next_free_type_byte(PROFILES_DIR)  # next free (adapts as profiles are added to the registry)
    profile = assemble_profile(cand, _operator(), type_byte=tb, profile_id="acme-testmeter9000")
    assert profile["device"]["type_byte"] == tb
    assert profile["payload"]["device_byte"] == tb
    assert profile["payload"]["total_len"] <= 53
    assert profile["defaults"]["word_order"] == "CDAB"
    assert all(m["key"] for m in profile["measurands"])  # every measurand named

    path = os.path.join(PROFILES_DIR, "acme-testmeter9000.json")
    import json

    try:
        with open(path, "w") as fh:
            json.dump(profile, fh, indent=2)
            fh.write("\n")
        v = subprocess.run(
            [sys.executable, os.path.join(REPO, "device-profiles", "validate_profiles.py")],
            cwd=REPO, capture_output=True, text=True,
        )
        assert v.returncode == 0, v.stdout + v.stderr
        blob = run_blob(path, REPO)
        assert blob and len(blob) % 2 == 0 and int(blob, 16) >= 0  # valid hex the node would accept
    finally:
        if os.path.exists(path):
            os.remove(path)
