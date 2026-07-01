"""
Unit tests for REQ-build-artifact-api (unit slice): 404 for unbuilt tags.

Wave 0 state: RED — api.main does not exist yet; client fixture will error
inside the test body. --collect-only succeeds.

Turning GREEN: Plan 01-05 (builds router + storage service).
"""


def test_get_build_not_found(settings_override, client):
    """
    GET /v1/builds/{tag} for a tag that was never built returns:
    - HTTP 404
    - body: { "error": { "code": "not_found", "message": <str> } }
    """
    resp = client.get("/v1/builds/never-built-tag", headers=client.auth_header)
    assert resp.status_code == 404

    body = resp.json()
    assert "error" in body, "Response must have top-level 'error' key"
    assert body["error"]["code"] == "not_found"
    assert isinstance(body["error"]["message"], str) and body["error"]["message"]
