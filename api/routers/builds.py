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

from fastapi import APIRouter, Depends, HTTPException, Query, Request

from api.auth import verify_bearer
from api.config import Settings, get_settings
from api.models import Build, BuildRequest
from api.products import firmware_path_for, resolve_product
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
    product: Optional[str] = Query(default=None),
    settings: Settings = Depends(get_settings),
) -> Build:
    """
    POST /v1/build — idempotent build trigger. `?product=` defaults to careflow (v1 calls unchanged).

    Accepts an optional body ``{"firmwareTag": "<tag>"}``; if omitted/empty, defaults to the product's
    pinned tag. Only the product's pinned tag is accepted (D-06); any other tag → 422
    ``unknown_tag``. First call uploads the product's binary to MinIO (product-scoped key) + presigned
    URL; later calls return the cached result. (`product` is a query param, not a `BuildRequest` field —
    `BuildRequest` is frozen.)
    """
    prod = resolve_product(request, product)
    tag: str = (body.firmwareTag if body and body.firmwareTag else None) or prod.firmware_tag

    if tag != prod.firmware_tag:
        raise HTTPException(
            status_code=422,
            detail={
                "error": {
                    "code": "unknown_tag",
                    "message": (
                        f"Unknown firmware tag '{tag}'. "
                        f"Only the pinned tag '{prod.firmware_tag}' is supported for product "
                        f"'{prod.id}'."
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
        firmware_path=firmware_path_for(request, prod),
        expiry_hours=settings.PRESIGNED_EXPIRY_HOURS,
    )


# ---------------------------------------------------------------------------
# GET /v1/builds/{firmware_tag} — fetch cached build status
# ---------------------------------------------------------------------------


@router.get("/builds/{firmware_tag}", response_model=Build)
async def get_build(
    firmware_tag: str,
    request: Request,
    product: Optional[str] = Query(default=None),
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
    firmware_path: Path = firmware_path_for(request, resolve_product(request, product))

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


_CMD_MODBUS = {
    "id": "prov-modbus",
    "description": "Set Modbus RTU connection parameters in NVS",
    "syntax": "prov modbus <baud> <parity> <stopBits> <slaveId>",
    "example": "prov modbus 9600 N 1 1",
    "nvsKeys": ["modbus_baud", "modbus_parity", "modbus_stop", "modbus_slave"],
}
_CMD_CREDS = {
    "id": "prov-creds",
    "description": "Inject LoRaWAN OTAA credentials into NVS",
    "syntax": "prov creds <devEui> <joinEui> <appKey>",
    "args": {
        "devEui": {"description": "16-hex DevEUI (unique per device)", "secret": False},
        "joinEui": {"description": "16-hex JoinEUI / AppEUI", "secret": False},
        "appKey": {"description": "32-hex AppKey (LoRaWAN OTAA root key)", "secret": True},
    },
    "nvsKeys": ["lorawan_deveui", "lorawan_joineui", "lorawan_appkey"],
}
_CMD_PROFILE = {
    "id": "prov-profile",
    "description": "Store the selected device-profile blob in NVS (device register/sensor map + ADR-005 payload)",
    "syntax": "prov-profile <blobHex>",
    "example": "prov-profile 02100176…",
    "nvsKeys": ["prov/profile"],
}


def _cmd_show(bus: str) -> dict:
    keys = (
        ["modbus_baud", "modbus_parity", "modbus_stop", "modbus_slave", "lorawan_deveui", "lorawan_joineui"]
        if bus == "modbus"
        else ["lorawan_deveui", "lorawan_joineui"]
    )
    return {
        "id": "prov-show",
        "description": "Print current NVS provisioning state (appKey redacted, other values shown)",
        "syntax": "prov show",
        "nvsKeys": keys,
    }


@router.get("/provisioning-protocol")
async def get_provisioning_protocol(
    request: Request,
    product: Optional[str] = Query(default=None),
) -> dict:
    """
    GET /v1/provisioning-protocol — advisory console command set for a product (`?product=` defaults
    to careflow, whose payload is byte-identical to before). The WebSerial flasher renders its
    provisioning UI from this: careflow = prov-modbus/creds/show; senseflow = prov-creds/prov-profile/
    show (no modbus). ``appKey`` is flagged ``secret: true`` and is NEVER populated (T-01-09 / D-07).

    PLACEHOLDER CONTRACT (hard, do not drift). Every ``<placeholder>`` token in a command's ``syntax``
    is substituted by the flasher with ``context[placeholderName]``; its context map uses camelCase keys
    ``{devEui, joinEui, appKey, baud, parity, stopBits, slaveId, blobHex}``. A placeholder with no matching
    key throws ``renderProvisioningCommands: no context value for <NAME>`` and the flash fails — so each
    placeholder name MUST match one of those keys exactly. ``blobHex`` = the ``.blobHex`` field of
    ``GET /provisioning/firmware-build/profile-blob/<key>?taskId&nodeId``. If a protocol omits the
    ``prov-profile`` command, the flasher self-appends ``prov-profile <blobHex>`` itself.
    """
    prod = resolve_product(request, product)
    builders = {"modbus": _CMD_MODBUS, "creds": _CMD_CREDS, "profile": _CMD_PROFILE}
    commands = [
        _cmd_show(prod.bus) if cid == "show" else builders[cid] for cid in prod.prov_command_ids
    ]
    return {
        "firmwareTag": prod.firmware_tag,
        "console": {"baud": 115200, "promptReady": "esp> "},
        "commands": commands,
        "bootVerify": {
            "markers": list(prod.boot_markers),
            "description": (
                "Confirm these strings appear in the boot log before declaring provisioning complete."
            ),
        },
    }
