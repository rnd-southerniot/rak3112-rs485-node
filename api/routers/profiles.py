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
import logging
import re
from functools import lru_cache
from pathlib import Path

from fastapi import APIRouter, Depends, HTTPException

from api.auth import verify_bearer

logger = logging.getLogger(__name__)

router = APIRouter(prefix="/v1", dependencies=[Depends(verify_bearer)])

_APP_ROOT = Path(__file__).resolve().parent.parent.parent
_DP_ROOT = _APP_ROOT / "device-profiles"
# Full firmware profiles (bus/defaults/scan/payload schema) live in profiles/;
# the top-level *.json files are the CRM catalog variants and cannot serialize.
_PROFILES_DIR = _DP_ROOT / "profiles"
_PROFILE_KEY_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")


@lru_cache(maxsize=1)
def _blob_module():
    """Load device-profiles/profile_to_blob.py as a module (hyphenated dir)."""
    path = _DP_ROOT / "profile_to_blob.py"
    spec = importlib.util.spec_from_file_location("profile_to_blob", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load profile serializer at {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@router.get("/profile-blob/{profile_key}")
async def get_profile_blob(profile_key: str) -> dict:
    """
    Return the NVS profile blob for one device profile: hex bytes ready for
    `prov-profile <hex>`, plus the device_byte the flasher asserts back via
    `prov-show` after the boot-verify reset.
    """
    if not _PROFILE_KEY_RE.match(profile_key):
        raise HTTPException(
            status_code=400,
            detail={"error": {"code": "bad_profile_key", "message": "Invalid profile key."}},
        )
    profile_path = _PROFILES_DIR / f"{profile_key}.json"
    if not profile_path.exists():
        raise HTTPException(
            status_code=404,
            detail={
                "error": {
                    "code": "not_found",
                    "message": f"Unknown device profile '{profile_key}'.",
                }
            },
        )
    import json

    mod = _blob_module()
    doc = json.loads(profile_path.read_text())
    normalized = mod.profile_from_json(doc)
    blob: bytes = mod.build_blob(normalized)
    return {
        "profileKey": profile_key,
        "deviceByte": normalized["device_byte"],
        "byteLen": len(blob),
        "blobHex": blob.hex(),
    }
