#!/usr/bin/env python3
"""lorawan-verify-join — report a device's presence + activation in Southern IoT ChirpStack.

Given a DevEUI (argv[1] or DEV_EUI env): prints whether the device exists, its last_seen_at, and the
OTAA activation DevAddr (DeviceService.GetActivation). Read-only — creates no state.

Note: a tenant-scoped API key returns UNAUTHENTICATED (not NOT_FOUND) for a device that does not
exist yet; this client maps both to "not found" — that is a ChirpStack quirk, NOT a bad token.
Live uplinks stream on MQTT topic application/<APP_ID>/device/<eui>/event/up.

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
        die("usage: verify.py <dev_eui-16hex>   (or set DEV_EUI)")

    try:
        jwt = cs.auth()
        got = cs.call("/api.DeviceService/Get", api.GetDeviceRequest(dev_eui=dev_eui),
                      api.GetDeviceResponse, jwt)
    except KeyError as e:
        die(f"missing env var {e}; set CS_BASE + UA and CS_API_TOKEN (or admin creds)")
    except urllib.error.HTTPError as e:
        die(f"HTTP {e.code} — Cloudflare needs a browser UA (UA env), or auth is wrong "
            "(do NOT rotate the token)")
    except urllib.error.URLError as e:
        die(f"cannot reach CS_BASE ({os.environ.get('CS_BASE')}): {e.reason}")

    if got is None or not got.device.dev_eui:
        print(f"NOT FOUND: no device {dev_eui} (or token not authorized for it — same signal here).")
        print("If you JUST provisioned it, re-run; otherwise provision it first.")
        sys.exit(1)

    last = got.last_seen_at
    seen = last.ToDatetime().isoformat() + "Z" if last and last.seconds else "never (no uplink yet)"
    print("PASS: device present")
    print(f"  name        {got.device.name}")
    print(f"  DevEUI      {dev_eui}")
    print(f"  last_seen   {seen}")

    try:
        act = cs.call("/api.DeviceService/GetActivation", api.GetDeviceActivationRequest(dev_eui=dev_eui),
                      api.GetDeviceActivationResponse, jwt)
    except urllib.error.HTTPError:
        act = None
    dev_addr = act.device_activation.dev_addr if act and act.device_activation.dev_addr else ""
    if dev_addr:
        print(f"  JOINED      DevAddr {dev_addr}  (OTAA activation present)")
    else:
        print("  activation  none yet — device has not completed an OTAA join")
    print(f"  uplinks     MQTT topic application/{APP_ID}/device/{dev_eui}/event/up")


if __name__ == "__main__":
    main()
