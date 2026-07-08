"""inference.py — propose a *candidate* profile from a swept register map. Structure only.

What data can reveal: which registers exist, the likely numeric **type** (float32 vs u16/u32) and the
**word order** (ABCD/CDAB/BADC/DCBA) via float plausibility, and whether a value looks like an
accumulator. What it CANNOT reveal: what a register *means* (is reg 0 "L-N voltage"?). So every
candidate's ``key``/``name``/``unit`` is left blank for the operator to fill; the emitter refuses to
auto-name. This mirrors the firmware's own byte math (``dp_decode`` / ``modbus_regs_to_f32``).
"""

from __future__ import annotations

import math
import struct

from .models import (
    BusParams,
    CandidateMeasurand,
    CandidateProfile,
    DType,
    OperatorInput,
    RegisterMap,
    RegisterObservation,
    WordOrder,
)

_ORDERS = (WordOrder.ABCD, WordOrder.CDAB, WordOrder.BADC, WordOrder.DCBA)


def _bswap16(w: int) -> int:
    return ((w & 0xFF) << 8) | ((w >> 8) & 0xFF)


def decode_f32(reg0: int, reg1: int, order: WordOrder) -> float:
    """Reconstruct a float32 from two big-endian registers under a word order (reg0 = lower addr)."""
    if order == WordOrder.ABCD:
        u32 = (reg0 << 16) | reg1
    elif order == WordOrder.CDAB:
        u32 = (reg1 << 16) | reg0
    elif order == WordOrder.BADC:
        u32 = (_bswap16(reg0) << 16) | _bswap16(reg1)
    else:  # DCBA
        u32 = (_bswap16(reg1) << 16) | _bswap16(reg0)
    return struct.unpack(">f", u32.to_bytes(4, "big"))[0]


def _plausible_f32(v: float) -> bool:
    """A value that could be a real engineering reading (not a wrong-word-order artefact)."""
    if not math.isfinite(v):
        return False
    a = abs(v)
    return a == 0.0 or (1e-3 <= a <= 1e6)


def _pairs(present_addrs: list[int]) -> list[tuple[int, int]]:
    """Even-aligned adjacent present pairs (a, a+1), the float32 candidates."""
    s = set(present_addrs)
    return [(a, a + 1) for a in present_addrs if a % 2 == 0 and (a + 1) in s]


def _best_word_order(rmap: RegisterMap, fc: int) -> tuple[WordOrder, float]:
    """Pick the word order that makes the most pairs plausible + non-zero. Returns (order, share)."""
    by_addr = rmap.by_addr(fc)
    pairs = _pairs(sorted(a for a, o in by_addr.items() if o.present))
    if not pairs:
        return WordOrder.CDAB, 0.0
    best, best_score = WordOrder.CDAB, -1
    for order in _ORDERS:
        score = 0
        for a, b in pairs:
            v = decode_f32(by_addr[a].raw or 0, by_addr[b].raw or 0, order)
            if _plausible_f32(v) and v != 0.0:
                score += 1
        if score > best_score:
            best, best_score = order, score
    return best, (best_score / len(pairs) if pairs else 0.0)


def _dominant_fc(rmap: RegisterMap) -> int:
    counts = {fc: len(rmap.present(fc)) for fc in rmap.present_fcs()}
    if not counts:
        return 4
    # prefer the fc with the most present registers; tie → FC04 (input regs, the common measurand fc)
    return max(counts, key=lambda fc: (counts[fc], fc == 4))


def _scan_target(rmap: RegisterMap, default_fc: int) -> tuple[int, int]:
    """Pick a single register the node can use to discover the slave ID (a linkcheck)."""
    # Prefer a present FC03 holding register (cheap, universally supported); else the default fc's first.
    fc03 = sorted(o.addr for o in rmap.present(3))
    if fc03:
        return 3, fc03[0]
    first = sorted(o.addr for o in rmap.present(default_fc))
    return default_fc, (first[0] if first else 0)


def infer_profile(rmap: RegisterMap, hints: OperatorInput | None = None) -> CandidateProfile:
    default_fc = _dominant_fc(rmap)
    word, _share = _best_word_order(rmap, default_fc)
    by_addr = rmap.by_addr(default_fc)
    present = sorted(a for a, o in by_addr.items() if o.present)
    present_set = set(present)

    measurands: list[CandidateMeasurand] = []
    consumed: set[int] = set()
    for a in present:
        if a in consumed:
            continue
        pair_ok = (a % 2 == 0) and (a + 1 in present_set) and (a + 1 not in consumed)
        if pair_ok:
            v = decode_f32(by_addr[a].raw or 0, by_addr[a + 1].raw or 0, word)
            if _plausible_f32(v):
                # float32 measurand; a value with |v|<1 and integer-ish low word can also be u32 —
                # flagged low-confidence for operator review, but float32 is the common meter case.
                high_zero = (by_addr[a].raw == 0 if word in (WordOrder.CDAB, WordOrder.DCBA)
                             else by_addr[a + 1].raw == 0)
                measurands.append(
                    CandidateMeasurand(
                        addr=a, fc=default_fc, dtype=DType.F32, word_order=word,
                        sample_value=round(v, 4), confidence=0.9,
                        accumulating=abs(v) > 1e4,  # large magnitude → likely an energy accumulator
                        notes="u32 alternative — verify" if high_zero and abs(v) < 1.0 else "",
                    )
                )
                consumed.update({a, a + 1})
                continue
        # single u16 (odd leftover, unpaired, or implausible-as-float)
        obs: RegisterObservation = by_addr[a]
        measurands.append(
            CandidateMeasurand(
                addr=a, fc=default_fc, dtype=DType.U16, word_order=word,
                sample_value=float(obs.raw or 0), confidence=0.4,
            )
        )
        consumed.add(a)

    scan_fc, scan_reg = _scan_target(rmap, default_fc)
    params: BusParams = rmap.params
    return CandidateProfile(
        params=params,
        default_fc=default_fc,
        default_word=word,
        scan_fc=scan_fc,
        scan_register=scan_reg,
        measurands=measurands,
    )
