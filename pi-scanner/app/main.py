"""FastAPI entrypoint — REST + WebSocket for the kiosk UI, static web/ mount.

Run: ``uvicorn app.main:app --host 127.0.0.1 --port 8080`` (a systemd unit boots this; Chromium kiosk
points at localhost). One global Orchestrator/session — a station scans one device at a time.
"""

from __future__ import annotations

import asyncio
import json
import os
from contextlib import asynccontextmanager

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

from .config import CONFIG
from .events import EventBus
from .orchestrator import Orchestrator

bus = EventBus()
orch = Orchestrator(bus, CONFIG)

_WEB_DIR = os.path.join(os.path.dirname(__file__), "..", "web")


@asynccontextmanager
async def lifespan(_app: "FastAPI"):
    bus.bind_loop(asyncio.get_running_loop())
    yield


app = FastAPI(title="Careflow RS-485 Scanner Station", lifespan=lifespan)


@app.get("/api/ports")
def list_ports() -> dict:
    ports = ["mock"]
    try:
        from serial.tools import list_ports as lp

        ports += [p.device for p in lp.comports()]
    except Exception:
        pass
    return {"ports": ports}


@app.get("/api/session")
def get_session() -> dict:
    return orch.snapshot()


@app.post("/api/session/start")
async def start(body: dict) -> JSONResponse:
    port = (body or {}).get("port") or CONFIG.node_port
    try:
        orch.start_search(port)
    except RuntimeError as exc:
        return JSONResponse({"error": str(exc)}, status_code=409)
    return JSONResponse({"ok": True, "port": port}, status_code=202)


@app.post("/api/session/operator-input")
async def operator_input(body: dict) -> dict:
    return orch.update_operator_input(body or {})


@app.post("/api/session/confirm")
async def confirm() -> JSONResponse:
    try:
        orch.confirm()
    except RuntimeError as exc:
        return JSONResponse({"error": str(exc)}, status_code=409)
    return JSONResponse({"ok": True}, status_code=202)


@app.post("/api/session/retry")
async def retry() -> dict:
    orch.retry()
    return {"ok": True}


@app.post("/api/session/reset")
async def reset() -> dict:
    orch.reset()
    return {"ok": True}


@app.post("/api/kiosk/keyboard")
async def kiosk_keyboard(body: dict) -> dict:
    """Show/hide the on-screen keyboard (squeekboard, via its DBus interface). No-op off-Pi."""
    import subprocess

    show = "true" if bool((body or {}).get("show", True)) else "false"
    try:
        subprocess.run(
            ["busctl", "--user", "set-property", "sm.puri.OSK0", "/sm/puri/OSK0",
             "sm.puri.OSK0", "Visible", "b", show],
            timeout=3, capture_output=True,
        )
    except Exception:
        pass  # not on a squeekboard kiosk — harmless
    return {"ok": True}


@app.post("/api/kiosk/exit")
async def kiosk_exit() -> dict:
    """Close the kiosk browser -> back to the desktop (touch-friendly exit; no keyboard needed)."""
    import subprocess

    try:
        subprocess.Popen(["pkill", "-f", "chromium"])
    except Exception:
        pass
    return {"ok": True}


@app.websocket("/ws")
async def ws(sock: WebSocket) -> None:
    await sock.accept()
    q = bus.subscribe()
    # send the current snapshot so a late-joining UI is immediately consistent
    await sock.send_text(json.dumps({"type": "snapshot", "data": orch.snapshot()}))
    try:
        while True:
            ev = await q.get()
            await sock.send_text(json.dumps({"type": ev.type, "data": ev.data}))
    except WebSocketDisconnect:
        pass
    finally:
        bus.unsubscribe(q)


# Static kiosk UI last, so /api and /ws win. html=True serves index.html at /.
if os.path.isdir(_WEB_DIR):
    app.mount("/", StaticFiles(directory=_WEB_DIR, html=True), name="web")
