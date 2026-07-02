"""
Flash router: GET /v1/flash-manifest, GET /v1/flash-part/{name}.

Serves the FULL esptool flash set for the pinned firmware — bootloader,
partition-table, ota-data, and app — at their real flash offsets (from the
build's flasher_args.json). The Phase-7 firmware uses a dual-slot OTA partition
layout, so a correct install writes all four regions; an app-only write at
0x10000 (the legacy single-app offset) does NOT install this firmware and the
board keeps booting the previous image.

All parts are baked into the image at FIRMWARE_PATH.parent and served from disk
(they are small + static per partition scheme). Bearer-protected like the rest
of /v1. The CRM backend proxies these same-origin to the WebSerial flasher.

Offsets are the ESP-IDF standard for this 16MB dual-OTA scheme:
  0x0      bootloader
  0x8000   partition-table
  0x9000   nvs-blank (factory reset: all-0xFF = erased NVS; see below)
  0xf000   ota-data (points the bootloader at ota_0)
  0x20000  app (ota_0)

The nvs-blank part makes the factory flash a FACTORY RESET: it wipes any stale
NVS state (old creds, device profile, and the LoRaWAN session/nonces). A board
that keeps a stale session skips OTAA re-join and uplinks on keys ChirpStack no
longer knows — frames silently dropped, never self-heals. Fresh nonces are safe
at factory: each flash pairs with a fresh mint + fresh ChirpStack registration
at QR scan, so the server has no DevNonce history. (ADR-007's preserve-nvs rule
protects FIELD OTA updates, a different lifecycle stage.)
"""
import hashlib
import logging
from pathlib import Path

from fastapi import APIRouter, Depends, HTTPException, Request
from fastapi.responses import Response

from api.auth import verify_bearer
from api.config import Settings, get_settings

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/v1", dependencies=[Depends(verify_bearer)])

# The nvs partition per firmware/partitions.csv: offset 0x9000, length 0x6000.
NVS_OFFSET = 0x9000
NVS_SIZE = 0x6000
_NVS_BLANK = b"\xff" * NVS_SIZE  # all-0xFF == erased flash; ESP-IDF NVS formats it on boot

# Flash layout for the pinned dual-OTA firmware. `file` is resolved relative to
# FIRMWARE_PATH.parent (the baked firmware dir); "blank" parts are generated
# in-memory. Order = ascending flash offset.
FLASH_PARTS: list[dict] = [
    {"name": "bootloader", "offset": 0x0, "file": "bootloader.bin"},
    {"name": "partition-table", "offset": 0x8000, "file": "partition-table.bin"},
    {"name": "nvs-blank", "offset": NVS_OFFSET, "file": "", "blank": NVS_SIZE},
    {"name": "ota-data", "offset": 0xF000, "file": "ota_data.bin"},
    {"name": "app", "offset": 0x20000, "file": None},  # None → FIRMWARE_PATH itself
]


def _part_bytes(part: dict, firmware_path: Path) -> bytes:
    """Resolve a part's bytes. 'blank' parts are generated (all-0xFF); the 'app'
    part is FIRMWARE_PATH; boot artifacts sit alongside it in the baked dir."""
    if part.get("blank"):
        return _NVS_BLANK
    p = firmware_path if part["file"] is None else firmware_path.parent / part["file"]
    if not p.exists():
        raise HTTPException(
            status_code=500,
            detail={
                "error": {
                    "code": "missing_flash_part",
                    "message": f"Flash part '{part['name']}' not baked into the image ({p}).",
                }
            },
        )
    return p.read_bytes()


@router.get("/flash-manifest")
async def get_flash_manifest(
    request: Request,
    settings: Settings = Depends(get_settings),
) -> dict:
    """
    Return the ordered flash set for the pinned firmware: each part's name, flash
    offset, byte size, and sha256. The flasher fetches each part via
    GET /v1/flash-part/{name} and writes it at `offset`. The set includes
    nvs-blank, so a factory flash is a factory reset (see module docstring).
    """
    firmware_path: Path = request.app.state.firmware_path
    parts = []
    for part in FLASH_PARTS:
        data = _part_bytes(part, firmware_path)
        parts.append(
            {
                "name": part["name"],
                "offset": part["offset"],
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )
    return {"firmwareTag": settings.FIRMWARE_TAG, "parts": parts}


@router.get("/flash-part/{name}")
async def get_flash_part(
    name: str,
    request: Request,
) -> Response:
    """Stream one flash part's raw bytes (application/octet-stream) with an
    X-Binary-Sha256 header for integrity verification before writing."""
    firmware_path: Path = request.app.state.firmware_path
    part = next((p for p in FLASH_PARTS if p["name"] == name), None)
    if part is None:
        raise HTTPException(
            status_code=404,
            detail={"error": {"code": "not_found", "message": f"Unknown flash part '{name}'."}},
        )
    data = _part_bytes(part, firmware_path)
    return Response(
        content=data,
        media_type="application/octet-stream",
        headers={
            "X-Binary-Sha256": hashlib.sha256(data).hexdigest(),
            "X-Flash-Offset": hex(part["offset"]),
        },
    )
