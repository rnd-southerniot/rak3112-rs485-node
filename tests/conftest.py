"""
Shared pytest fixtures for the rak3112 firmware service test suite.

Top-level import rule: ONLY stdlib + api.models / api.config allowed here.
All feature-module imports (api.main, api.services.*, api.routers.*) are
done LAZILY inside fixture bodies so --collect-only succeeds in Wave 0
before those modules are created.
"""
import json
import os
import tempfile
from pathlib import Path
from typing import Generator
from unittest.mock import MagicMock

import pytest

# Allowed top-level imports (api.models + api.config exist in Wave 0)
from api.config import Settings, get_settings

# ---------------------------------------------------------------------------
# Settings override fixture
# ---------------------------------------------------------------------------

TEST_TOKEN = "test-token"


@pytest.fixture(autouse=False)
def settings_override(monkeypatch) -> Settings:
    """
    Override get_settings() with a known test configuration.

    Returns the overridden Settings instance so tests can inspect values.
    Auto-applied only when explicitly requested — not autouse=True so that
    tests that want a clean env can opt out.
    """
    get_settings.cache_clear()

    test_settings = Settings(
        API_TOKEN=TEST_TOKEN,
        MINIO_INTERNAL_ENDPOINT="minio-test:9000",
        MINIO_EXTERNAL_ENDPOINT="minio-test:9000",
        MINIO_ACCESS_KEY="test-access-key",
        MINIO_SECRET_KEY="test-secret-key",
        MINIO_BUCKET="test-firmware-artifacts",
        FIRMWARE_TAG="phase-6-modbus-green",
        PRESIGNED_EXPIRY_HOURS=1,
    )
    monkeypatch.setattr("api.config.get_settings", lambda: test_settings)

    yield test_settings

    get_settings.cache_clear()


# ---------------------------------------------------------------------------
# compiled_readers fixture
# ---------------------------------------------------------------------------

COMPILED_READERS = ["mfm384", "rsfsjt"]


@pytest.fixture
def compiled_readers(tmp_path) -> tuple[list[str], Path]:
    """
    Return the pinned compiled-reader list and write compiled_readers.json
    to a temp path. The path is what api.services.profiles.load_sensors will
    read when the COMPILED_READERS_PATH env var is set by a wrapping fixture.

    Returns: (readers_list, json_path)
    """
    json_path = tmp_path / "compiled_readers.json"
    json_path.write_text(json.dumps(COMPILED_READERS))
    return COMPILED_READERS, json_path


# ---------------------------------------------------------------------------
# profiles_dir fixture — 4 device-profile JSONs seeded in a tmp dir
# ---------------------------------------------------------------------------

PROFILES = [
    {
        "profileKey": "selec-mfm384",
        "displayName": "SELEC MFM384 3-Phase Energy Meter",
        "manufacturer": "SELEC",
        "model": "MFM384",
        "deviceByte": 1,
        "modbus": {
            "baud": 9600,
            "parity": "N",
            "stopBits": 1,
            "functionCode": 4,
            "wordOrder": "CDAB",
        },
        "payloadBytes": 19,
        "isActive": True,
    },
    {
        "profileKey": "rs-fsjt-n01",
        "displayName": "RS-FSJT-N01 Wind Speed Sensor",
        "manufacturer": "RS",
        "model": "RS-FSJT-N01",
        "deviceByte": 2,
        "modbus": {
            "baud": 4800,
            "parity": "N",
            "stopBits": 1,
            "functionCode": 3,
            "wordOrder": "AB",
        },
        "payloadBytes": 5,
        "isActive": True,
    },
    {
        "profileKey": "honeywell-eem400",
        "displayName": "Honeywell EEM400 Energy Meter",
        "manufacturer": "Honeywell",
        "model": "EEM400",
        "deviceByte": 3,
        "modbus": {
            "baud": 9600,
            "parity": "N",
            "stopBits": 1,
            "functionCode": 3,
            "wordOrder": "AB",
        },
        "payloadBytes": 20,
        "isActive": False,
    },
    {
        "profileKey": "deepsea-dse",
        "displayName": "DeepSea DSE Generator Controller",
        "manufacturer": "DeepSea",
        "model": "DSE",
        "deviceByte": 4,
        "modbus": {
            "baud": 9600,
            "parity": "N",
            "stopBits": 1,
            "functionCode": 3,
            "wordOrder": "AB",
        },
        "payloadBytes": 24,
        "isActive": False,
    },
]


@pytest.fixture
def profiles_dir(tmp_path) -> Path:
    """
    Create a tmp dir seeded with the 4 device-profile JSONs.

    Note: 'flashable' is NOT stored in the JSON — it is computed at runtime
    from compiled_readers.json. EEM400 (deviceByte 3) and DSE (deviceByte 4)
    are isActive=False.
    """
    profiles_path = tmp_path / "device-profiles"
    profiles_path.mkdir()
    for profile in PROFILES:
        (profiles_path / f"{profile['profileKey']}.json").write_text(
            json.dumps(profile)
        )
    return profiles_path


# ---------------------------------------------------------------------------
# fake_firmware_bin fixture
# ---------------------------------------------------------------------------


@pytest.fixture
def fake_firmware_bin(tmp_path) -> Path:
    """A tmp file with >100 bytes of fake firmware data for storage tests."""
    bin_path = tmp_path / "firmware.bin"
    bin_path.write_bytes(b"\x00" * 200)
    return bin_path


# ---------------------------------------------------------------------------
# minio_mock fixture
# ---------------------------------------------------------------------------


@pytest.fixture
def minio_mock():
    """
    Stub MinIO client:
    - stat_object raises S3Error(NoSuchKey) until fput_object has been called
    - presigned_get_object returns a stable fake URL
    - fput_object is tracked via call_count

    Matches Pitfall 2: minio-py raises S3Error, not returns None.
    """
    # Import S3Error lazily (minio is a runtime dep, not stdlib)
    from minio.error import S3Error

    mock = MagicMock()
    uploaded_keys: set[str] = set()

    def _stat_object(bucket: str, key: str, **kwargs):
        if key not in uploaded_keys:
            # Simulate S3Error for NoSuchKey — same as real minio-py behaviour
            err = S3Error(
                code="NoSuchKey",
                message="Object does not exist",
                resource=f"/{bucket}/{key}",
                request_id="test-req",
                host_id="test-host",
                response=MagicMock(status=404, headers={}, text=""),
            )
            raise err
        stat = MagicMock()
        stat.size = 200
        return stat

    def _fput_object(bucket: str, key: str, file_path: str, **kwargs):
        uploaded_keys.add(key)
        return MagicMock(etag="fake-etag")

    def _presigned_get_object(bucket: str, key: str, **kwargs):
        return f"http://minio-test:9000/{bucket}/{key}?presigned=1"

    mock.stat_object.side_effect = _stat_object
    mock.fput_object.side_effect = _fput_object
    mock.presigned_get_object.side_effect = _presigned_get_object

    # Expose uploaded_keys so tests can inspect state
    mock._uploaded_keys = uploaded_keys

    return mock


# ---------------------------------------------------------------------------
# client fixture — lazy import of api.main inside fixture body
# ---------------------------------------------------------------------------


@pytest.fixture
def client(settings_override):
    """
    Build a FastAPI TestClient with the test bearer token pre-configured.

    api.main is imported LAZILY (inside this fixture body) so that
    --collect-only succeeds in Wave 0 before main.py exists.

    Returns an httpx TestClient. Use client.get("/v1/sensors",
    headers={"Authorization": "Bearer test-token"}) pattern.
    """
    # Lazy import — Wave 0 guard
    import api.main  # noqa: F401 — triggers app creation

    from fastapi.testclient import TestClient

    tc = TestClient(api.main.app)

    # Attach a helper so tests can include the auth header easily
    tc.auth_header = {"Authorization": f"Bearer {TEST_TOKEN}"}
    return tc
