"""
Unit tests for REQ-sensors-api: GET /v1/sensors with flashable computation.

Wave 0 state: RED — api.services.profiles does not exist yet; these tests
will error at import time inside the test body and show as ERROR in pytest
output. That is expected. --collect-only succeeds.

Turning GREEN: Plan 01-02 (sensors router + profiles service).
"""
import pytest

# Only stdlib + api.models/api.config at top level (Wave 0 guard)
from api.models import Sensor


def test_flashable_flag(compiled_readers, profiles_dir):
    """
    load_sensors against the fixture data:
    - MFM384 (deviceByte 1) and RS-FSJT (deviceByte 2) → flashable=True
    - EEM400 (deviceByte 3) and DSE (deviceByte 4) → flashable=False
    """
    # Lazy import — feature module absent in Wave 0
    from api.services.profiles import load_sensors  # noqa

    _, cr_path = compiled_readers
    sensors = load_sensors(
        compiled_readers_path=str(cr_path),
        profiles_dir=str(profiles_dir),
    )

    by_byte = {s.deviceByte: s for s in sensors}

    assert by_byte[1].flashable is True, "MFM384 should be flashable"
    assert by_byte[2].flashable is True, "RS-FSJT should be flashable"
    assert by_byte[3].flashable is False, "EEM400 should NOT be flashable"
    assert by_byte[4].flashable is False, "DSE should NOT be flashable"


def test_sensor_shape(compiled_readers, profiles_dir):
    """
    Every returned object must have all frozen Sensor fields with correct types.
    """
    from api.services.profiles import load_sensors  # noqa

    _, cr_path = compiled_readers
    sensors = load_sensors(
        compiled_readers_path=str(cr_path),
        profiles_dir=str(profiles_dir),
    )

    assert len(sensors) == 4, "Expected 4 device profiles"

    for s in sensors:
        assert isinstance(s, Sensor)
        # Verify all frozen-contract fields are present and typed
        assert isinstance(s.profileKey, str) and s.profileKey
        assert isinstance(s.displayName, str) and s.displayName
        assert isinstance(s.manufacturer, str) and s.manufacturer
        assert isinstance(s.model, str) and s.model
        assert isinstance(s.deviceByte, int) and s.deviceByte > 0
        assert s.modbus is not None
        assert isinstance(s.modbus.baud, int)
        assert isinstance(s.modbus.parity, str)
        assert isinstance(s.modbus.stopBits, int)
        assert isinstance(s.modbus.functionCode, int)
        assert isinstance(s.modbus.wordOrder, str)
        assert isinstance(s.payloadBytes, int) and s.payloadBytes > 0
        assert isinstance(s.flashable, bool)
        assert isinstance(s.isActive, bool)


def test_flashable_filter(settings_override, compiled_readers, profiles_dir, client):
    """
    GET /v1/sensors?flashable=true excludes EEM400 (deviceByte 3) and DSE (deviceByte 4).
    """
    # client fixture does lazy import of api.main
    resp = client.get("/v1/sensors", params={"flashable": "true"}, headers=client.auth_header)
    assert resp.status_code == 200

    sensors = resp.json()
    device_bytes = [s["deviceByte"] for s in sensors]

    assert 1 in device_bytes, "MFM384 (deviceByte 1) should appear"
    assert 2 in device_bytes, "RS-FSJT (deviceByte 2) should appear"
    assert 3 not in device_bytes, "EEM400 (deviceByte 3) should be excluded"
    assert 4 not in device_bytes, "DSE (deviceByte 4) should be excluded"
