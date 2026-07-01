"""
Bearer token auth dependency for all /v1/* endpoints.

Usage in routers:
    router = APIRouter(prefix="/v1", dependencies=[Depends(verify_bearer)])

Settings are injected via Depends(get_settings) so tests can override the
token by patching get_settings (no inline token checks anywhere else).

Source: .planning/phases/01-firmware-service-in-cluster-b/01-RESEARCH.md Pattern 5
"""
from fastapi import Depends, HTTPException, Security
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer

from api.config import Settings, get_settings

bearer_scheme = HTTPBearer()


def verify_bearer(
    credentials: HTTPAuthorizationCredentials = Security(bearer_scheme),
    settings: Settings = Depends(get_settings),
) -> None:
    """Raise 401 if the bearer token does not match settings.API_TOKEN."""
    if credentials.credentials != settings.API_TOKEN:
        raise HTTPException(
            status_code=401,
            detail={"error": {"code": "unauthorized", "message": "Invalid token"}},
        )
