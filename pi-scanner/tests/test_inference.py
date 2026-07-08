"""P2 gate: inference reproduces the committed selec-mfm384.json structure from a swept map.

Types + word order must match for >=90% of the profile's registers; semantics stay blank.
"""

from __future__ import annotations

import json
import os

from app.inference import decode_f32, infer_profile
from app.models import DType, WordOrder
from app.sweep_engine import discover_units, sweep_registers
from tests.mock_node import MockNode

_PROFILE = os.path.join(
    os.path.dirname(__file__), "..", "..", "device-profiles", "profiles", "selec-mfm384.json"
)


def _candidate():
    node = MockNode()
    params = discover_units(node, id_lo=1, id_hi=4)
    rmap = sweep_registers(node, params, ranges=((4, 0, 71), (3, 0, 15)), stride=8)
    return infer_profile(rmap)


def test_word_order_is_cdab():
    assert _candidate().default_word == WordOrder.CDAB


def test_default_fc_is_input_registers():
    assert _candidate().default_fc == 4


def test_types_match_profile_over_90pct():
    cand = _candidate()
    by_addr = {(m.fc, m.addr): m for m in cand.measurands}
    with open(_PROFILE) as fh:
        profile = json.load(fh)

    total = 0
    matched = 0
    for meas in profile["measurands"]:
        key = (meas["function_code"], meas["register"])
        total += 1
        m = by_addr.get(key)
        if m and m.dtype == DType(meas["type"]) and m.word_order == WordOrder.CDAB:
            matched += 1
    assert total > 0
    assert matched / total >= 0.90, f"only {matched}/{total} registers matched"


def test_semantics_left_blank_for_operator():
    for m in _candidate().measurands:
        assert m.key == "" and m.name == "" and m.unit == ""


def test_scan_target_prefers_fc03_linkcheck():
    cand = _candidate()
    assert cand.scan_fc == 3 and cand.scan_register == 6


def test_decode_f32_word_orders():
    # 1000.0f big-endian = 0x447A0000 → high word 0x447A, low word 0x0000.
    assert abs(decode_f32(0x447A, 0x0000, WordOrder.ABCD) - 1000.0) < 1e-3
    assert abs(decode_f32(0x0000, 0x447A, WordOrder.CDAB) - 1000.0) < 1e-3
