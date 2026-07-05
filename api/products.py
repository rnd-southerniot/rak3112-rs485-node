"""Product registry — additive multi-product layer (P5.1).

The service was single-product (careflow RS-485/Modbus). This registers a second product
(senseflow e-ink / I2C) WITHOUT touching the FROZEN v1 contracts (api/models.py): every endpoint
gains an optional ``?product=`` param defaulting to ``careflow``, so existing v1 calls stay
byte-identical and all current tests pass.

Cross-repo sourcing is **model B (pre-built artifact)**: careflow's firmware is baked into the image
(FIRMWARE_PATH); senseflow's artifact (`.bin` + boot parts + device-profiles catalog +
`compiled_sensors.json`) is referenced by unpacking it under ``SENSEFLOW_ROOT``. Senseflow is
registered only when ``SENSEFLOW_ROOT`` is set — the service is careflow-only otherwise.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from api.config import Settings

DEFAULT_PRODUCT = "careflow"

# careflow: compiled meter_read_<suffix> -> ADR-005 device byte (Modbus meters, 0x01..0x0F).
CAREFLOW_READER_TO_BYTE: dict[str, int] = {"mfm384": 1, "rsfsjt": 2, "eem400": 3, "dse": 4}
# senseflow: compiled I2C sensor driver (sensor_type) -> device byte (0x10..).
SENSEFLOW_SENSOR_TO_BYTE: dict[str, int] = {"bme280": 0x10, "sgp40": 0x11, "shtc3": 0x12}

_REPO = Path(__file__).resolve().parent.parent  # rak3112-rs485-node/ (local) or /app (Docker)


@dataclass(frozen=True)
class Product:
    id: str
    display_name: str
    bus: str  # "modbus" | "i2c"
    firmware_tag: str
    binary_name: str
    firmware_path: Path  # the app binary (careflow: baked; senseflow: from artifact)
    flash_dir: Path  # holds bootloader/partition-table/ota_data + the binary
    reader_manifest: Path  # compiled_readers.json | compiled_sensors.json
    reader_map: dict[str, int]  # reader/sensor key -> device byte
    prov_command_ids: tuple[str, ...]  # which prov commands: modbus/creds/profile/show
    boot_markers: tuple[str, ...]
    # careflow re-serializes full profiles at request time; senseflow reads the catalog blobHex.
    profiles_dir: Path | None = None  # serializable full-profile JSONs (careflow)
    serializer_path: Path | None = None  # device-profiles/profile_to_blob.py (careflow)
    catalog_dir: Path | None = None  # catalog JSONs w/ blobHex (senseflow, from artifact)


def build_products(settings: Settings) -> dict[str, "Product"]:
    """Build the product registry from settings. careflow always present; senseflow if configured."""
    fw = Path(settings.FIRMWARE_PATH)
    products: dict[str, Product] = {
        "careflow": Product(
            id="careflow",
            display_name="Careflow RS-485/Modbus node (RAK3112)",
            bus="modbus",
            firmware_tag=settings.FIRMWARE_TAG,
            binary_name=fw.name,
            firmware_path=fw,
            flash_dir=fw.parent,
            reader_manifest=_REPO / "compiled_readers.json",
            reader_map=CAREFLOW_READER_TO_BYTE,
            prov_command_ids=("modbus", "creds", "show"),
            boot_markers=("[creds:NVS]", "PSRAM", "modbus"),
            profiles_dir=_REPO / "device-profiles" / "profiles",
            serializer_path=_REPO / "device-profiles" / "profile_to_blob.py",
            catalog_dir=_REPO / "device-profiles",
        )
    }
    if settings.SENSEFLOW_ROOT:
        root = Path(settings.SENSEFLOW_ROOT)
        products["senseflow"] = Product(
            id="senseflow",
            display_name="Senseflow e-ink / I2C node (RAK3312)",
            bus="i2c",
            firmware_tag=settings.SENSEFLOW_FIRMWARE_TAG,
            binary_name="senseflow_eink_node.bin",
            firmware_path=root / "senseflow_eink_node.bin",
            flash_dir=root,
            reader_manifest=root / "compiled_sensors.json",
            reader_map=SENSEFLOW_SENSOR_TO_BYTE,
            prov_command_ids=("creds", "profile", "show"),
            boot_markers=("[creds:NVS]", "PSRAM", "profile"),
            catalog_dir=root / "device-profiles",
        )
    return products


def get_products(request) -> dict[str, "Product"]:
    """Registry from app.state (set at lifespan); rebuild from settings for lifespan-less tests."""
    products = getattr(request.app.state, "products", None)
    if products is None:
        from api.config import get_settings

        products = build_products(get_settings())
    return products


def resolve_product(request, product_id: str | None) -> "Product":
    """Resolve the requested product (default careflow); 404 unknown_product otherwise."""
    products = get_products(request)
    pid = product_id or DEFAULT_PRODUCT
    p = products.get(pid)
    if p is None:
        from fastapi import HTTPException

        raise HTTPException(
            status_code=404,
            detail={
                "error": {
                    "code": "unknown_product",
                    "message": f"Unknown product '{pid}'. Known: {sorted(products)}.",
                }
            },
        )
    return p


def firmware_path_for(request, product: "Product") -> Path:
    """Effective binary path for a product. Honors a directly-injected app.state.firmware_path for the
    DEFAULT product (preserves the existing lifespan/test injection); registry path otherwise."""
    if product.id == DEFAULT_PRODUCT:
        injected = getattr(request.app.state, "firmware_path", None)
        if injected is not None:
            return injected
    return product.firmware_path
