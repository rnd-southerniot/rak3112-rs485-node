"""
RAK3112 Firmware Service — FastAPI application entry point.

Mounts all /v1/* routers. Lifespan caching (startup sensor load + MinIO client
init) is added in Plan 01-05 once the full router set is in place.

Source: .planning/phases/01-firmware-service-in-cluster-b/01-02-PLAN.md
"""
from fastapi import FastAPI

from api.routers.sensors import router as sensors_router

app = FastAPI(
    title="RAK3112 Firmware Service",
    version="1.0.0",
    description=(
        "Private in-cluster firmware service: sensor profile registry and "
        "build artifact API for the rak3112-rs485-node. CRM-only consumer "
        "via ClusterIP (decision D-06)."
    ),
)

app.include_router(sensors_router)
