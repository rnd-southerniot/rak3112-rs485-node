"""Products router — multi-product discovery + bus-agnostic sensors (P5.1).

- GET /v1/products       — list registered products (careflow + senseflow if configured).
- GET /v2/sensors        — bus-agnostic sensor list for ?product= (default careflow). This is the v2
  home for I2C sensors, which cannot fit the FROZEN v1 Sensor shape (required modbus block).

Bearer-protected like the rest of the API.
"""
from typing import Optional

from fastapi import APIRouter, Depends, Query, Request

from api.auth import verify_bearer
from api.models_v2 import ProductInfo, SensorV2
from api.products import get_products, resolve_product
from api.services.profiles import load_sensors_v2

router = APIRouter(dependencies=[Depends(verify_bearer)])


@router.get("/v1/products", response_model=list[ProductInfo])
async def list_products(request: Request) -> list[ProductInfo]:
    """List the registered firmware products (CRM discovery entry point)."""
    return [
        ProductInfo(id=p.id, displayName=p.display_name, bus=p.bus, firmwareTag=p.firmware_tag)
        for p in get_products(request).values()
    ]


@router.get("/v2/sensors", response_model=list[SensorV2])
async def get_sensors_v2(
    request: Request,
    product: Optional[str] = Query(default=None),
    flashable: Optional[bool] = Query(default=None),
) -> list[SensorV2]:
    """Bus-agnostic sensor list for a product. `?product=` defaults to careflow; `?flashable=` filters
    to sensors whose reader/driver is compiled into that product's firmware."""
    p = resolve_product(request, product)
    sensors = load_sensors_v2(p)
    if flashable is not None:
        sensors = [s for s in sensors if s.flashable == flashable]
    return sensors
