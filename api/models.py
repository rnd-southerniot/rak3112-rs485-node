"""
Frozen contract Pydantic shapes for the rak3112 firmware service.

These shapes are the coupling point between Phase B (firmware service) and
Phase A (CRM consumer). Field names are the frozen surface — no renaming.
Change via /v2, not in place.

Source: docs/contracts/firmware-service-api-v1.md (FROZEN 2026-06-29)
"""
from datetime import datetime
from typing import Literal, Optional

from pydantic import BaseModel


class ModbusParams(BaseModel):
    baud: int
    parity: str
    stopBits: int
    functionCode: int
    wordOrder: str


class Sensor(BaseModel):
    profileKey: str
    displayName: str
    manufacturer: str
    model: str
    deviceByte: int
    modbus: ModbusParams
    payloadBytes: int
    flashable: bool
    isActive: bool


class Build(BaseModel):
    firmwareTag: str
    status: Literal["queued", "building", "ready", "failed"]
    binarySha256: Optional[str] = None
    binaryUrl: Optional[str] = None
    builtAt: Optional[datetime] = None
    error: Optional[str] = None


class BuildRequest(BaseModel):
    firmwareTag: Optional[str] = None


class ErrorDetail(BaseModel):
    code: str
    message: str


class ErrorResponse(BaseModel):
    error: ErrorDetail
