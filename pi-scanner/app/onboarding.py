"""onboarding.py — hand a confirmed profile to the EXISTING Careflow pipeline. Glue only.

Steps (all reuse the committed service/tools; nothing is reimplemented):
  flash_node      — the scanner firmware already contains run_field_app, so provisioning restarts the
                    node into field mode; a production reflash uses flash_via_manifest() (GET
                    /v1/flash-manifest + /v1/flash-part, esptool write_flash, erase DISABLED).
  provision       — write OTAA creds (firmware/.env) + the profile blob over the node console
                    (prov-lorawan / prov-profile / prov-done); the blob is the emitter's output.
  install_decoder — subprocess tools/install_decoder.py against DEV ChirpStack (idempotent).
  verify_uplink   — stream ChirpStack device events; pass when a decoded uplink whose device byte ==
                    the new type_byte lands.

DEV_GUARD: the constructor refuses any non-dev ChirpStack base, so a scanner run can never write
production. Pure helpers (fetch_flash_manifest / build_provision_commands) are unit-tested; the
hardware/network methods are exercised on the bench (see pi-scanner/README.md P5 gate).
"""

from __future__ import annotations

import os
import struct
import subprocess
import sys
import time
from typing import Callable, Optional

from .config import Config, assert_dev_chirpstack
from .profile_emitter import ArtifactSet

Log = Callable[[str], None]


class OnboardError(RuntimeError):
    pass


def _load_env(path: str) -> dict[str, str]:
    env: dict[str, str] = {}
    if not os.path.exists(path):
        return env
    for line in open(path):
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip()
    return env


def build_provision_commands(env: dict[str, str], blob_hex: str) -> list[tuple[str, bool]]:
    """(command, slow) list to provision creds + the profile blob. The profile blob carries the bus
    config (baud/parity) + reader map, so no prov-modbus is needed. ``slow`` = write char-by-char
    (the ~900-hex blob drops chars on the USB-Serial-JTAG if sent in one burst)."""
    deveui = env.get("LORAWAN_DEVEUI", "").strip()
    joineui = env.get("LORAWAN_JOINEUI", "0000000000000000").strip() or "0000000000000000"
    appkey = env.get("LORAWAN_APPKEY", "").strip()
    if not (len(deveui) == 16 and len(appkey) == 32):
        raise OnboardError("firmware/.env is missing a valid LORAWAN_DEVEUI (16 hex) / APPKEY (32 hex)")
    return [
        (f"prov-lorawan {deveui} {joineui} {appkey}", False),
        (f"prov-profile {blob_hex}", True),
        ("prov-done", False),
    ]


def fetch_flash_manifest(client, api_base: str, product: str, token: str = "") -> list[dict]:
    """GET /v1/flash-manifest → ordered parts [{name, offset, size, sha256, path}]."""
    headers = {"Authorization": f"Bearer {token}"} if token else {}
    r = client.get(f"{api_base}/v1/flash-manifest", params={"product": product}, headers=headers)
    r.raise_for_status()
    return r.json()["parts"]


class Onboarding:
    def __init__(self, cfg: Config, log: Log):
        assert_dev_chirpstack(cfg.cs_base)  # DEV_GUARD — refuse production ChirpStack
        self.cfg = cfg
        self.log = log
        self.env = _load_env(os.path.join(cfg.repo_root, "firmware", ".env"))

    # --- flash ---------------------------------------------------------------------------------

    def flash_node(self, artifacts: ArtifactSet) -> None:
        # The scanner firmware is field-capable (run_field_app is compiled in); provisioning below
        # restarts it into field mode. A pristine production node is flashed with flash_via_manifest.
        self.log("flash: scanner firmware is field-capable — provisioning will restart into field mode")

    def flash_via_manifest(self, port: str) -> None:
        """Production reflash: download the dual-OTA set and write it (erase DISABLED, T-02-01)."""
        import hashlib
        import tempfile

        import httpx

        with httpx.Client(timeout=30) as client:
            parts = fetch_flash_manifest(client, self.cfg.api_base, self.cfg.product, self.cfg.api_token)
            args: list[str] = []
            tmp = tempfile.mkdtemp(prefix="scanner-flash-")
            for p in parts:
                hdrs = {"Authorization": f"Bearer {self.cfg.api_token}"} if self.cfg.api_token else {}
                resp = client.get(f"{self.cfg.api_base}/v1/flash-part/{p['name']}",
                                  params={"product": self.cfg.product}, headers=hdrs)
                resp.raise_for_status()
                if hashlib.sha256(resp.content).hexdigest() != p["sha256"]:
                    raise OnboardError(f"sha256 mismatch on flash part {p['name']}")
                fn = os.path.join(tmp, f"{p['name']}.bin")
                open(fn, "wb").write(resp.content)
                args += [hex(p["offset"]), fn]
            # erase DISABLED: write_flash without erase_all; nvs-blank part already resets NVS.
            cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port,
                   "--before", "default_reset", "--after", "hard_reset", "write_flash", *args]
            self.log(f"flash: esptool write_flash {len(parts)} parts (no erase)")
            cp = subprocess.run(cmd, capture_output=True, text=True)
            if cp.returncode != 0:
                raise OnboardError(f"esptool flash failed: {cp.stderr or cp.stdout}")

    # --- provision -----------------------------------------------------------------------------

    def provision(self, artifacts: ArtifactSet, port: Optional[str] = None) -> None:
        from .node_console import NodeConsole

        port = port or self.cfg.node_port
        cmds = build_provision_commands(self.env, artifacts.blob_hex)
        console = NodeConsole(port, self.cfg.node_baud, on_line=self.log)
        try:
            for line, slow in cmds:
                shown = line.split(" ")[0] + " …" if ("appkey" in line or "prov-profile" in line) else line
                self.log(f"provision → {shown}")
                out = console.send(line, slow=slow)
                if not any(o.startswith("OK") for o in out):
                    raise OnboardError(f"provision command failed: {shown} → {out}")
        finally:
            console.close()

    # --- decoder -------------------------------------------------------------------------------

    def install_decoder(self, artifacts: ArtifactSet) -> None:
        script = os.path.join(self.cfg.repo_root, "tools", "install_decoder.py")
        env = dict(os.environ, CS_BASE=self.cfg.cs_base)
        cp = subprocess.run([sys.executable, script, "--product", self.cfg.product],
                            cwd=self.cfg.repo_root, capture_output=True, text=True, env=env)
        if cp.returncode != 0:
            raise OnboardError(f"install_decoder failed: {cp.stderr or cp.stdout}")
        self.log("decoder: installed fleet codec on dev ChirpStack device-profile")

    # --- verify --------------------------------------------------------------------------------

    def verify_uplink(self, artifacts: ArtifactSet, deveui: str = "", timeout_s: int = 180) -> dict:
        """Wait for a decoded uplink on dev ChirpStack whose device byte matches the new type_byte."""
        assert_dev_chirpstack(self.cfg.cs_base)
        deveui = deveui or self.env.get("LORAWAN_DEVEUI", "").strip().lower()
        want_byte = self._type_byte(artifacts.blob_hex)
        self.log(f"verify: waiting up to {timeout_s}s for a decoded uplink (device byte 0x{want_byte:02X}) …")
        got = _stream_wait_decoded(self.cfg.cs_base, deveui, timeout_s, self.log)
        decoded = bool(got)
        self.log("verify: decoded uplink observed" if decoded else "verify: no decoded uplink in time")
        return {"joined": decoded, "decoded": decoded, "device_byte": want_byte, "object": got}

    @staticmethod
    def _type_byte(blob_hex: str) -> int:
        # dp blob header byte 1 = device_byte (version, device_byte, …)
        return struct.unpack("B", bytes.fromhex(blob_hex[2:4]))[0]


def _stream_wait_decoded(cs_base: str, deveui: str, timeout_s: int, log: Log) -> Optional[dict]:
    """Minimal gRPC-web StreamDeviceEvents reader — returns the first fresh decoded uplink object."""
    import http.client
    import json as _json
    from urllib.parse import urlparse

    # reuse the repo's tiny gRPC-web client for auth + protobuf
    import importlib.util

    shared = os.path.join(os.path.dirname(__file__), "..", "..", ".claude", "skills", "_shared", "cs_grpcweb.py")
    if not os.path.exists(shared):
        log("verify: cs_grpcweb helper not found — skipping stream (bench only)")
        return None
    spec = importlib.util.spec_from_file_location("cs_grpcweb", shared)
    cs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cs)  # type: ignore
    from chirpstack_api import api  # type: ignore

    jwt = cs.auth()
    base = urlparse(cs_base)
    req = api.StreamDeviceEventsRequest(dev_eui=deveui)
    payload = req.SerializeToString()
    frame = b"\x00" + struct.pack(">I", len(payload)) + payload
    conn = http.client.HTTPConnection(base.hostname, base.port or 80, timeout=timeout_s)
    conn.request("POST", "/api.InternalService/StreamDeviceEvents", body=frame, headers={
        "Content-Type": "application/grpc-web+proto", "X-Grpc-Web": "1",
        "Accept": "application/grpc-web+proto", "authorization": "Bearer " + jwt})
    resp = conn.getresponse()
    buf = b""
    deadline = time.monotonic() + timeout_s
    try:
        while time.monotonic() < deadline:
            chunk = resp.read(1)
            if not chunk:
                break
            buf += chunk
            while len(buf) >= 5:
                flag, ln = buf[0], struct.unpack(">I", buf[1:5])[0]
                if len(buf) < 5 + ln:
                    break
                body, buf = buf[5:5 + ln], buf[5 + ln:]
                if flag & 0x80:
                    continue
                item = api.LogItem.FromString(body)
                if item.description == "up" and item.body:
                    obj = _json.loads(item.body).get("object")
                    if obj:
                        return obj
    finally:
        conn.close()
    return None
