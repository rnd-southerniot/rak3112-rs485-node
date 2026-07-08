"""MockNode — a NodeBackend that emulates a SELEC MFM384 on the wire.

Lets the sweep/inference/emitter engines run in CI with no hardware. The device answers only at
9600 8N1 unit 1; FC04 exposes float32 measurands at even registers 0..58 in **CDAB** word order (the
proven MFM384 order), so a whole-register present set of {0..59}; FC03 answers only at reg 6 (the
linkcheck holding register), excepting elsewhere. Block reads that straddle the present/absent
boundary return ILLEGAL_DATA_ADDRESS (0x02), exactly like the firmware, so the engine's narrowing is
exercised.
"""

from __future__ import annotations

import struct

from app.models import IdResult, ProbeResult, ReadClass, ReadResult, SweepChunk

_ILLEGAL_DATA_ADDRESS = 0x02
_ILLEGAL_FUNCTION = 0x01

# Named MFM384 measurands (addr → engineering value); other even addrs in 0..58 get a filler value so
# the whole 0..59 register span is present, as on the real meter.
_MFM_VALUES = {
    0: 230.1, 2: 231.2, 4: 229.8, 6: 230.4, 8: 399.0, 10: 400.1, 12: 398.5, 14: 399.2,
    16: 5.2, 18: 5.0, 20: 4.8, 22: 5.0, 24: 1.2, 26: 1.1, 28: 1.0, 30: 3.3, 32: 3.3,
    34: 3.5, 36: 0.5, 38: 0.5, 40: 0.5, 42: 3.3, 44: 3.5, 46: 0.5, 48: 0.95, 50: 0.96,
    52: 0.94, 54: 0.95, 56: 50.0, 58: 1000.25,
}


def _f32_cdab(v: float) -> tuple[int, int]:
    """(reg@even, reg@even+1) for value v in CDAB order: even = low word, odd = high word."""
    b = struct.pack(">f", v)  # ABCD bytes: b[0:2]=high word, b[2:4]=low word
    high = struct.unpack(">H", b[0:2])[0]
    low = struct.unpack(">H", b[2:4])[0]
    return low, high


def _build_fc04() -> dict[int, int]:
    words: dict[int, int] = {}
    for even in range(0, 60, 2):
        lo, hi = _f32_cdab(_MFM_VALUES.get(even, 0.0))
        words[even] = lo
        words[even + 1] = hi
    return words


class MockNode:
    def __init__(self, unit: int = 1, baud: int = 9600, parity: str = "N"):
        self._unit, self._baud, self._parity = unit, baud, parity
        self._cfg: tuple[int, str] | None = None
        self._fc04 = _build_fc04()  # addr -> u16, present set {0..59}
        self._fc03 = {6: 0x0001}  # linkcheck holding register only

    # --- NodeBackend ---------------------------------------------------------------------------

    def scan_cfg(self, baud: int, parity: str, stop: int = 1) -> None:
        self._cfg = (baud, parity)

    def _live(self, unit: int) -> bool:
        return self._cfg == (self._baud, self._parity) and unit == self._unit

    def _regs(self, fc: int) -> dict[int, int]:
        return self._fc04 if fc == 4 else self._fc03

    def _read_block(self, unit: int, fc: int, addr: int, qty: int):
        """Return (ok, regs, exc, cls) mirroring the firmware/Modbus semantics."""
        if not self._live(unit):
            return False, [], 0, ReadClass.ABS  # wrong baud/parity/unit → timeout
        if fc not in (3, 4):
            return False, [], _ILLEGAL_FUNCTION, ReadClass.EXC
        regs = self._regs(fc)
        block = list(range(addr, addr + qty))
        if all(a in regs for a in block):
            return True, [regs[a] for a in block], 0, ReadClass.DATA
        return False, [], _ILLEGAL_DATA_ADDRESS, ReadClass.EXC  # any illegal reg → block excepts

    def scan_probe(self, unit: int, fc: int, addr: int) -> ProbeResult:
        ok, regs, exc, cls = self._read_block(unit, fc, addr, 1)
        return ProbeResult(unit=unit, fc=fc, addr=addr, cls=cls,
                           reg0=regs[0] if ok else 0, exc=exc)

    def scan_ids(self, lo: int, hi: int, addr: int = 0) -> list[IdResult]:
        out: list[IdResult] = []
        for uid in range(lo, hi + 1):
            ok, regs, exc, cls = self._read_block(uid, 3, addr, 1)  # firmware probe is FC03
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
