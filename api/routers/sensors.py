"""
GET /v1/sensors router — live sensor-profile registry.

Returns the merged list of device profiles with computed flashable flag.
Supports optional ?flashable=true|false filter (the CRM dropdown uses
?flashable=true to show only sensors the current firmware can serve).

Authentication: bearer token required on all requests (T-01-01 mitigation).
All requests without a valid bearer token return 401.

Source: .planning/phases/01-firmware-service-in-cluster-b/01-02-PLAN.md Task 2
"""
from typing import Optional

from fastapi import APIRouter, Depends, Query, Request

from api.auth import verify_bearer
from api.models import Sensor
from api.services.profiles import load_sensors

router = APIRouter(prefix="/v1", dependencies=[Depends(verify_bearer)])


@router.get("/sensors", response_model=list[Sensor])
async def get_sensors(
    request: Request,
    flashable: Optional[bool] = Query(default=None),
) -> list[Sensor]:
    """
    Return all sensor profiles with computed flashable flag.

    The flashable flag reflects which meter_read_* readers are compiled into
    the current firmware image — it is derived from compiled_readers.json,
    never from the profile JSON itself (decision D-01).

    Query Parameters
    ----------------
    flashable : bool | None
        When true, return only flashable sensors (CRM procurement dropdown).
        When false, return only non-flashable sensors.
        When omitted, return all sensors.
    """
    # Use cached sensor list from app state when available (populated at lifespan
    # in Plan 05). Fall back to loading from disk for standalone testability.
    sensors: list[Sensor] = getattr(request.app.state, "sensors", None)
    if sensors is None:
        sensors = load_sensors()

    if flashable is not None:
        sensors = [s for s in sensors if s.flashable == flashable]

    return sensors
