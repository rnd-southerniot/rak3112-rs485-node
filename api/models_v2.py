"""/v2 contract shapes — bus-agnostic Sensor for multi-product (P5.1).

v1 `api/models.py` is FROZEN: `Sensor.modbus` is a REQUIRED Modbus block, so I2C sensors cannot be
represented there. `SensorV2` is bus-agnostic (Modbus meters OR I2C sensors) and is served by the new
`GET /v2/sensors`. v1 `/v1/sensors` is untouched (careflow/Modbus only).
"""
from typing import Optional

from pydantic import BaseModel

from api.models import ModbusParams


class I2CParams(BaseModel):
    addr: int
    sensorType: str


class SensorV2(BaseModel):
    profileKey: str
    displayName: str
    manufacturer: str
    model: str
    deviceByte: int
    bus: str  # "modbus" | "i2c"
    modbus: Optional[ModbusParams] = None
    i2c: Optional[I2CParams] = None
    measurands: list[str] = []
    payloadBytes: int
    flashable: bool
    isActive: bool


class ProductInfo(BaseModel):
    id: str
    displayName: str
    bus: str
    firmwareTag: str
