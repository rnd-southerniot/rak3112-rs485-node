"""Data model for the scanner: console results, the swept register map, and the candidate profile.

Plain dataclasses (no pydantic in the engine) so the sweep/inference/emitter logic stays trivially
unit-testable; the FastAPI layer serialises these with ``dataclasses.asdict``. Enum *values* mirror
the on-wire / profile-schema vocabulary exactly (``dp_word_t`` ABCD/CDAB/BADC/DCBA, dtypes
u16/i16/u32/i32/float32) so the emitter can pass them straight into the profile JSON.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Optional

Parity = str  # "N" | "E" | "O"


class DType(str, Enum):
    U16 = "u16"
    I16 = "i16"
    U32 = "u32"
    I32 = "i32"
    F32 = "float32"

    @property
    def regs(self) -> int:
        return 2 if self in (DType.U32, DType.I32, DType.F32) else 1


class WordOrder(str, Enum):
    ABCD = "ABCD"
    CDAB = "CDAB"
    BADC = "BADC"
    DCBA = "DCBA"


# --- console-command results (parsed from scan-* OK/ERR lines) ---------------------------------


class ReadClass(str, Enum):
    DATA = "DATA"  # MODBUS_OK — register data returned
    EXC = "EXC"  # MODBUS_ERR_EXCEPTION — device replied with a Modbus exception (still present)
    ABS = "ABS"  # timeout / short — absent at this baud/parity/unit
    BAD = "BAD"  # bytes seen but CRC/ID/func mismatch — noise / wrong baud


@dataclass
class ProbeResult:
    unit: int
    fc: int
    addr: int
    cls: ReadClass
    reg0: int = 0
    exc: int = 0


@dataclass
class IdResult:
    unit: int
    present: bool
    cls: str  # DATA | EXCEPTION | BADFRAME (firmware probe vocabulary)
    reg0: int = 0
    exc: int = 0
    latency_ms: int = 0


@dataclass
class ReadResult:
    ok: bool
    unit: int
    fc: int
    addr: int
    qty: int
    regs: list[int] = field(default_factory=list)
    exc: int = 0
    cls: ReadClass = ReadClass.DATA


@dataclass
class SweepChunk:
    addr: int
    qty: int
    cls: ReadClass
    regs: list[int] = field(default_factory=list)
    exc: int = 0


# --- swept register map ------------------------------------------------------------------------


@dataclass
class BusParams:
    baud: int
    parity: Parity
    unit: int
    stop: int = 1


@dataclass
class RegisterObservation:
    """One register's presence + last raw value, resolved to per-register granularity."""

    fc: int
    addr: int
    present: bool
    raw: Optional[int] = None  # u16 word when present with data
    exception: Optional[int] = None  # Modbus exception code if the register excepts


@dataclass
class RegisterMap:
    unit: int
    params: BusParams
    observations: list[RegisterObservation] = field(default_factory=list)

    def present(self, fc: Optional[int] = None) -> list[RegisterObservation]:
        return [o for o in self.observations if o.present and (fc is None or o.fc == fc)]

    def by_addr(self, fc: int) -> dict[int, RegisterObservation]:
        return {o.addr: o for o in self.observations if o.fc == fc}

    def present_fcs(self) -> list[int]:
        return sorted({o.fc for o in self.observations if o.present})


# --- candidate profile (populated by inference, edited by operator, packaged by emitter) -------


@dataclass
class CandidateMeasurand:
    addr: int
    fc: int
    dtype: DType
    word_order: WordOrder
    sample_value: Optional[float] = None
    confidence: float = 0.0
    accumulating: bool = False
    # operator-supplied semantics (blank until confirmed):
    key: str = ""
    name: str = ""
    unit: str = ""
    scale: float = 1.0
    offset: float = 0.0
    include_in_payload: bool = True
    notes: str = ""


@dataclass
class MeasurandEdit:
    addr: int
    fc: int
    key: Optional[str] = None
    name: Optional[str] = None
    unit: Optional[str] = None
    dtype: Optional[str] = None
    word_order: Optional[str] = None
    scale: Optional[float] = None
    offset: Optional[float] = None
    accumulating: Optional[bool] = None
    include_in_payload: Optional[bool] = None


@dataclass
class OperatorInput:
    manufacturer: str = ""
    model: str = ""
    category: str = "other"  # profile.schema.json device.category enum
    label: str = ""  # human label printed on the device
    known_baud: Optional[int] = None
    known_parity: Optional[str] = None
    notes: str = ""
    measurand_edits: list[MeasurandEdit] = field(default_factory=list)


@dataclass
class CandidateProfile:
    params: BusParams
    default_fc: int
    default_word: WordOrder
    scan_fc: int
    scan_register: int
    measurands: list[CandidateMeasurand] = field(default_factory=list)


# --- session / UI state ------------------------------------------------------------------------


class SessionState(str, Enum):
    IDLE = "idle"
    SEARCHING = "searching"
    PREPARING_PROFILE = "preparing_profile"
    FLASHING = "flashing"
    PROBING = "probing"
    PROFILING = "profiling"
    SUCCESS = "success"
    FAILED = "failed"
