"""
Builds router: POST /v1/build, GET /v1/builds/{tag}, GET /v1/provisioning-protocol.

All endpoints require a valid Bearer token (T-01-01: verify_bearer dependency on
the APIRouter). Only the pinned FIRMWARE_TAG is accepted for builds (D-06).

MinIO clients and firmware_path are read from request.app.state, which is
populated at lifespan startup (Plan 01-05). Tests inject mocks by setting
app.state before calling the TestClient.

Source: .planning/phases/01-firmware-service-in-cluster-b/01-03-PLAN.md Task 2
"""
import logging
from pathlib import Path
from typing import Optional

from fastapi import APIRouter, Depends, HTTPException, Request

from api.auth import verify_bearer
from api.config import Settings, get_settings
from api.models import Build, BuildRequest
from api.services.storage import ensure_built, get_cached_build

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/v1", dependencies=[Depends(verify_bearer)])


# ---------------------------------------------------------------------------
# POST /v1/build — build-once + cache (idempotent per firmware tag)
# ---------------------------------------------------------------------------


@router.post("/build", response_model=Build, status_code=200)
async def trigger_build(
    request: Request,
    body: Optional[BuildRequest] = None,
    settings: Settings = Depends(get_settings),
) -> Build:
    """
    POST /v1/build — idempotent build trigger.

    Accepts an optional body ``{"firmwareTag": "<tag>"}``. If omitted, or if
    ``firmwareTag`` is null/empty, defaults to the pinned ``FIRMWARE_TAG`` from
    settings. Only the pinned tag is accepted (D-06); any other tag returns 422
    with ``error.code = unknown_tag``.

    On first call: uploads the baked firmware binary to MinIO, computes sha256,
    generates a 1-hour presigned binaryUrl.
    On subsequent calls: returns the cached result without re-uploading.
    """
    tag: str = (
        (body.firmwareTag if body and body.firmwareTag else None) or settings.FIRMWARE_TAG
    )

    if tag != settings.FIRMWARE_TAG:
        raise HTTPException(
            status_code=422,
            detail={
                "error": {
                    "code": "unknown_tag",
                    "message": (
                        f"Unknown firmware tag '{tag}'. "
                        f"Only the pinned tag '{settings.FIRMWARE_TAG}' is supported."
                    ),
                }
            },
        )

    state = request.app.state
    return await ensure_built(
        tag,
        internal=state.minio_internal,
        external=state.minio_external,
        bucket=getattr(state, "firmware_bucket", settings.MINIO_BUCKET),
        firmware_path=state.firmware_path,
        expiry_hours=settings.PRESIGNED_EXPIRY_HOURS,
    )


# ---------------------------------------------------------------------------
# GET /v1/builds/{firmware_tag} — fetch cached build status
# ---------------------------------------------------------------------------


@router.get("/builds/{firmware_tag}", response_model=Build)
async def get_build(
    firmware_tag: str,
    request: Request,
    settings: Settings = Depends(get_settings),
) -> Build:
    """
    GET /v1/builds/{firmware_tag} — return the cached build or 404.

    Returns 404 with ``error.code = not_found`` when the tag has never been
    built. Returns the ready Build (binaryUrl + binarySha256) if cached.

    Does NOT trigger an upload — this is a read-only cache check. Use
    POST /v1/build to trigger the upload.
    """
    state = request.app.state

    # Read minio clients from app state. If not yet initialised (e.g. test
    # environment without lifespan), treat as empty cache — return 404.
    minio_internal = getattr(state, "minio_internal", None)
    minio_external = getattr(state, "minio_external", None)

    if minio_internal is None or minio_external is None:
        logger.warning(
            "GET /v1/builds/%s: MinIO clients not initialised in app.state — "
            "returning not_found (empty cache)",
            firmware_tag,
        )
        raise HTTPException(
            status_code=404,
            detail={
                "error": {
                    "code": "not_found",
                    "message": f"Tag '{firmware_tag}' has never been built.",
                }
            },
        )

    bucket = getattr(state, "firmware_bucket", settings.MINIO_BUCKET)
    firmware_path: Path = state.firmware_path

    result = await get_cached_build(
        firmware_tag,
        internal=minio_internal,
        external=minio_external,
        bucket=bucket,
        firmware_path=firmware_path,
        expiry_hours=settings.PRESIGNED_EXPIRY_HOURS,
    )

    if result is None:
        raise HTTPException(
            status_code=404,
            detail={
                "error": {
                    "code": "not_found",
                    "message": f"Tag '{firmware_tag}' has never been built.",
                }
            },
        )

    return result


# ---------------------------------------------------------------------------
# GET /v1/provisioning-protocol — advisory NVS command set
# ---------------------------------------------------------------------------


@router.get("/provisioning-protocol")
async def get_provisioning_protocol(
    settings: Settings = Depends(get_settings),
) -> dict:
    """
    GET /v1/provisioning-protocol — advisory console command set.

    Returns the NVS provisioning workflow for the pinned firmware tag. This
    endpoint is optional and advisory — the Phase A WebSerial flasher uses it
    to populate its provisioning UI.

    ``appKey`` is flagged ``secret: true`` and is NEVER populated with a live
    value (T-01-09: no credential disclosure; D-07: placeholder creds only).

    Console baud is 115200; promptReady is the string the firmware prints when
    it is ready to accept prov-* commands (bootVerify confirms boot state).
    """
    return {
        "firmwareTag": settings.FIRMWARE_TAG,
        "console": {
            "baud": 115200,
            "promptReady": "esp> ",
        },
        "commands": [
            {
                "id": "prov-modbus",
                "description": "Set Modbus RTU connection parameters in NVS",
                "syntax": "prov modbus <baud> <parity> <stopBits> <slaveId>",
                "example": "prov modbus 9600 N 1 1",
                "nvsKeys": [
                    "modbus_baud",
                    "modbus_parity",
                    "modbus_stop",
                    "modbus_slave",
                ],
            },
            {
                "id": "prov-creds",
                "description": "Inject LoRaWAN OTAA credentials into NVS",
                "syntax": "prov creds <devEui> <joinEui> <appKey>",
                "args": {
                    "devEui": {
                        "description": "16-hex DevEUI (unique per device)",
                        "secret": False,
                    },
                    "joinEui": {
                        "description": "16-hex JoinEUI / AppEUI",
                        "secret": False,
                    },
                    "appKey": {
                        "description": "32-hex AppKey (LoRaWAN OTAA root key)",
                        "secret": True,
                    },
                },
                "nvsKeys": [
                    "lorawan_deveui",
                    "lorawan_joineui",
                    "lorawan_appkey",
                ],
            },
            {
                "id": "prov-show",
                "description": (
                    "Print current NVS provisioning state "
                    "(appKey redacted, other values shown)"
                ),
                "syntax": "prov show",
                "nvsKeys": [
                    "modbus_baud",
                    "modbus_parity",
                    "modbus_stop",
                    "modbus_slave",
                    "lorawan_deveui",
                    "lorawan_joineui",
                ],
            },
        ],
        "bootVerify": {
            "markers": [
                "[creds:NVS]",
                "PSRAM",
                "modbus",
            ],
            "description": (
                "Confirm these strings appear in the boot log "
                "before declaring provisioning complete."
            ),
        },
    }
