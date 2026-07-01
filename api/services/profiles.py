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
