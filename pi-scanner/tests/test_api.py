"""Smoke: the FastAPI app boots, serves the kiosk UI, and exposes the session/ports endpoints."""

from __future__ import annotations

from fastapi.testclient import TestClient

from app.main import app


def test_api_and_static():
    with TestClient(app) as c:
        ports = c.get("/api/ports")
        assert ports.status_code == 200 and "mock" in ports.json()["ports"]

        snap = c.get("/api/session")
        assert snap.status_code == 200 and snap.json()["state"] == "idle"

        index = c.get("/")
        assert index.status_code == 200 and "RS-485 Scanner" in index.text

        assert c.get("/app.js").status_code == 200
