#!/usr/bin/env python3
"""lorawan-deprovision — delete a test device from Southern IoT ChirpStack and confirm removal.

Given a DevEUI (argv[1] or DEV_EUI env): DeviceService.Delete, then confirm via DeviceService.List
(a single Get can read stale right after a delete, so we list-and-check instead).

Env: CS_BASE, UA, CS_API_TOKEN (or CS_ADMIN_USER/CS_ADMIN_PASS).
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


def die(msg, code=2):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(code)


def main():
    dev_eui = (os.environ.get("DEV_EUI") or (sys.argv[1] if len(sys.argv) > 1 else "")).lower()
    if len(dev_eui) != 16:
        die("usage: deprovision.py <dev_eui-16hex>   (or set DEV_EUI)")

    try:
        jwt = cs.auth()
        # Delete is idempotent here: ALREADY-gone returns None, which we ignore.
        cs.call("/api.DeviceService/Delete", api.DeleteDeviceRequest(dev_eui=dev_eui),
                empty_pb2.Empty, jwt)
        # Confirm via List (search matches name or DevEUI) — robust against read-after-delete staleness.
        lst = cs.call("/api.DeviceService/List",
                      api.ListDevicesRequest(application_id=APP_ID, limit=100, search=dev_eui),
                      api.ListDevicesResponse, jwt)
    except KeyError as e:
        die(f"missing env var {e}; set CS_BASE + UA and CS_API_TOKEN (or admin creds)")
    except urllib.error.HTTPError as e:
        die(f"HTTP {e.code} — Cloudflare needs a browser UA (UA env), or auth is wrong "
            "(do NOT rotate the token)")
    except urllib.error.URLError as e:
        die(f"cannot reach CS_BASE ({os.environ.get('CS_BASE')}): {e.reason}")

    still_there = bool(lst) and any(d.dev_eui.lower() == dev_eui for d in lst.result)
    if still_there:
        die(f"device {dev_eui} still present after Delete — retry, or check token scope")
    print(f"PASS: device {dev_eui} removed (confirmed absent from application {APP_ID}).")


if __name__ == "__main__":
    main()
