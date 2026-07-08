"""Sweep engine — discover the bus params + unit ID, then map which registers actually exist.

Two steps, both driven purely through the ``NodeBackend`` (real node or MockNode):

* ``discover_units`` — sweep baud × parity, probe a unit-ID range (FC03; a Modbus *exception* still
  proves presence, so it finds FC04-only meters too). Operator hints (known baud/parity) are tried
  first. Returns the first ``BusParams`` that answers.
* ``sweep_registers`` — block-read FC03 and FC04 across an address range; a block that returns data
  marks each register present with its raw word; a block that excepts (one illegal address inside it)
  is narrowed by per-register reads so the present/absent boundary is exact.
"""

from __future__ import annotations

from typing import Callable, Optional

from .models import (
    BusParams,
    ReadClass,
    RegisterMap,
    RegisterObservation,
)
from .node_console import NodeBackend

Log = Callable[[str], None]

DEFAULT_BAUDS = (9600, 4800, 19200, 38400, 57600, 115200, 1200, 2400)
DEFAULT_PARITIES = ("N", "E", "O")


def _noop(_: str) -> None:
    pass


def discover_units(
    node: NodeBackend,
    id_lo: int = 1,
    id_hi: int = 32,
    bauds: tuple[int, ...] = DEFAULT_BAUDS,
    parities: tuple[str, ...] = DEFAULT_PARITIES,
    probe_addr: int = 0,
    known_baud: Optional[int] = None,
    known_parity: Optional[str] = None,
    log: Log = _noop,
) -> Optional[BusParams]:
    """Return the first (baud, parity, unit) that answers, or None. Hints are tried first."""
    baud_order = ([known_baud] if known_baud else []) + [b for b in bauds if b != known_baud]
    par_order = ([known_parity] if known_parity else []) + [p for p in parities if p != known_parity]
    for parity in par_order:
        for baud in baud_order:
            log(f"discover: {baud} {parity} …")
            node.scan_cfg(baud, parity)
            ids = node.scan_ids(id_lo, id_hi, probe_addr)
            present = [r for r in ids if r.present]
            if present:
                unit = min(r.unit for r in present)
                log(f"discover: device at {baud} {parity} unit {unit} "
                    f"({present[0].cls.lower()})")
                return BusParams(baud=baud, parity=parity, unit=unit)
    log("discover: no device on any baud × parity")
    return None


def _classify_register(node: NodeBackend, unit: int, fc: int, addr: int) -> RegisterObservation:
    """One-register read to resolve presence exactly (used to narrow an excepting block)."""
    r = node.scan_read(unit, fc, addr, 1)
    if r.ok and r.regs:
        return RegisterObservation(fc=fc, addr=addr, present=True, raw=r.regs[0])
    if r.cls == ReadClass.EXC:
        return RegisterObservation(fc=fc, addr=addr, present=False, exception=r.exc)
    return RegisterObservation(fc=fc, addr=addr, present=False)


def sweep_registers(
    node: NodeBackend,
    params: BusParams,
    ranges: tuple[tuple[int, int, int], ...] = ((4, 0, 127), (3, 0, 127)),
    stride: int = 8,
    log: Log = _noop,
) -> RegisterMap:
    """Map present/absent (+ raw) for every register in ``ranges`` = ((fc, start, end), …)."""
    node.scan_cfg(params.baud, params.parity)
    rmap = RegisterMap(unit=params.unit, params=params)
    for fc, start, end in ranges:
        log(f"sweep: FC{fc:02d} regs {start}..{end} (stride {stride})")
        for chunk in node.scan_sweep(params.unit, fc, start, end, stride):
            if chunk.cls == ReadClass.DATA:
                for i, word in enumerate(chunk.regs):
                    rmap.observations.append(
                        RegisterObservation(fc=fc, addr=chunk.addr + i, present=True, raw=word)
                    )
            elif chunk.cls == ReadClass.EXC:
                # one illegal address somewhere in the block → narrow to find the boundary exactly.
                for addr in range(chunk.addr, chunk.addr + chunk.qty):
                    rmap.observations.append(_classify_register(node, params.unit, fc, addr))
            else:  # ABS (timeout) / BAD — treat the whole block as absent.
                for addr in range(chunk.addr, chunk.addr + chunk.qty):
                    rmap.observations.append(
                        RegisterObservation(fc=fc, addr=addr, present=False)
                    )
        n = len(rmap.present(fc))
        log(f"sweep: FC{fc:02d} → {n} present register(s)")
    return rmap
