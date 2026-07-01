"""
Integration-test conftest: overrides the root `client` fixture to inject
MinIO mocks and a fake firmware binary into app.state.

Why this is needed:
  - TestClient without a `with` block does NOT trigger the lifespan startup.
  - Integration tests call POST /v1/build which reads state.minio_internal,
    state.minio_external, and state.firmware_path directly (no getattr fallback).
  - Without pre-set state the route raises AttributeError → 500.

This conftest overrides the `client` fixture for tests under tests/integration/
only (pytest resolves fixtures from the nearest conftest first).  The root
conftest.py fixture continues to serve unit tests unchanged.
"""
import pytest

# Mirror the test token from the root conftest
TEST_TOKEN = "test-token"


@pytest.fixture
def client(settings_override, minio_mock, fake_firmware_bin):
    """
    TestClient with MinIO mocks and fake firmware path pre-loaded into app.state.

    Dependency resolution order (pytest fixture graph):
      settings_override → monkeypatches api.config.get_settings
      minio_mock        → MagicMock with stat_object / fput_object / presigned_get_object
      fake_firmware_bin → tmp_path / "firmware.bin" (200 bytes of zeros)

    We set app.state attributes BEFORE creating the TestClient so that the
    first request sees the correct state (lifespan doesn't run without `with`).

    Teardown: deletes the injected attributes to prevent state leakage into
    subsequent unit tests that rely on getattr(state, "minio_internal", None)
    returning None.
    """
    # Lazy import — honors Wave 0 guard; api.main must exist by Plan 01-05
    import api.main
    from fastapi.testclient import TestClient

    # Inject mocks into the module-level app singleton BEFORE any request
    api.main.app.state.minio_internal = minio_mock
    api.main.app.state.minio_external = minio_mock
    api.main.app.state.firmware_path = fake_firmware_bin
    api.main.app.state.firmware_bucket = settings_override.MINIO_BUCKET

    tc = TestClient(api.main.app)
    tc.auth_header = {"Authorization": f"Bearer {TEST_TOKEN}"}

    yield tc

    # --- Teardown: remove injected attributes to avoid cross-test pollution ---
    # Unit tests that follow (e.g. test_get_build_not_found) rely on
    # minio_internal being ABSENT from state (the getattr fallback → 404).
    for attr in ("minio_internal", "minio_external", "firmware_path", "firmware_bucket"):
        try:
            delattr(api.main.app.state, attr)
        except AttributeError:
            pass
