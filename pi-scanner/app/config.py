"""Station configuration — env-driven, with bench-safe defaults.

All firmware-service calls go through ``API_BASE`` on the frozen ``/v1`` contract so retargeting the
hub (``siot-node-firmware-automation``) is a base-URL swap. ChirpStack is pinned to the dev stack and
guarded: ``DEV_GUARD`` refuses any non-dev base so a scanner run can never touch production.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field

# Dev ChirpStack (10.10.8.140) is the ONLY stack this station may touch. Production hostnames
# (crm/chirpstack.siot.solutions, 10.10.8.168) are refused by _assert_dev_chirpstack().
_DEV_CS_HOSTS = ("10.10.8.140", "localhost", "127.0.0.1")


def _env(name: str, default: str) -> str:
    return os.environ.get(name, default)


@dataclass(frozen=True)
class Config:
    # Careflow node console (USB-Serial-JTAG). Auto-detected at runtime if left as "auto".
    node_port: str = field(default_factory=lambda: _env("SCANNER_NODE_PORT", "auto"))
    node_baud: int = 115200

    # Firmware-build service (this repo's api/, hub-compatible). Bearer token if the service needs it.
    api_base: str = field(default_factory=lambda: _env("SCANNER_API_BASE", "http://127.0.0.1:8000"))
    api_token: str = field(default_factory=lambda: _env("SCANNER_API_TOKEN", ""))
    product: str = "careflow"

    # Dev ChirpStack (decoder install + join/uplink verify). Guarded to dev-only.
    cs_base: str = field(default_factory=lambda: _env("CS_BASE", "http://10.10.8.140:8080"))

    # Repo root (device-profiles/ generators, tools/). Defaults to two levels up from this file.
    repo_root: str = field(
        default_factory=lambda: _env(
            "SCANNER_REPO_ROOT",
            os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")),
        )
    )

    # HTTP server for the kiosk UI.
    host: str = "127.0.0.1"
    port: int = 8080

    @property
    def profiles_dir(self) -> str:
        return os.path.join(self.repo_root, "device-profiles", "profiles")

    @property
    def device_profiles_dir(self) -> str:
        return os.path.join(self.repo_root, "device-profiles")

    @property
    def tools_dir(self) -> str:
        return os.path.join(self.repo_root, "tools")


def assert_dev_chirpstack(cs_base: str) -> None:
    """Refuse any non-dev ChirpStack base. Guards every decoder-install / verify call (two-stack rule)."""
    host = cs_base.split("://", 1)[-1].split("/", 1)[0].split(":", 1)[0]
    if host not in _DEV_CS_HOSTS:
        raise RuntimeError(
            f"DEV_GUARD: CS_BASE host {host!r} is not the dev ChirpStack ({_DEV_CS_HOSTS}); "
            "the scanner station is dev-only — production must never be written."
        )


CONFIG = Config()
