"""P1 gate: discover_units + sweep_registers reproduce the MFM384 ground truth via MockNode.

Also unit-tests the pure console-line parsers against captured firmware output so the real
NodeConsole path is covered without a serial port.
"""

from __future__ import annotations

from app.models import ReadClass
from app.node_console import parse_scan_ids, parse_scan_read, parse_scan_sweep
from app.sweep_engine import discover_units, sweep_registers
from tests.mock_node import MockNode


def test_discover_finds_mfm384():
    node = MockNode()
    params = discover_units(node, id_lo=1, id_hi=8, bauds=(4800, 9600, 19200), parities=("N", "E"))
    assert params is not None
    assert (params.baud, params.parity, params.unit) == (9600, "N", 1)


def test_discover_honours_hint_first():
    node = MockNode()
    params = discover_units(node, known_baud=9600, known_parity="N")
    assert params is not None and params.unit == 1


def test_sweep_matches_mfm384_present_set():
    node = MockNode()
    params = discover_units(node, id_lo=1, id_hi=4)
    rmap = sweep_registers(node, params, ranges=((4, 0, 71), (3, 0, 15)), stride=8)

    fc04_present = sorted(o.addr for o in rmap.present(4))
    assert fc04_present == list(range(0, 60))  # FC04 registers 0..59, exactly

    fc03_present = sorted(o.addr for o in rmap.present(3))
    assert fc03_present == [6]  # only the linkcheck holding register

    # a present FC04 register carries its raw word (CDAB low word of a float32)
    assert rmap.by_addr(4)[0].raw is not None


def test_parse_scan_read_data_and_exc():
    ok = parse_scan_read(["OK scan-read unit=1 fc=4 addr=0 qty=3 regs=0000 4366 199A"])
    assert ok.ok and ok.regs == [0x0000, 0x4366, 0x199A]
    exc = parse_scan_read(["ERR scan-read exc=0x02"])
    assert not exc.ok and exc.exc == 0x02 and exc.cls == ReadClass.EXC


def test_parse_scan_ids_and_sweep():
    ids = parse_scan_ids(["ID 1 EXCEPTION reg0=0x0000 exc=0x02 lat=3ms", "OK scan-ids found=1"])
    assert len(ids) == 1 and ids[0].unit == 1 and ids[0].present

    chunks = parse_scan_sweep(
        [
            "SWEEP addr=0 qty=8 DATA regs=0000 4366 0000 4367 0000 4365 0000 4366",
            "SWEEP addr=56 qty=8 EXC exc=0x02",
            "OK scan-sweep present=1 exc=1",
        ]
    )
    assert chunks[0].cls == ReadClass.DATA and len(chunks[0].regs) == 8
    assert chunks[1].cls == ReadClass.EXC and chunks[1].exc == 0x02
