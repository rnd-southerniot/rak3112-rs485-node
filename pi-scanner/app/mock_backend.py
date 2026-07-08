"""In-app mocks for the hardware-free "mock" port: a MockNode (emulates an MFM384 on the wire) and a
MockOnboarding (logs + advances through the flash/provision/decoder/verify steps without hardware).

The MockNode lets the whole UI/orchestrator walk every state in CI and on a bench with no device; the
test suite re-exports it from tests/mock_node.py.
"""

from __future__ import annotations

import struct
import time
from typing import Callable

from .models import IdResult, ProbeResult, ReadClass, ReadResult, SweepChunk

_ILLEGAL_DATA_ADDRESS = 0x02
_ILLEGAL_FUNCTION = 0x01

_MFM_VALUES = {
    0: 230.1, 2: 231.2, 4: 229.8, 6: 230.4, 8: 399.0, 10: 400.1, 12: 398.5, 14: 399.2,
    16: 5.2, 18: 5.0, 20: 4.8, 22: 5.0, 24: 1.2, 26: 1.1, 28: 1.0, 30: 3.3, 32: 3.3,
    34: 3.5, 36: 0.5, 38: 0.5, 40: 0.5, 42: 3.3, 44: 3.5, 46: 0.5, 48: 0.95, 50: 0.96,
    52: 0.94, 54: 0.95, 56: 50.0, 58: 1000.25,
}


def _f32_cdab(v: float) -> tuple[int, int]:
    b = struct.pack(">f", v)
    return struct.unpack(">H", b[2:4])[0], struct.unpack(">H", b[0:2])[0]  # (low, high)


def _build_fc04() -> dict[int, int]:
    words: dict[int, int] = {}
    for even in range(0, 60, 2):
        lo, hi = _f32_cdab(_MFM_VALUES.get(even, 0.0))
        words[even], words[even + 1] = lo, hi
    return words


class MockNode:
    """A NodeBackend emulating a SELEC MFM384: 9600 8N1 unit 1, FC04 float32 CDAB regs 0..59,
    FC03 reg 6 linkcheck; block reads that straddle the boundary except (ILLEGAL_DATA_ADDRESS)."""

    def __init__(self, unit: int = 1, baud: int = 9600, parity: str = "N"):
        self._unit, self._baud, self._parity = unit, baud, parity
        self._cfg: tuple[int, str] | None = None
        self._fc04 = _build_fc04()
        self._fc03 = {6: 0x0001}

    def scan_cfg(self, baud: int, parity: str, stop: int = 1) -> None:
        self._cfg = (baud, parity)

    def _live(self, unit: int) -> bool:
        return self._cfg == (self._baud, self._parity) and unit == self._unit

    def _regs(self, fc: int) -> dict[int, int]:
        return self._fc04 if fc == 4 else self._fc03

    def _read_block(self, unit: int, fc: int, addr: int, qty: int):
        if not self._live(unit):
            return False, [], 0, ReadClass.ABS
        if fc not in (3, 4):
            return False, [], _ILLEGAL_FUNCTION, ReadClass.EXC
        regs = self._regs(fc)
        block = list(range(addr, addr + qty))
        if all(a in regs for a in block):
            return True, [regs[a] for a in block], 0, ReadClass.DATA
        return False, [], _ILLEGAL_DATA_ADDRESS, ReadClass.EXC

    def scan_probe(self, unit: int, fc: int, addr: int) -> ProbeResult:
        ok, regs, exc, cls = self._read_block(unit, fc, addr, 1)
        return ProbeResult(unit=unit, fc=fc, addr=addr, cls=cls, reg0=regs[0] if ok else 0, exc=exc)

    def scan_ids(self, lo: int, hi: int, addr: int = 0) -> list[IdResult]:
        out: list[IdResult] = []
        for uid in range(lo, hi + 1):
            ok, regs, exc, cls = self._read_block(uid, 3, addr, 1)
            if cls == ReadClass.ABS:
                continue
            probe_cls = "DATA" if cls == ReadClass.DATA else ("EXCEPTION" if cls == ReadClass.EXC else "BADFRAME")
            out.append(IdResult(unit=uid, present=cls in (ReadClass.DATA, ReadClass.EXC),
                                cls=probe_cls, reg0=regs[0] if ok else 0, exc=exc, latency_ms=3))
        return out

    def scan_read(self, unit: int, fc: int, addr: int, qty: int) -> ReadResult:
        ok, regs, exc, cls = self._read_block(unit, fc, addr, qty)
        return ReadResult(ok=ok, unit=unit, fc=fc, addr=addr, qty=qty, regs=regs, exc=exc, cls=cls)

    def scan_sweep(self, unit: int, fc: int, start: int, end: int, stride: int) -> list[SweepChunk]:
        chunks: list[SweepChunk] = []
        addr = start
        while addr <= end:
            qty = min(stride, end - addr + 1)
            ok, regs, exc, cls = self._read_block(unit, fc, addr, qty)
            chunks.append(SweepChunk(addr=addr, qty=qty, cls=cls, regs=regs, exc=exc))
            addr += stride
        return chunks

    def close(self) -> None:
        pass


class MockOnboarding:
    """Stand-in for the real onboarding (P5): logs each step, no hardware/network. Used by the
    'mock' port so the UI can walk FLASHING → PROBING → PROFILING → SUCCESS."""

    def __init__(self, log: Callable[[str], None], delay: float = 0.05):
        self._log = log
        self._delay = delay

    def flash_node(self, artifacts) -> None:
        self._log("mock flash: wrote scanner firmware + app (erase disabled)")
        time.sleep(self._delay)

    def provision(self, artifacts, port=None) -> None:
        self._log(f"mock provision: wrote NVS creds + profile blob ({len(artifacts.blob_hex)//2} B)")
        time.sleep(self._delay)

    def install_decoder(self, artifacts) -> None:
        self._log("mock decoder: installed fleet codec on dev ChirpStack device-profile")
        time.sleep(self._delay)

    def verify_uplink(self, artifacts, deveui: str = "", timeout_s: int = 0) -> dict:
        self._log("mock verify: join + decoded uplink observed (device_byte matches)")
        time.sleep(self._delay)
        return {"joined": True, "decoded": True, "device_byte": None}
