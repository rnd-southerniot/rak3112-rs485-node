"""
Environment-injected settings for the rak3112 firmware service.

Uses pydantic-settings so all values come from env vars / k8s Secrets.
Provides a cached get_settings() accessor for FastAPI Depends() injection,
which allows test overrides via get_settings.cache_clear() + monkeypatching.

Source: .planning/phases/01-firmware-service-in-cluster-b/01-PATTERNS.md
"""
from functools import lru_cache

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(
        env_file=".env",
        env_file_encoding="utf-8",
        extra="ignore",
    )

    API_TOKEN: str
    MINIO_INTERNAL_ENDPOINT: str = "minio.storage.svc.cluster.local:80"
    MINIO_EXTERNAL_ENDPOINT: str = "10.10.8.171:30900"
    MINIO_ACCESS_KEY: str
    MINIO_SECRET_KEY: str
    MINIO_BUCKET: str = "firmware-artifacts"
    FIRMWARE_TAG: str = "phase-7-provisioning-green"
    # Absolute path to the firmware binary baked into the Docker image at build time.
    # The Docker build runs `idf.py build` and copies the resulting .bin here.
    FIRMWARE_PATH: str = "/app/firmware/rak3112_rs485_node.bin"
    PRESIGNED_EXPIRY_HOURS: int = 1

    # --- senseflow (2nd product, P5.1 — additive; empty SENSEFLOW_ROOT disables it) --------------
    # Model B (pre-built artifact): senseflow publishes its .bin + boot parts + device-profiles
    # catalog + compiled_sensors.json as a tagged artifact; the service references it by unpacking
    # the artifact under SENSEFLOW_ROOT (default /app/products/senseflow in the image). The careflow
    # (default) product is unaffected when this is unset.
    SENSEFLOW_ROOT: str = ""
    SENSEFLOW_FIRMWARE_TAG: str = "senseflow-p4-green"


@lru_cache()
def get_settings() -> Settings:
    """Return a cached Settings instance (test-overridable via cache_clear)."""
    return Settings()
