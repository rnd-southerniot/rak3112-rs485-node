"""P5 unit coverage: the testable onboarding logic (provision-command build, manifest fetch, DEV_GUARD,

type-byte). The live flash→provision→decoder→verify path is the bench gate (pi-scanner/README.md).
"""

from __future__ import annotations

import dataclasses
import os

import pytest

from app.config import CONFIG, assert_dev_chirpstack
from app.onboarding import (
    Onboarding,
    OnboardError,
    build_provision_commands,
    fetch_flash_manifest,
)

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def test_build_provision_commands_order_and_slow():
    env = {"LORAWAN_DEVEUI": "3cdc75fffe6f85dc", "LORAWAN_JOINEUI": "0" * 16,
           "LORAWAN_APPKEY": "0" * 32}
    cmds = build_provision_commands(env, "0105abcd")
    assert [c[0].split()[0] for c in cmds] == ["prov-lorawan", "prov-profile", "prov-done"]
    # the blob command must be sent char-by-char (slow=True)
    assert cmds[1] == ("prov-profile 0105abcd", True)
    assert cmds[0][1] is False and cmds[2][1] is False


def test_build_provision_commands_rejects_bad_creds():
    with pytest.raises(OnboardError):
        build_provision_commands({"LORAWAN_DEVEUI": "short", "LORAWAN_APPKEY": ""}, "0105")


def test_dev_guard_blocks_production():
    assert_dev_chirpstack("http://10.10.8.140:8080")  # dev — allowed
    for prod in ("https://chirpstack.siot.solutions", "http://10.10.8.168:8080"):
        with pytest.raises(RuntimeError):
            assert_dev_chirpstack(prod)


def test_onboarding_ctor_refuses_production():
    prod_cfg = dataclasses.replace(CONFIG, cs_base="https://chirpstack.siot.solutions", repo_root=REPO)
    with pytest.raises(RuntimeError):
        Onboarding(prod_cfg, lambda _m: None)


def test_fetch_flash_manifest_parses_parts():
    class _Resp:
        def raise_for_status(self):
            pass

        def json(self):
            return {"product": "careflow", "parts": [
                {"name": "bootloader", "offset": 0, "size": 20, "sha256": "aa", "path": "/x"},
                {"name": "app", "offset": 0x20000, "size": 400, "sha256": "bb", "path": "/y"},
            ]}

    class _Client:
        def get(self, url, params=None, headers=None):
            assert "flash-manifest" in url and params["product"] == "careflow"
            return _Resp()

    parts = fetch_flash_manifest(_Client(), "http://svc", "careflow")
    assert [p["name"] for p in parts] == ["bootloader", "app"]
    assert parts[1]["offset"] == 0x20000


def test_type_byte_from_blob():
    # dp blob header: version(01) device_byte(05) …
    assert Onboarding._type_byte("0105deadbeef") == 5
