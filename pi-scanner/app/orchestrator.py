"""orchestrator.py — the session state machine that maps scanner steps to the UI states.

One station, one device at a time. Long steps (search/onboard) run as asyncio tasks; blocking serial /
subprocess work is offloaded with ``asyncio.to_thread``. Every state change + log line is published on
the EventBus for the WebSocket UI, and a JSON snapshot is available via ``snapshot()`` for polling.

port == "mock" wires the in-app MockNode + MockOnboarding so the whole flow walks end-to-end with no
hardware (the P4 dry-run + CI).
"""

from __future__ import annotations

import asyncio
import dataclasses
from collections import deque
from typing import Any, Optional

from .config import Config
from .events import Event, EventBus
from .inference import infer_profile
from .models import (
    CandidateProfile,
    MeasurandEdit,
    OperatorInput,
    SessionState,
)
from .profile_emitter import (
    ENC_SIZE,
    DR3_MAX,
    HEADER_LEN,
    apply_edits,
    assemble_profile,
    next_free_type_byte,
    write_and_generate,
)
from .sweep_engine import discover_units, sweep_registers


def _payload_encoding_size(m) -> int:
    if m.accumulating:
        return ENC_SIZE["u32"]
    if m.sample_value is not None and m.sample_value < 0:
        return ENC_SIZE["i16"]
    return ENC_SIZE["u16"]


class Orchestrator:
    def __init__(self, bus: EventBus, config: Config):
        self.bus = bus
        self.cfg = config
        self.state = SessionState.IDLE
        self.port: Optional[str] = None
        self.logs: deque[str] = deque(maxlen=500)
        self.candidate: Optional[CandidateProfile] = None
        self.operator = OperatorInput()
        self.artifacts: Optional[Any] = None
        self.error: Optional[str] = None
        self._task: Optional[asyncio.Task] = None
        self._node = None
        self._onboarding = None

    # --- event helpers -------------------------------------------------------------------------

    def _log(self, msg: str) -> None:
        self.logs.append(msg)
        self.bus.emit(Event("log", msg))

    def _set_state(self, st: SessionState) -> None:
        self.state = st
        self.bus.emit(Event("state", st.value))

    def _emit_candidate(self) -> None:
        self.bus.emit(Event("candidate", self.candidate_dict()))

    # --- backends ------------------------------------------------------------------------------

    def _make_backends(self, port: str):
        if port == "mock":
            from .mock_backend import MockNode, MockOnboarding

            return MockNode(), MockOnboarding(self._log)
        from .node_console import NodeConsole
        from .onboarding import Onboarding

        node = NodeConsole(port, self.cfg.node_baud, on_line=lambda ln: self._log(ln))
        cfg = dataclasses.replace(self.cfg, node_port=port)  # onboarding provisions on the same port
        return node, Onboarding(cfg, self._log)

    # --- flow: search --------------------------------------------------------------------------

    def start_search(self, port: str) -> None:
        if self._task and not self._task.done():
            raise RuntimeError("a scan is already running")
        self.reset()
        self.port = port
        self._task = asyncio.create_task(self._run_search(port))

    async def _run_search(self, port: str) -> None:
        try:
            self._set_state(SessionState.SEARCHING)
            self._log(f"opening node on {port} …")
            self._node, self._onboarding = await asyncio.to_thread(self._make_backends, port)

            params = await asyncio.to_thread(
                discover_units, self._node,
                known_baud=self.operator.known_baud, known_parity=self.operator.known_parity,
                log=self._log,
            )
            if params is None:
                raise RuntimeError("no Modbus device found on any baud × parity")
            self._log(f"device: {params.baud} {params.parity} unit {params.unit}; sweeping registers …")

            rmap = await asyncio.to_thread(sweep_registers, self._node, params, log=self._log)
            self.candidate = await asyncio.to_thread(infer_profile, rmap, self.operator)
            self._log(f"proposed {len(self.candidate.measurands)} candidate measurand(s); "
                      f"word order {self.candidate.default_word.value}")
            self._set_state(SessionState.PREPARING_PROFILE)
            self._emit_candidate()
        except Exception as exc:  # noqa: BLE001 — surface any failure to the operator
            self._fail(f"search failed: {exc}")

    # --- flow: operator input + payload meter --------------------------------------------------

    def update_operator_input(self, data: dict) -> dict:
        """Apply operator metadata + measurand edits; return the candidate preview + payload status."""
        edits = [MeasurandEdit(**e) for e in data.get("measurand_edits", [])]
        self.operator = OperatorInput(
            manufacturer=data.get("manufacturer", ""),
            model=data.get("model", ""),
            category=data.get("category", "other"),
            label=data.get("label", ""),
            known_baud=data.get("known_baud"),
            known_parity=data.get("known_parity"),
            notes=data.get("notes", ""),
            measurand_edits=edits,
        )
        if self.candidate is not None:
            apply_edits(self.candidate, self.operator)
            # NOTE: do NOT emit a "candidate" event here. This runs on every keystroke (debounced), and
            # the UI rebuilds the whole preparing_profile panel on a candidate event — which destroys
            # the input the operator is typing in, dropping focus and dismissing the on-screen keyboard.
            # The operator's edits are already in the DOM; the caller gets the fresh payload below.
        return {"candidate": self.candidate_dict(), "payload": self.payload_status()}

    def payload_status(self) -> dict:
        """Requested vs 53-byte budget for the operator's currently-included named measurands."""
        if self.candidate is None:
            return {"requested": HEADER_LEN, "budget": DR3_MAX, "over_budget": False, "fields": 0}
        included = [m for m in self.candidate.measurands if m.include_in_payload and m.key]
        requested = HEADER_LEN + sum(_payload_encoding_size(m) for m in included)
        return {
            "requested": requested,
            "budget": DR3_MAX,
            "over_budget": requested > DR3_MAX,
            "fields": len(included),
        }

    # --- flow: confirm + onboard ---------------------------------------------------------------

    def confirm(self) -> None:
        if self.candidate is None:
            raise RuntimeError("no candidate to confirm")
        if self.payload_status()["over_budget"]:
            raise RuntimeError("payload exceeds 53 bytes — deselect measurands before confirming")
        if self._task and not self._task.done():
            raise RuntimeError("busy")
        self._task = asyncio.create_task(self._run_onboard())

    async def _run_onboard(self) -> None:
        try:
            type_byte = await asyncio.to_thread(next_free_type_byte, self.cfg.profiles_dir)
            profile = assemble_profile(self.candidate, self.operator, type_byte)
            self._log(f"assembled profile '{profile['profile_id']}' type_byte={type_byte} "
                      f"({profile['payload']['total_len']} B payload)")

            self._set_state(SessionState.FLASHING)
            self.artifacts = await asyncio.to_thread(write_and_generate, profile, self.cfg.repo_root)
            self._log(f"generated blob ({len(self.artifacts.blob_hex)//2} B) + decoder + catalog")
            await asyncio.to_thread(self._onboarding.flash_node, self.artifacts)

            self._set_state(SessionState.PROBING)
            await asyncio.to_thread(self._onboarding.provision, self.artifacts)

            self._set_state(SessionState.PROFILING)
            await asyncio.to_thread(self._onboarding.install_decoder, self.artifacts)
            result = await asyncio.to_thread(self._onboarding.verify_uplink, self.artifacts)
            if not result.get("decoded"):
                raise RuntimeError("no decoded uplink observed on ChirpStack")

            self._set_state(SessionState.SUCCESS)
            self._log(f"SUCCESS — {profile['profile_id']} onboarded and decoding")
        except Exception as exc:  # noqa: BLE001
            self._fail(f"onboarding failed: {exc}")

    # --- control -------------------------------------------------------------------------------

    def retry(self) -> None:
        if self.port:
            self.start_search(self.port)

    def reset(self) -> None:
        if self._node is not None:
            try:
                self._node.close()
            except Exception:
                pass
        self._node = None
        self._onboarding = None
        self.candidate = None
        self.artifacts = None
        self.error = None
        self.logs.clear()
        self._set_state(SessionState.IDLE)

    def _fail(self, msg: str) -> None:
        self.error = msg
        self._log(msg)
        self._set_state(SessionState.FAILED)
        self.bus.emit(Event("error", msg))

    # --- serialization -------------------------------------------------------------------------

    def candidate_dict(self) -> Optional[dict]:
        if self.candidate is None:
            return None
        return dataclasses.asdict(self.candidate)

    def snapshot(self) -> dict:
        return {
            "state": self.state.value,
            "port": self.port,
            "logs": list(self.logs)[-200:],
            "candidate": self.candidate_dict(),
            "operator": dataclasses.asdict(self.operator),
            "payload": self.payload_status(),
            "artifacts": (dataclasses.asdict(self.artifacts) if self.artifacts else None),
            "error": self.error,
        }
