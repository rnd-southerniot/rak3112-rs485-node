"""
Sensor profile service: load device-profiles/*.json and compute flashable flag.

The flashable flag is NEVER read from the profile JSON. It is derived at runtime
by cross-referencing compiled_readers.json (generated at Docker build time by
parsing meter.h) against READER_SUFFIX_TO_BYTE.

Adding a new meter_read_<dev>.c reader to the firmware and rebuilding the image
automatically flips the corresponding profile to flashable=true with zero CRM
change and zero change to the profile JSON (decision D-01).

Source: .planning/phases/01-firmware-service-in-cluster-b/01-RESEARCH.md Pattern 2
"""
import json
from pathlib import Path

from api.models import ModbusParams, Sensor

# Module root: api/services/../../  == rak3112-rs485-node/ (local) or /app/ (Docker)
_APP_ROOT = Path(__file__).resolve().parent.parent.parent

# Device byte registry — maps meter_read_<suffix> function suffixes to deviceByte values.
# Source: firmware/components/payload/include/telemetry.h enum + Phase 6 state block.
READER_SUFFIX_TO_BYTE: dict[str, int] = {
    "mfm384": 1,  # SELEC MFM384 3-Phase Energy Meter
    "rsfsjt": 2,  # RS-FSJT-N01 Wind Speed Sensor
    "eem400": 3,  # Honeywell EEM400 Energy Meter (not yet compiled in)
    "dse": 4,  # DeepSea DSE Generator Controller (not yet compiled in)
}


def load_sensors(
    compiled_readers_path: str | Path = _APP_ROOT / "compiled_readers.json",
    profiles_dir: str | Path = _APP_ROOT / "device-profiles",
) -> list[Sensor]:
    """
    Load sensor profiles from device-profiles/*.json and compute the flashable
    flag from the compiled_readers.json manifest baked into the image.

    Parameters
    ----------
    compiled_readers_path:
        Path to compiled_readers.json — a JSON array of meter_read_<suffix>
        suffixes extracted from meter.h at Docker build time.
        Defaults to the co-located path in the deployed image (or repo root
        for local development).
    profiles_dir:
        Directory containing one JSON file per device profile.
        Defaults to device-profiles/ relative to the application root.

    Returns
    -------
    list[Sensor]:
        Merged list of Sensor objects with computed flashable flag.
        Sorted by profile filename (deterministic ordering).
    """
    compiled_readers_path = Path(compiled_readers_path)
    profiles_dir = Path(profiles_dir)

    # Build set of deviceBytes whose readers are compiled into this image.
    readers: list[str] = json.loads(compiled_readers_path.read_text())
    flashable_bytes: set[int] = {
        READER_SUFFIX_TO_BYTE[r] for r in readers if r in READER_SUFFIX_TO_BYTE
    }

    sensors: list[Sensor] = []
    for profile_file in sorted(profiles_dir.glob("*.json")):
        raw: dict = json.loads(profile_file.read_text())
        # flashable is ALWAYS computed — never read from the JSON (D-01 enforcement).
        sensors.append(
            Sensor(
                profileKey=raw["profileKey"],
                displayName=raw["displayName"],
                manufacturer=raw["manufacturer"],
                model=raw["model"],
                deviceByte=raw["deviceByte"],
                modbus=ModbusParams(**raw["modbus"]),
                payloadBytes=raw["payloadBytes"],
                flashable=(raw["deviceByte"] in flashable_bytes),
                isActive=raw["isActive"],
            )
        )
    return sensors


# --- P5.1: bus-agnostic sensor list (v2, multi-product) --------------------------------------------


def _flashable_bytes(reader_manifest: Path, reader_map: dict[str, int]) -> set[int]:
    """Device bytes whose reader/sensor driver is compiled into the product's firmware."""
    try:
        readers: list[str] = json.loads(Path(reader_manifest).read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        readers = []
    return {reader_map[r] for r in readers if r in reader_map}


def load_sensors_v2(product) -> list:
    """Load a product's device catalog into bus-agnostic SensorV2 (Modbus meters OR I2C sensors).

    Reads product.catalog_dir/*.json leniently (careflow's v1 catalog and senseflow's richer catalog
    have different shapes). flashable is computed from the product's reader/sensor manifest, never read
    from the JSON (D-01)."""
    from api.models_v2 import I2CParams, SensorV2

    flashable = _flashable_bytes(product.reader_manifest, product.reader_map)
    out: list[SensorV2] = []
    for f in sorted(Path(product.catalog_dir).glob("*.json")):
        raw: dict = json.loads(f.read_text())
        bus = raw.get("bus", product.bus)
        modbus = ModbusParams(**raw["modbus"]) if isinstance(raw.get("modbus"), dict) else None
        i2c = None
        if bus == "i2c":
            i2c = I2CParams(
                addr=int(raw.get("i2cAddr", raw.get("addr", 0))),
                sensorType=str(raw.get("sensorType", "")),
            )
        out.append(
            SensorV2(
                profileKey=raw["profileKey"],
                displayName=raw.get("displayName") or raw.get("model") or raw["profileKey"],
                manufacturer=raw.get("manufacturer", ""),
                model=raw.get("model", ""),
                deviceByte=raw["deviceByte"],
                bus=bus,
                modbus=modbus,
                i2c=i2c,
                measurands=raw.get("measurands", []),
                payloadBytes=raw["payloadBytes"],
                flashable=(raw["deviceByte"] in flashable),
                isActive=raw.get("isActive", True),
            )
        )
    return out
