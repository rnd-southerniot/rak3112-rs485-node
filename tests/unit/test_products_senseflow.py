"""P5.1 multi-product tests — senseflow served as a 2nd product (careflow stays default).

A self-contained fixture stands up a fake senseflow artifact (bin + boot parts + device-profiles
catalog + compiled_sensors.json) under SENSEFLOW_ROOT and points get_settings at it, so the product
registry registers senseflow. Careflow behavior is proven unchanged by the existing test suite.
"""
import json

import pytest

from api.config import Settings, get_settings

TEST_TOKEN = "test-token"

# The on-hardware-verified BME280 v2 blob (== C dp_serialize; see firmware P4.2/P4.3).
BME280_BLOB_HEX = (
    "02100176010000000000000000000000000007020200000004003f80000000000000"
    "00000004003f8000000000000000000342c8000001020241200000aa6b"
)


@pytest.fixture
def senseflow_client(tmp_path, monkeypatch):
    """TestClient with a senseflow product registered from a fake artifact under SENSEFLOW_ROOT."""
    root = tmp_path / "senseflow"
    (root / "device-profiles").mkdir(parents=True)
    # firmware binary + boot parts (fake bytes; the flash-manifest reads them by name)
    (root / "senseflow_eink_node.bin").write_bytes(b"\x00" * 200)
    for boot in ("bootloader.bin", "partition-table.bin", "ota_data.bin"):
        (root / boot).write_bytes(b"\x00" * 64)
    # sensor manifest — bme280 driver compiled in → flashable
    (root / "compiled_sensors.json").write_text(json.dumps(["bme280"]))
    # device-profiles catalog entry (blobHex + i2c metadata)
    (root / "device-profiles" / "bosch-bme280.json").write_text(json.dumps({
        "profileKey": "bosch-bme280",
        "displayName": "Bosch BME280/BMP280",
        "manufacturer": "Bosch",
        "model": "BME280",
        "deviceByte": 16,
        "sensorType": "bme280",
        "bus": "i2c",
        "i2cAddr": 118,
        "measurands": ["temp", "pressure"],
        "payloadBytes": 7,
        "isActive": True,
        "blobHex": BME280_BLOB_HEX,
    }))

    get_settings.cache_clear()
    settings = Settings(
        API_TOKEN=TEST_TOKEN,
        MINIO_ACCESS_KEY="k",
        MINIO_SECRET_KEY="s",
        FIRMWARE_TAG="phase-6-modbus-green",
        SENSEFLOW_ROOT=str(root),
        SENSEFLOW_FIRMWARE_TAG="senseflow-p4-green",
    )
    monkeypatch.setattr("api.config.get_settings", lambda: settings)

    import api.main
    from fastapi.testclient import TestClient

    tc = TestClient(api.main.app)
    tc.auth_header = {"Authorization": f"Bearer {TEST_TOKEN}"}
    yield tc
    get_settings.cache_clear()


def test_products_lists_both(senseflow_client):
    r = senseflow_client.get("/v1/products", headers=senseflow_client.auth_header)
    assert r.status_code == 200
    ids = {p["id"]: p for p in r.json()}
    assert set(ids) == {"careflow", "senseflow"}
    assert ids["senseflow"]["bus"] == "i2c"
    assert ids["senseflow"]["firmwareTag"] == "senseflow-p4-green"
    assert ids["careflow"]["bus"] == "modbus"


def test_v2_sensors_senseflow_bme280(senseflow_client):
    r = senseflow_client.get(
        "/v2/sensors", params={"product": "senseflow"}, headers=senseflow_client.auth_header
    )
    assert r.status_code == 200
    sensors = r.json()
    assert len(sensors) == 1
    s = sensors[0]
    assert s["profileKey"] == "bosch-bme280"
    assert s["bus"] == "i2c"
    assert s["modbus"] is None
    assert s["i2c"] == {"addr": 118, "sensorType": "bme280"}
    assert s["deviceByte"] == 16
    assert s["flashable"] is True  # bme280 in compiled_sensors.json
    assert s["measurands"] == ["temp", "pressure"]


def test_profile_blob_senseflow_matches_firmware(senseflow_client):
    r = senseflow_client.get(
        "/v1/profile-blob/bosch-bme280",
        params={"product": "senseflow"},
        headers=senseflow_client.auth_header,
    )
    assert r.status_code == 200
    body = r.json()
    assert body["deviceByte"] == 16
    assert body["blobHex"] == BME280_BLOB_HEX  # byte-identical to the C dp_serialize / on-hardware


def test_provisioning_protocol_senseflow_has_profile_no_modbus(senseflow_client):
    r = senseflow_client.get(
        "/v1/provisioning-protocol",
        params={"product": "senseflow"},
        headers=senseflow_client.auth_header,
    )
    assert r.status_code == 200
    body = r.json()
    ids = {c["id"] for c in body["commands"]}
    assert "prov-profile" in ids
    assert "prov-creds" in ids
    assert "prov-modbus" not in ids
    assert "modbus" not in body["bootVerify"]["markers"]
    assert body["firmwareTag"] == "senseflow-p4-green"


def test_flash_manifest_senseflow(senseflow_client):
    r = senseflow_client.get(
        "/v1/flash-manifest", params={"product": "senseflow"}, headers=senseflow_client.auth_header
    )
    assert r.status_code == 200
    body = r.json()
    assert body["firmwareTag"] == "senseflow-p4-green"
    names = [p["name"] for p in body["parts"]]
    assert names == ["bootloader", "partition-table", "nvs-blank", "ota-data", "app"]


def test_unknown_product_404(senseflow_client):
    r = senseflow_client.get(
        "/v2/sensors", params={"product": "nope"}, headers=senseflow_client.auth_header
    )
    assert r.status_code == 404
    assert r.json()["error"]["code"] == "unknown_product"


def test_v1_sensors_still_careflow_default(senseflow_client):
    """v1 /v1/sensors is unchanged (careflow default); it must not error with senseflow registered."""
    r = senseflow_client.get("/v1/sensors", headers=senseflow_client.auth_header)
    assert r.status_code == 200  # careflow profiles load from the repo device-profiles/
