"""
Integration tests for REQ-build-artifact-api: full POST /build + GET /builds/{tag} flow.

MinIO is mocked via the minio_mock fixture so these tests are self-contained.
The fake_firmware_bin fixture provides >100 bytes of data for upload simulation.

Wave 0 state: RED — api.main and api.services.storage do not exist yet;
tests will error inside the test body. --collect-only succeeds.

Turning GREEN: Plan 01-05 (builds router + storage service).
"""
PINNED_TAG = "phase-6-modbus-green"


def test_post_build_idempotent(settings_override, minio_mock, fake_firmware_bin, client):
    """
    POST /v1/build with the pinned tag returns a Build with:
    - status = "ready"
    - binarySha256 is a non-empty string (hex SHA-256)
    - binaryUrl is a non-empty string (MinIO presigned URL)
    """
    resp = client.post(
        "/v1/build",
        json={"firmwareTag": PINNED_TAG},
        headers=client.auth_header,
    )
    assert resp.status_code == 200

    body = resp.json()
    assert body["firmwareTag"] == PINNED_TAG
    assert body["status"] == "ready"
    assert isinstance(body["binarySha256"], str) and body["binarySha256"]
    assert isinstance(body["binaryUrl"], str) and body["binaryUrl"]


def test_post_build_cached(settings_override, minio_mock, fake_firmware_bin, client):
    """
    Calling POST /v1/build a second time for the same tag must NOT call
    fput_object again (the artifact is already cached in MinIO).

    Verifies idempotence: only one upload per tag.
    """
    # First call — triggers upload
    resp1 = client.post(
        "/v1/build",
        json={"firmwareTag": PINNED_TAG},
        headers=client.auth_header,
    )
    assert resp1.status_code == 200

    upload_count_after_first = minio_mock.fput_object.call_count

    # Second call — should return cached result without re-uploading
    resp2 = client.post(
        "/v1/build",
        json={"firmwareTag": PINNED_TAG},
        headers=client.auth_header,
    )
    assert resp2.status_code == 200
    assert minio_mock.fput_object.call_count == upload_count_after_first, (
        "fput_object should NOT be called again on the second POST (idempotent cache)"
    )


def test_get_build_ready(settings_override, minio_mock, fake_firmware_bin, client):
    """
    After POST /v1/build succeeds, GET /v1/builds/{tag} returns:
    - HTTP 200
    - binaryUrl and binarySha256 populated
    """
    # Trigger build first
    post_resp = client.post(
        "/v1/build",
        json={"firmwareTag": PINNED_TAG},
        headers=client.auth_header,
    )
    assert post_resp.status_code == 200

    # Now fetch the build status
    get_resp = client.get(
        f"/v1/builds/{PINNED_TAG}",
        headers=client.auth_header,
    )
    assert get_resp.status_code == 200

    body = get_resp.json()
    assert body["firmwareTag"] == PINNED_TAG
    assert body["status"] == "ready"
    assert isinstance(body["binaryUrl"], str) and body["binaryUrl"]
    assert isinstance(body["binarySha256"], str) and body["binarySha256"]
