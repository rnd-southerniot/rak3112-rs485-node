"""
Profile-blob router: GET /v1/profile-blob/{profile_key}.

Serializes a device profile JSON into the firmware's NVS blob (the exact
dp_deserialize() byte format, ADR-006) so the CRM WebSerial flasher can push it
to a board with `prov-profile <hex>` during the factory console phase. This is
the interim §4 path (CRM_PROVISIONING_WORKFLOW): the profile travels to NVS at
flash time until the build-time profile→C generator lands.

Reuses device-profiles/profile_to_blob.py verbatim (the same module the C host
test cross-checks against), loaded by file path because the directory name is
not a valid python package name.
"""
import importlib.util
import json
import logging
import re
from functools import lru_cache
from pathlib import Path
from typing import Optional

from fastapi import APIRouter, Depends, HTTPException, Query, Request

from api.auth import verify_bearer
from api.products import resolve_product

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/v1", dependencies=[Depends(verify_bearer)])

_PROFILE_KEY_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")


@lru_cache(maxsize=4)
def _blob_module(path: str):
    """Load a product's profile_to_blob.py as a module (cached per path; hyphenated dir)."""
    spec = importlib.util.spec_from_file_location(f"profile_to_blob_{abs(hash(path))}", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load profile serializer at {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _not_found(profile_key: str) -> HTTPException:
    return HTTPException(
        status_code=404,
        detail={"error": {"code": "not_found", "message": f"Unknown device profile '{profile_key}'."}},
    )


@router.get("/profile-blob/{profile_key}")
async def get_profile_blob(
    profile_key: str,
    request: Request,
    product: Optional[str] = Query(default=None),
) -> dict:
    """
    Return the NVS profile blob for one device profile: hex bytes ready for `prov-profile <hex>`,
    plus the device_byte the flasher asserts back via `prov-show`. `?product=` defaults to careflow.

    careflow re-serializes its full profile via its v1 `profile_to_blob.py`; senseflow (I2C, v2 blob)
    serves the pre-computed `blobHex` from its published catalog.
    """
    if not _PROFILE_KEY_RE.match(profile_key):
        raise HTTPException(
            status_code=400,
            detail={"error": {"code": "bad_profile_key", "message": "Invalid profile key."}},
        )
    p = resolve_product(request, product)

    # Catalog-blobHex path (senseflow / any product without a request-time serializer).
    if p.serializer_path is None:
        cat_path = (p.catalog_dir or Path("/nonexistent")) / f"{profile_key}.json"
        if not cat_path.exists():
            raise _not_found(profile_key)
        doc = json.loads(cat_path.read_text())
        blob_hex = doc.get("blobHex")
        if not blob_hex:
            raise HTTPException(
                status_code=500,
                detail={"error": {"code": "no_blob", "message": f"Catalog '{profile_key}' has no blobHex."}},
            )
        return {
            "profileKey": profile_key,
            "deviceByte": doc["deviceByte"],
            "byteLen": len(blob_hex) // 2,
            "blobHex": blob_hex,
        }

    # Re-serialize path (careflow): full profile JSON -> profile_to_blob.py.
    profile_path = (p.profiles_dir or Path("/nonexistent")) / f"{profile_key}.json"
    if not profile_path.exists():
        raise _not_found(profile_key)
    mod = _blob_module(str(p.serializer_path))
    doc = json.loads(profile_path.read_text())
    normalized = mod.profile_from_json(doc)
    blob: bytes = mod.build_blob(normalized)
    return {
        "profileKey": profile_key,
        "deviceByte": normalized["device_byte"],
        "byteLen": len(blob),
        "blobHex": blob.hex(),
    }
