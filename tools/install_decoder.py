#!/usr/bin/env python3
"""install_decoder.py — install a product's ChirpStack JS payload codec onto a device-profile.

P5.2 Plane-A: the senseflow (I2C) node's uplinks need the senseflow decoder installed on a
senseflow-specific ChirpStack device-profile — NOT on the shared/careflow device-profile (whose Modbus
codec must not be clobbered, per senseflow ad69c22). This tool find-or-creates a device-profile named
for the product, installs the JS codec, and (optionally) repoints a device at it. Idempotent.

Reuses the gRPC-web client from the lorawan-provision-device skill (no REST API on the host). Targets
whatever CS_BASE points at — set it to the DEV ChirpStack (http://10.10.8.140:8080) for the dev-only
Plane-A; the PRODUCTION stack is Fahim's and must not be written from here.

Env (never hardcode — see the skill's SKILL.md):
  CS_BASE          e.g. http://10.10.8.140:8080   (DEV) — REQUIRED
  UA               browser UA (prod Cloudflare needs it; harmless on dev)
  CS_API_TOKEN     ChirpStack API key (preferred)   — or CS_ADMIN_USER/CS_ADMIN_PASS
  CS_TENANT_ID     tenant UUID the device-profile lives under — REQUIRED (or --tenant-id)

Usage:
  # dry-run (no writes) — shows what would be created/updated
  CS_BASE=http://10.10.8.140:8080 CS_API_TOKEN=… CS_TENANT_ID=… \
    python3 tools/install_decoder.py --product senseflow --dry-run

  # apply, and repoint the joined senseflow device at the new profile
  CS_BASE=http://10.10.8.140:8080 CS_API_TOKEN=… CS_TENANT_ID=… \
    python3 tools/install_decoder.py --product senseflow --repoint-deveui 3cdc75fffe6f80e4
"""
import argparse
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
# Reuse the skill's gRPC-web client + chirpstack-api (installed in the skill venv).
sys.path.insert(0, os.path.join(_HERE, "..", ".claude", "skills", "_shared"))

try:
    import cs_grpcweb as cs
    from chirpstack_api import api, common
except (ImportError, KeyError) as e:
    sys.exit(f"FAIL: {e}. Set CS_BASE/UA and run inside the skill venv "
             "(.claude/skills/.v) so chirpstack-api + cs_grpcweb import.")

# Per-product device-profile name + the decoder artifact that carries its JS codec.
PRODUCTS = {
    "senseflow": {
        "dp_name": "senseflow-eink-AS923",
        "decoder": os.path.join(_HERE, "..", "..", "rak4630-e-ink-claude",
                                "device-profiles", "chirpstack_senseflow_decoder.js"),
    },
    "careflow": {
        "dp_name": "careflow-rs485-AS923",
        "decoder": os.path.join(_HERE, "chirpstack_mfm384_decoder.js"),
    },
}


def die(msg, code=2):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(code)


def main():
    ap = argparse.ArgumentParser(description="Install a product's JS codec onto a ChirpStack device-profile.")
    ap.add_argument("--product", default="senseflow", choices=sorted(PRODUCTS))
    ap.add_argument("--decoder", help="override the decoder .js path")
    ap.add_argument("--dp-name", help="override the device-profile name")
    ap.add_argument("--tenant-id", default=os.environ.get("CS_TENANT_ID", ""),
                    help="tenant UUID (or CS_TENANT_ID env)")
    ap.add_argument("--region", default="AS923", help="LoRaWAN region (default AS923)")
    ap.add_argument("--repoint-deveui", help="also point this device (16-hex DevEUI) at the profile")
    ap.add_argument("--dry-run", action="store_true", help="show actions without writing")
    args = ap.parse_args()

    P = PRODUCTS[args.product]
    dp_name = args.dp_name or P["dp_name"]
    decoder_path = args.decoder or P["decoder"]
    tenant_id = args.tenant_id
    if not tenant_id:
        die("tenant id required — set CS_TENANT_ID or pass --tenant-id (the tenant the gateway/device "
            "live under; senseflow dev tenant = siot-dev-stack).")
    if not os.path.exists(decoder_path):
        die(f"decoder not found: {decoder_path} (pass --decoder). For senseflow it lives in the "
            "senseflow repo device-profiles/chirpstack_senseflow_decoder.js.")
    with open(decoder_path) as fh:
        codec = fh.read()

    region = getattr(common.Region, args.region, None)
    if region is None:
        die(f"unknown region '{args.region}'")

    print(f"[*] product={args.product} dp='{dp_name}' region={args.region} "
          f"codec={os.path.basename(decoder_path)} ({len(codec)} bytes) base={os.environ.get('CS_BASE')}")

    jwt = cs.auth()

    # find-or-create the device-profile by name within the tenant
    lst = cs.call("/api.DeviceProfileService/List",
                  api.ListDeviceProfilesRequest(tenant_id=tenant_id, limit=200),
                  api.ListDeviceProfilesResponse, jwt)
    existing = next((r for r in lst.result if r.name == dp_name), None)

    dp = api.DeviceProfile(
        tenant_id=tenant_id,
        name=dp_name,
        region=region,
        mac_version=common.MacVersion.LORAWAN_1_0_3,
        reg_params_revision=common.RegParamsRevision.RP002_1_0_3,
        adr_algorithm_id="default",
        supports_otaa=True,
        payload_codec_runtime=api.CodecRuntime.JS,
        payload_codec_script=codec,
    )

    if args.dry_run:
        action = "UPDATE" if existing else "CREATE"
        print(f"[dry-run] would {action} device-profile '{dp_name}'"
              + (f" (id={existing.id})" if existing else "")
              + " with JS codec")
        if args.repoint_deveui:
            print(f"[dry-run] would repoint device {args.repoint_deveui} -> '{dp_name}'")
        return

    if existing:
        dp.id = existing.id
        from google.protobuf import empty_pb2
        cs.call("/api.DeviceProfileService/Update",
                api.UpdateDeviceProfileRequest(device_profile=dp), empty_pb2.Empty, jwt)
        dp_id = existing.id
        print(f"[2] device-profile UPDATED '{dp_name}' id={dp_id} (JS codec installed)")
    else:
        created = cs.call("/api.DeviceProfileService/Create",
                          api.CreateDeviceProfileRequest(device_profile=dp),
                          api.CreateDeviceProfileResponse, jwt)
        dp_id = created.id
        print(f"[2] device-profile CREATED '{dp_name}' id={dp_id} (JS codec installed)")

    # verify the codec landed
    got = cs.call("/api.DeviceProfileService/Get", api.GetDeviceProfileRequest(id=dp_id),
                  api.GetDeviceProfileResponse, jwt)
    ok = got.device_profile.payload_codec_runtime == api.CodecRuntime.JS and \
        len(got.device_profile.payload_codec_script) == len(codec)
    print(f"[3] verify: runtime=JS={got.device_profile.payload_codec_runtime == api.CodecRuntime.JS} "
          f"codec_len={len(got.device_profile.payload_codec_script)} -> {'OK' if ok else 'MISMATCH'}")
    if not ok:
        die("codec did not persist as expected")

    if args.repoint_deveui:
        deveui = args.repoint_deveui.lower()
        from google.protobuf import empty_pb2
        gd = cs.call("/api.DeviceService/Get", api.GetDeviceRequest(dev_eui=deveui),
                     api.GetDeviceResponse, jwt)
        dev = gd.device
        if dev.device_profile_id == dp_id:
            print(f"[4] device {deveui} already on '{dp_name}' — no change")
        else:
            prev = dev.device_profile_id
            dev.device_profile_id = dp_id
            cs.call("/api.DeviceService/Update", api.UpdateDeviceRequest(device=dev),
                    empty_pb2.Empty, jwt)
            print(f"[4] device {deveui} repointed {prev} -> {dp_id} ('{dp_name}')")

    print("PASS: codec installed" + (" + device repointed" if args.repoint_deveui else "")
          + ". Send/await an uplink and confirm the decode in the ChirpStack device event log.")


if __name__ == "__main__":
    main()
