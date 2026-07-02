"""
RAK3112 Firmware Service — FastAPI application entry point.

Assembles the service: mounts sensors and builds routers, caches the sensor
list and initializes dual MinIO clients + firmware path in app.state at startup
(lifespan), renders errors in the frozen {error:{code,message}} contract shape,
and exposes an unauthenticated /healthz for the Kubernetes probe.

Source: .planning/phases/01-firmware-service-in-cluster-b/01-05-PLAN.md
"""
import logging
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any, AsyncGenerator

from fastapi import FastAPI, HTTPException, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse

from api.config import get_settings
from api.routers import builds as builds_module
from api.routers import flash as flash_module
from api.routers import profiles as profiles_module
from api.routers import sensors as sensors_module
from api.services.profiles import load_sensors
from api.services.storage import make_minio_clients

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Lifespan: cache sensors + initialize MinIO clients at startup (D-01/D-02/D-04)
# ---------------------------------------------------------------------------


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncGenerator[None, Any]:
    """
    Startup: cache the sensor profile list and initialize dual MinIO clients.

    This runs once at process startup (e.g. `uvicorn api.main:app`). Tests
    that use TestClient without the context-manager form do NOT trigger the
    lifespan — they use the app.state fallback paths in the routers (or the
    integration conftest injects the required state directly).

    app.state attributes set here:
      - sensors: list[Sensor]  — flashable-computed sensor list served by /v1/sensors
      - minio_internal: Minio — stat_object + fput_object via ClusterIP DNS
      - minio_external: Minio — presigned_get_object via NodePort (browser-reachable)
      - firmware_bucket: str   — MinIO bucket name
      - firmware_path: Path    — absolute path to the baked firmware binary
    """
    settings = get_settings()

    # Sensor cache: load once at startup so /v1/sensors serves from memory.
    # Falls back to on-disk load per-request if this fails (router fallback).
    try:
        app.state.sensors = load_sensors()
        logger.info(
            "Sensor cache initialized: %d sensors loaded", len(app.state.sensors)
        )
    except Exception as exc:  # pragma: no cover
        logger.warning(
            "load_sensors() failed at startup: %s — /v1/sensors will fall back to disk",
            exc,
        )
        # Do not set app.state.sensors — the router will call load_sensors() per request.

    # MinIO dual-endpoint clients (Pattern 3 from research):
    #   internal → stat_object + fput_object (in-cluster ClusterIP DNS)
    #   external → presigned_get_object (NodePort reachable from LAN browsers)
    internal, external = make_minio_clients(settings)
    app.state.minio_internal = internal
    app.state.minio_external = external
    app.state.firmware_bucket = settings.MINIO_BUCKET
    app.state.firmware_path = Path(settings.FIRMWARE_PATH)
    logger.info(
        "MinIO clients initialized (internal=%s, external=%s, bucket=%s)",
        settings.MINIO_INTERNAL_ENDPOINT,
        settings.MINIO_EXTERNAL_ENDPOINT,
        settings.MINIO_BUCKET,
    )

    yield
    # No persistent connections to close — MinIO clients are stateless HTTP wrappers.
    logger.info("Firmware service shutdown complete")


# ---------------------------------------------------------------------------
# FastAPI application
# ---------------------------------------------------------------------------

app = FastAPI(
    title="RAK3112 Firmware Service",
    version="1.0.0",
    description=(
        "Private in-cluster firmware service: sensor profile registry and "
        "build artifact API for the rak3112-rs485-node. CRM-only consumer "
        "via ClusterIP (decision D-06)."
    ),
    lifespan=lifespan,
)


# ---------------------------------------------------------------------------
# Error handlers — frozen {error:{code,message}} contract shape (T-01-12)
# ---------------------------------------------------------------------------


@app.exception_handler(HTTPException)
async def http_exception_handler(request: Request, exc: HTTPException) -> JSONResponse:
    """
    Reformat FastAPI HTTPExceptions into the frozen {error:{code,message}} shape.

    All HTTPExceptions raised in routers and dependencies use:
        detail = {"error": {"code": "<code>", "message": "<message>"}}

    FastAPI's default handler wraps this as {"detail": <exc.detail>}, which
    violates the contract. This handler unwraps dict details to the top level.
    """
    if isinstance(exc.detail, dict):
        # detail is already the contract dict — return it directly
        return JSONResponse(status_code=exc.status_code, content=exc.detail)
    # Fallback: plain-string or other detail → wrap in the frozen shape
    return JSONResponse(
        status_code=exc.status_code,
        content={"error": {"code": "http_error", "message": str(exc.detail)}},
    )


@app.exception_handler(RequestValidationError)
async def validation_error_handler(
    request: Request, exc: RequestValidationError
) -> JSONResponse:
    """
    Pydantic request validation errors → 422 in the frozen error shape (T-01-05a).

    Converts FastAPI's default {"detail": [errors]} into {"error": {...}} so
    the CRM can handle all error responses uniformly.
    """
    return JSONResponse(
        status_code=422,
        content={
            "error": {
                "code": "validation_error",
                "message": str(exc),
            }
        },
    )


@app.exception_handler(Exception)
async def unhandled_exception_handler(request: Request, exc: Exception) -> JSONResponse:
    """
    Catch-all for unhandled exceptions — 500 in the frozen error shape (T-01-12).

    Logs the full traceback server-side; returns no internal detail to clients.
    """
    logger.exception(
        "Unhandled exception processing %s %s", request.method, request.url.path
    )
    return JSONResponse(
        status_code=500,
        content={
            "error": {
                "code": "internal_error",
                "message": "An unexpected error occurred.",
            }
        },
    )


# ---------------------------------------------------------------------------
# Unauthenticated liveness/readiness probe (k8s kubelet — no bearer token)
# Trust boundary: returns no data, exposes no secret, no /v1/* handler path.
# ---------------------------------------------------------------------------


@app.get("/healthz", include_in_schema=False)
async def healthz() -> dict:
    """
    Kubernetes liveness / readiness probe.

    Unauthenticated — kubelet does not carry a bearer token. Returns {"status":
    "ok"} and nothing else (T-01-01: the probe route exposes no sensitive data).
    """
    return {"status": "ok"}


# ---------------------------------------------------------------------------
# /v1 API routers (both carry Depends(verify_bearer) at the APIRouter level)
# ---------------------------------------------------------------------------

app.include_router(sensors_module.router)
app.include_router(builds_module.router)
app.include_router(flash_module.router)
app.include_router(profiles_module.router)
