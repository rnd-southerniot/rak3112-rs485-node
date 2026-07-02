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
  0xf000   ota-data (points the bootloader at ota_0)
  0x20000  app (ota_0)
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

# Flash layout for the pinned dual-OTA firmware. `file` is resolved relative to
# FIRMWARE_PATH.parent (the baked firmware dir). Order = ascending flash offset.
FLASH_PARTS: list[dict] = [
    {"name": "bootloader", "offset": 0x0, "file": "bootloader.bin"},
    {"name": "partition-table", "offset": 0x8000, "file": "partition-table.bin"},
    {"name": "ota-data", "offset": 0xF000, "file": "ota_data.bin"},
    {"name": "app", "offset": 0x20000, "file": None},  # None → FIRMWARE_PATH itself
]


def _part_path(part: dict, firmware_path: Path) -> Path:
    """Resolve a part's on-disk path. The 'app' part is FIRMWARE_PATH; the boot
    artifacts sit alongside it in the same baked firmware directory."""
    if part["file"] is None:
        return firmware_path
    return firmware_path.parent / part["file"]


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


@router.get("/flash-manifest")
async def get_flash_manifest(
    request: Request,
    settings: Settings = Depends(get_settings),
) -> dict:
    """
    Return the ordered flash set for the pinned firmware: each part's name, flash
    offset, byte size, and sha256. The flasher fetches each part via
    GET /v1/flash-part/{name} and writes it at `offset` (eraseAll:false so the
    nvs region is preserved — creds + LoRaWAN nonces survive per firmware ADR-007).
    """
    firmware_path: Path = request.app.state.firmware_path
    parts = []
    for part in FLASH_PARTS:
        p = _part_path(part, firmware_path)
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
        parts.append(
            {
                "name": part["name"],
                "offset": part["offset"],
                "size": p.stat().st_size,
                "sha256": _sha256(p),
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
    p = _part_path(part, firmware_path)
    if not p.exists():
        raise HTTPException(
            status_code=500,
            detail={
                "error": {
                    "code": "missing_flash_part",
                    "message": f"Flash part '{name}' not baked into the image ({p}).",
                }
            },
        )
    data = p.read_bytes()
    return Response(
        content=data,
        media_type="application/octet-stream",
        headers={
            "X-Binary-Sha256": hashlib.sha256(data).hexdigest(),
            "X-Flash-Offset": hex(part["offset"]),
        },
    )
