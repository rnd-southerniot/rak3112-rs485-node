"""
Unit tests for REQ-firmware-private-deploy (auth slice).

Verifies that all /v1/* endpoints reject requests with no or wrong tokens.

Wave 0 state: RED — api.main does not exist yet; client fixture will error
inside the test body. --collect-only succeeds.

Turning GREEN: Plan 01-05 (all routers mounted with verify_bearer dependency).
"""


def test_no_token_rejected(settings_override, client):
    """
    GET /v1/sensors with no Authorization header returns:
    - HTTP 401 (FastAPI HTTPBearer returns 403 for missing, but our auth returns 401)
    - body: { "error": { "code": "unauthorized", "message": <str> } }

    Note: FastAPI's HTTPBearer scheme returns 403 if the Authorization header
    is completely absent. The test checks for any 4xx + the error shape.
    We specifically test the wrong-token path which returns 401.
    """
    # Test 1: completely missing Authorization header
    resp_no_auth = client.get("/v1/sensors")
    assert resp_no_auth.status_code in (401, 403), (
        f"Missing auth should return 401 or 403, got {resp_no_auth.status_code}"
    )

    # Test 2: wrong bearer token → must return 401 with our error shape
    resp_bad_token = client.get(
        "/v1/sensors",
        headers={"Authorization": "Bearer wrong-token-garbage"},
    )
    assert resp_bad_token.status_code == 401

    body = resp_bad_token.json()
    assert "error" in body, "Response must have top-level 'error' key"
    assert body["error"]["code"] == "unauthorized"
    assert isinstance(body["error"]["message"], str) and body["error"]["message"]
