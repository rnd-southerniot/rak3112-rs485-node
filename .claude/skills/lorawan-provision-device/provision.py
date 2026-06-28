#!/usr/bin/env python3
"""lorawan-provision-device — create ONE OTAA device in Southern IoT ChirpStack via gRPC-web.

Generates a random DevEUI (8 bytes) + AppKey (16 bytes) unless DEV_EUI / APP_KEY are set (env or
argv[1] for the DevEUI). Names it fw-<eui-tail>. Idempotent: an existing device is fine
(ALREADY_EXISTS is ignored; the stored key is read back so the printed AppKey is always authoritative).

This creates REAL state on the production cluster — always pair with lorawan-deprovision for tests.

Env: CS_BASE, UA, and CS_API_TOKEN (preferred) or CS_ADMIN_USER/CS_ADMIN_PASS. Known IDs default to
the Southern IoT production app + OTAA-AS923 profile; override via CS_APPLICATION_ID /
CS_DEVICE_PROFILE_ID.
"""
import os
import sys
import urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "_shared"))
try:  # cs_grpcweb reads CS_BASE at import; fail cleanly instead of tracebacking
    import cs_grpcweb as cs  # noqa: E402
    from chirpstack_api import api  # noqa: E402
    from google.protobuf import empty_pb2  # noqa: E402
except KeyError as _e:
    sys.exit(f"FAIL: missing env var {_e}; set CS_BASE, UA, and CS_API_TOKEN (or admin creds)")
except ModuleNotFoundError as _e:
    sys.exit(f"FAIL: {_e}; run: uv venv .v && . .v/bin/activate && uv pip install chirpstack-api grpcio")

APP_ID = os.environ.get("CS_APPLICATION_ID", "7d5c0d50-4a4b-496d-ae7f-8ac61c4b3b18")
DP_ID = os.environ.get("CS_DEVICE_PROFILE_ID", "b14d1236-070b-49bb-a401-215bb29ae2b2")


def die(msg, code=2):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(code)


def main():
    try:
        jwt = cs.auth()
    except KeyError as e:
        die(f"missing env var {e}; set CS_BASE + UA and CS_API_TOKEN (preferred) "
            "or CS_ADMIN_USER/CS_ADMIN_PASS")
    except urllib.error.HTTPError as e:
        die(f"login HTTP {e.code} — Cloudflare needs a browser UA (UA env), or admin creds are wrong")
    except urllib.error.URLError as e:
        die(f"cannot reach CS_BASE ({os.environ.get('CS_BASE')}): {e.reason}")

    dev_eui = (os.environ.get("DEV_EUI") or (sys.argv[1] if len(sys.argv) > 1 else "")
               or os.urandom(8).hex()).lower()
    app_key = (os.environ.get("APP_KEY") or os.urandom(16).hex()).lower()
    if len(dev_eui) != 16 or any(c not in "0123456789abcdef" for c in dev_eui):
        die("DEV_EUI must be 16 hex chars (8 bytes)")
    if len(app_key) != 32 or any(c not in "0123456789abcdef" for c in app_key):
        die("APP_KEY must be 32 hex chars (16 bytes)")
    name = f"fw-{dev_eui[-6:]}"

    dev = api.Device(dev_eui=dev_eui, name=name, application_id=APP_ID, device_profile_id=DP_ID,
                     is_disabled=False, description="firmware bring-up — auto-provisioned (delete after test)")
    try:
        # Create then CreateKeys. Both return Empty on success / None on ALREADY_EXISTS (idempotent).
        cs.call("/api.DeviceService/Create", api.CreateDeviceRequest(device=dev), empty_pb2.Empty, jwt)
        # OTAA LoRaWAN 1.0.x: set BOTH nwk_key and app_key to the AppKey.
        keys = api.DeviceKeys(dev_eui=dev_eui, nwk_key=app_key, app_key=app_key)
        cs.call("/api.DeviceService/CreateKeys", api.CreateDeviceKeysRequest(device_keys=keys),
                empty_pb2.Empty, jwt)
        # Confirm presence (robust against the gRPC-web Empty/ALREADY_EXISTS ambiguity).
        got = cs.call("/api.DeviceService/Get", api.GetDeviceRequest(dev_eui=dev_eui),
                      api.GetDeviceResponse, jwt)
        # Read the stored key back so the printed AppKey is authoritative even on ALREADY_EXISTS.
        gk = cs.call("/api.DeviceService/GetKeys", api.GetDeviceKeysRequest(dev_eui=dev_eui),
                     api.GetDeviceKeysResponse, jwt)
    except urllib.error.HTTPError as e:
        die(f"HTTP {e.code} during create/confirm — Cloudflare UA or auth issue (do NOT rotate the token)")
    except urllib.error.URLError as e:
        die(f"network error: {e.reason}")

    if got is None or not got.device.dev_eui:
        die("device not present after Create — check CS_API_TOKEN scope + network. "
            "(A tenant token reads UNAUTHENTICATED for a missing device; that is NOT a bad token.)")
    stored_key = (gk.device_keys.app_key.lower() if gk and gk.device_keys.app_key else app_key)

    print("PASS: device provisioned")
    print(f"  name      {got.device.name}")
    print(f"  DevEUI    {dev_eui}")
    print(f"  AppKey    {stored_key}")
    print("  JoinEUI   0000000000000000   (AS923 OTAA — AppEUI/JoinEUI unconstrained on this profile)")
    print(f"  app_id    {APP_ID}")
    print("Flash into firmware (docs/LORAWAN_PROVISIONING.md), then run lorawan-verify-join.")
    print(f"CLEANUP when done:  DEV_EUI={dev_eui}  ->  lorawan-deprovision")


if __name__ == "__main__":
    main()
