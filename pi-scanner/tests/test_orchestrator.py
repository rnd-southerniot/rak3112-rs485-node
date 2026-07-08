"""P4 gate: the orchestrator drives a full mock-node dry-run through every state, the candidate is

editable, and the payload meter blocks Confirm above 53 bytes. Runs against a temp copy of
device-profiles/ so the real repo is never mutated.
"""

from __future__ import annotations

import asyncio
import dataclasses
import os
import shutil

from app.config import CONFIG
from app.events import EventBus
from app.models import SessionState
from app.orchestrator import Orchestrator

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _temp_repo(tmp_path) -> str:
    """Copy device-profiles/ into a throwaway repo root so onboarding writes there, not the real repo."""
    dst = tmp_path / "repo"
    (dst).mkdir()
    shutil.copytree(os.path.join(REPO, "device-profiles"), dst / "device-profiles")
    return str(dst)


def _name_all(orch: Orchestrator, included: bool) -> None:
    edits = [
        {"fc": m.fc, "addr": m.addr, "key": f"m{m.addr}", "name": f"reg {m.addr}",
         "unit": "x", "include_in_payload": included}
        for m in orch.candidate.measurands
    ]
    orch.update_operator_input({"manufacturer": "ACME", "model": "TM9000",
                                "category": "energy-meter", "measurand_edits": edits})


def test_full_mock_dry_run(tmp_path):
    async def drive():
        bus = EventBus()
        bus.bind_loop(asyncio.get_running_loop())
        cfg = dataclasses.replace(CONFIG, repo_root=_temp_repo(tmp_path))
        orch = Orchestrator(bus, cfg)
        states: list[str] = []
        q = bus.subscribe()

        orch.port = "mock"
        await orch._run_search("mock")
        assert orch.state == SessionState.PREPARING_PROFILE
        assert orch.candidate and len(orch.candidate.measurands) == 30

        # candidate is editable: naming a register lands on the measurand
        orch.update_operator_input(
            {"manufacturer": "ACME", "model": "TM9000", "category": "energy-meter",
             "measurand_edits": [{"fc": 4, "addr": 0, "key": "v1n", "name": "V1N", "unit": "V"}]}
        )
        assert any(m.key == "v1n" for m in orch.candidate.measurands)

        # payload meter blocks Confirm when all 30 u16 fields are included (3 + 60 = 63 > 53)
        _name_all(orch, included=True)
        assert orch.payload_status()["over_budget"] is True
        try:
            orch.confirm()
            assert False, "confirm should refuse over-budget payload"
        except RuntimeError:
            pass

        # keep only a few → under budget → onboard walks flashing→probing→profiling→success
        keep = {0, 2, 56}
        edits = [
            {"fc": m.fc, "addr": m.addr, "key": f"m{m.addr}", "name": f"r{m.addr}", "unit": "x",
             "include_in_payload": m.addr in keep}
            for m in orch.candidate.measurands
        ]
        orch.update_operator_input({"manufacturer": "ACME", "model": "TM9000",
                                    "category": "energy-meter", "measurand_edits": edits})
        assert orch.payload_status()["over_budget"] is False

        await orch._run_onboard()
        assert orch.state == SessionState.SUCCESS, orch.error
        assert orch.artifacts and os.path.exists(orch.artifacts.profile_path)

        # every visited state was published
        seen = set()
        while not q.empty():
            ev = q.get_nowait()
            if ev.type == "state":
                seen.add(ev.data)
        for s in ("searching", "preparing_profile", "flashing", "probing", "profiling", "success"):
            assert s in seen, f"state {s} was never emitted; saw {seen}"

    asyncio.run(drive())
