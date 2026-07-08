"""NodeConsole — drive the Careflow node's ``scan-*`` console over USB-Serial-JTAG.

Reuses the ``tools/provision_nvs.py`` serial recipe (reset input, write line, read until the
terminating ``OK``/``ERR``). USB-Serial-JTAG gotcha (this project's hard-won lesson): the S3 only
flushes console TX when the host asserts **DTR**, so we keep pyserial's default ``dtr=True``; the
linenoise REPL eats the first scripted line, so ``open()`` primes it with a throwaway newline.

Parsing lives in pure module functions (``parse_*``) so it is unit-testable against captured firmware
lines without a serial port. ``NodeBackend`` is the interface the sweep engine depends on — satisfied
by ``NodeConsole`` (real) and by the test ``MockNode``.
"""

from __future__ import annotations

import time
from typing import Callable, Optional, Protocol

from .models import (
    BusParams,
    IdResult,
    ProbeResult,
    ReadClass,
    ReadResult,
    SweepChunk,
)


class NodeError(RuntimeError):
    """A scan-* command returned ERR, or the console produced no terminator line in time."""


# --- pure line parsers -------------------------------------------------------------------------


def _kv(tokens: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for t in tokens:
        if "=" in t:
            k, v = t.split("=", 1)
            out[k] = v
    return out


def _int(s: str) -> int:
    return int(s, 16) if s.lower().startswith("0x") else int(s)


def _hex_words(s: str) -> list[int]:
    return [int(w, 16) for w in s.split() if w]


def parse_scan_read(lines: list[str]) -> ReadResult:
    """Parse a scan-read reply. Raises NodeError only on a malformed/absent terminator."""
    for ln in lines:
        if ln.startswith("OK scan-read"):
            # OK scan-read unit=U fc=F addr=A qty=Q regs=AAAA BBBB ...
            head, _, regs_s = ln.partition("regs=")
            kv = _kv(head.split())
            return ReadResult(
                ok=True,
                unit=int(kv["unit"]),
                fc=int(kv["fc"]),
                addr=int(kv["addr"]),
                qty=int(kv["qty"]),
                regs=_hex_words(regs_s),
                cls=ReadClass.DATA,
            )
        if ln.startswith("ERR scan-read"):
            kv = _kv(ln.split())
            if "exc" in kv:
                return ReadResult(ok=False, unit=0, fc=0, addr=0, qty=0, exc=_int(kv["exc"]),
                                  cls=ReadClass.EXC)
            cls = ReadClass(kv["class"]) if "class" in kv else ReadClass.BAD
            return ReadResult(ok=False, unit=0, fc=0, addr=0, qty=0, cls=cls)
    raise NodeError(f"no scan-read terminator in: {lines!r}")


def parse_scan_ids(lines: list[str]) -> list[IdResult]:
    out: list[IdResult] = []
    for ln in lines:
        if ln.startswith("ID "):
            # ID <id> <CLASS> reg0=0x.. exc=0x.. lat=..ms
            parts = ln.split()
            cls = parts[2]
            kv = _kv(parts[3:])
            out.append(
                IdResult(
                    unit=int(parts[1]),
                    present=cls in ("DATA", "EXCEPTION"),
                    cls=cls,
                    reg0=_int(kv.get("reg0", "0")),
                    exc=_int(kv.get("exc", "0")),
                    latency_ms=int(kv.get("lat", "0ms").rstrip("ms") or "0"),
                )
            )
    return out


def parse_scan_sweep(lines: list[str]) -> list[SweepChunk]:
    out: list[SweepChunk] = []
    for ln in lines:
        if not ln.startswith("SWEEP "):
            continue
        # SWEEP addr=A qty=Q <DATA regs=..|ABS|EXC exc=0x..|BAD st=..>
        head, _, rest = ln.partition(" regs=")
        toks = head.split()
        kv = _kv(toks)
        cls_tok = toks[-1]  # last token before regs= is the class word
        # class word sits after the qty=.. token
        cls_word = None
        for t in toks:
            if t in ("DATA", "ABS", "EXC", "BAD"):
                cls_word = t
        cls = ReadClass(cls_word) if cls_word else ReadClass.BAD
        regs = _hex_words(rest) if cls == ReadClass.DATA else []
        exc = _int(kv["exc"]) if "exc" in kv else 0
        out.append(SweepChunk(addr=int(kv["addr"]), qty=int(kv["qty"]), cls=cls, regs=regs, exc=exc))
    return out


def parse_scan_probe(lines: list[str]) -> ProbeResult:
    for ln in lines:
        if ln.startswith("OK scan-probe"):
            kv = _kv(ln.split())
            return ProbeResult(
                unit=int(kv["unit"]),
                fc=int(kv["fc"]),
                addr=int(kv["addr"]),
                cls=ReadClass(kv["class"]),
                reg0=_int(kv.get("reg0", "0")),
                exc=_int(kv.get("exc", "0")),
            )
    raise NodeError(f"no scan-probe terminator in: {lines!r}")


# --- backend interface -------------------------------------------------------------------------


class NodeBackend(Protocol):
    def scan_cfg(self, baud: int, parity: str, stop: int = 1) -> None: ...
    def scan_probe(self, unit: int, fc: int, addr: int) -> ProbeResult: ...
    def scan_ids(self, lo: int, hi: int, addr: int = 0) -> list[IdResult]: ...
    def scan_read(self, unit: int, fc: int, addr: int, qty: int) -> ReadResult: ...
    def scan_sweep(self, unit: int, fc: int, start: int, end: int, stride: int) -> list[SweepChunk]: ...


# --- real serial backend -----------------------------------------------------------------------


class NodeConsole:
    """pyserial-backed NodeBackend. ``on_line`` (if set) receives every console line for the log UI."""

    def __init__(self, port: str, baud: int = 115200, timeout: float = 0.4,
                 on_line: Optional[Callable[[str], None]] = None):
        import serial  # local import so the package imports without pyserial (tests use MockNode)

        self._ser = serial.Serial(port, baud, timeout=timeout)  # dtr defaults True → USJ TX flushes
        self._on_line = on_line
        self.params: Optional[BusParams] = None
        time.sleep(0.3)
        self._ser.reset_input_buffer()
        self._raw(b"\r\n")  # prime the linenoise REPL (it eats the first scripted line)
        time.sleep(0.2)
        self._ser.reset_input_buffer()

    def close(self) -> None:
        try:
            self._ser.close()
        except Exception:
            pass

    def _raw(self, b: bytes) -> None:
        self._ser.write(b)
        self._ser.flush()

    def _cmd(self, line: str, deadline_s: float = 8.0) -> list[str]:
        """Send a command, collect lines until an ``OK``/``ERR`` terminator or the deadline."""
        self._ser.reset_input_buffer()
        self._raw((line + "\r\n").encode())
        lines: list[str] = []
        end = time.monotonic() + deadline_s
        while time.monotonic() < end:
            raw = self._ser.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", "replace").strip()
            if not text or text in ("esp>", "esp> ") or text == line:
                continue
            if self._on_line:
                self._on_line(text)
            lines.append(text)
            if text.startswith("OK ") or text.startswith("ERR "):
                return lines
        raise NodeError(f"timeout waiting for terminator after {line!r}: {lines!r}")

    def scan_cfg(self, baud: int, parity: str, stop: int = 1) -> None:
        lines = self._cmd(f"scan-cfg {baud} {parity} {stop}")
        if not any(ln.startswith("OK scan-cfg") for ln in lines):
            raise NodeError(f"scan-cfg failed: {lines!r}")
        self.params = BusParams(baud=baud, parity=parity, unit=0, stop=stop)

    def scan_probe(self, unit: int, fc: int, addr: int) -> ProbeResult:
        return parse_scan_probe(self._cmd(f"scan-probe {unit} {fc} {addr}"))

    def scan_ids(self, lo: int, hi: int, addr: int = 0) -> list[IdResult]:
        # id sweep can be long (247 * timeout); scale the deadline with the range.
        return parse_scan_ids(self._cmd(f"scan-ids {lo} {hi} {addr}", deadline_s=2.0 + 0.8 * (hi - lo + 1)))

    def scan_read(self, unit: int, fc: int, addr: int, qty: int) -> ReadResult:
        return parse_scan_read(self._cmd(f"scan-read {unit} {fc} {addr} {qty}"))

    def scan_sweep(self, unit: int, fc: int, start: int, end: int, stride: int) -> list[SweepChunk]:
        chunks = (end - start) // max(stride, 1) + 2
        return parse_scan_sweep(
            self._cmd(f"scan-sweep {unit} {fc} {start} {end} {stride}", deadline_s=3.0 + 0.6 * chunks)
        )
