"""
MinIO dual-endpoint storage service for the rak3112 firmware build cache.

Design: Stateless build tracking via asyncio lock + MinIO object existence.
No external DB or queue (D-04 superseded v2).

Two MinIO clients (Pattern 3 — Research §Architecture Patterns):
  - internal: minio.storage.svc.cluster.local:80
      → stat_object + fput_object (in-cluster ClusterIP DNS)
  - external: 10.10.8.171:30900
      → presigned_get_object only (NodePort reachable from LAN browsers)

Key layout (Claude's Discretion):
  builds/{firmwareTag}/rak3112_rs485_node.bin   — binary artifact (D-05: one generic image)
  builds/{firmwareTag}/manifest.json            — {sha256, builtAt, firmwareTag}

Source: .planning/phases/01-firmware-service-in-cluster-b/01-03-PLAN.md Task 1
"""
import asyncio
import hashlib
import io
import json
import logging
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Optional

from minio import Minio
from minio.error import S3Error

from api.config import Settings
from api.models import Build

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Module-level asyncio lock: prevents concurrent uploads for the same tag.
# One lock per process (one pod = one process). Double-checked locking pattern
# avoids holding the lock on the common fast path (already cached).
# ---------------------------------------------------------------------------
_build_lock: asyncio.Lock = asyncio.Lock()


# ---------------------------------------------------------------------------
# Key helpers
# ---------------------------------------------------------------------------


def _bin_key(tag: str) -> str:
    """S3 object key for the firmware binary."""
    return f"builds/{tag}/rak3112_rs485_node.bin"


def _manifest_key(tag: str) -> str:
    """S3 object key for the build manifest (sha256 + metadata)."""
    return f"builds/{tag}/manifest.json"


# ---------------------------------------------------------------------------
# Client factory (Pattern 3)
# ---------------------------------------------------------------------------


def make_minio_clients(settings: Settings) -> tuple[Minio, Minio]:
    """
    Create the dual MinIO clients from settings.

    Returns (internal, external):
      - internal: stat_object + fput_object via in-cluster ClusterIP
      - external: presigned_get_object only — generates browser-reachable URLs

    Both use the same access/secret key (scoped service account on bucket
    firmware-artifacts, Pitfall 6: 8–40 char key length enforced at provision).
    """
    internal = Minio(
        settings.MINIO_INTERNAL_ENDPOINT,
        access_key=settings.MINIO_ACCESS_KEY,
        secret_key=settings.MINIO_SECRET_KEY,
        secure=False,
    )
    external = Minio(
        settings.MINIO_EXTERNAL_ENDPOINT,
        access_key=settings.MINIO_ACCESS_KEY,
        secret_key=settings.MINIO_SECRET_KEY,
        secure=False,
    )
    return internal, external


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------


def _is_cached(internal: Minio, bucket: str, key: str) -> bool:
    """
    Return True if the object exists in MinIO.

    Treats S3Error with code NoSuchKey/NoSuchBucket/NotFound as a cache-miss
    (not-cached) and returns False. Other S3Errors (auth, unexpected network
    codes) are re-raised after logging — do NOT bare-except (Pitfall 2).
    """
    try:
        internal.stat_object(bucket, key)
        return True
    except S3Error as exc:
        if exc.code in ("NoSuchKey", "NoSuchBucket", "NotFound"):
            logger.debug("Cache miss: code=%s bucket=%s key=%s", exc.code, bucket, key)
            return False
        logger.error(
            "MinIO S3Error (unexpected): code=%s bucket=%s key=%s",
            exc.code,
            bucket,
            key,
        )
        raise


def _compute_sha256(firmware_path: Path) -> str:
    """Compute the hex SHA-256 digest of the local firmware binary."""
    return hashlib.sha256(firmware_path.read_bytes()).hexdigest()


def _presigned_url(external: Minio, bucket: str, key: str, expiry_hours: int) -> str:
    """
    Generate a presigned GET URL from the EXTERNAL MinIO client.

    MUST use the external client: the internal hostname
    (minio.storage.svc.cluster.local) is not resolvable from outside the
    cluster. The engineer's browser GETs this URL directly (Phase A WebSerial
    flasher). Expiry is configurable; default 1 h (Pitfall 4).
    """
    return external.presigned_get_object(
        bucket_name=bucket,
        object_name=key,
        expires=timedelta(hours=expiry_hours),
    )


def _ready_build(
    tag: str,
    *,
    external: Minio,
    bucket: str,
    firmware_path: Path,
    expiry_hours: int,
    built_at: Optional[datetime] = None,
) -> Build:
    """Construct a ready Build from the local firmware binary (baked at image build time)."""
    sha256 = _compute_sha256(firmware_path)
    url = _presigned_url(external, bucket, _bin_key(tag), expiry_hours)
    return Build(
        firmwareTag=tag,
        status="ready",
        binarySha256=sha256,
        binaryUrl=url,
        builtAt=built_at or datetime.now(tz=timezone.utc),
    )


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


async def ensure_built(
    tag: str,
    *,
    internal: Minio,
    external: Minio,
    bucket: str,
    firmware_path: Path,
    expiry_hours: int,
) -> Build:
    """
    Idempotent: upload the firmware binary to MinIO once, return the ready Build.

    All parameters are injectable so tests pass minio_mock + fake_firmware_bin
    fixtures without hitting real MinIO or any DB/queue (D-04 superseded).

    Flow:
      1. stat_object the bin key — if cached, return ready Build immediately
         (fast path: no lock contention).
      2. Acquire _build_lock to prevent concurrent uploads.
      3. Re-check under lock (double-checked locking).
      4. Compute sha256 from local firmware_path (the baked generic binary, D-05).
      5. fput_object the binary to internal MinIO.
      6. put_object the manifest.json {sha256, builtAt, firmwareTag}.
      7. Return ready Build with presigned binaryUrl from the EXTERNAL client.
    """
    bin_key = _bin_key(tag)

    # Fast path: already cached (no lock overhead on common case)
    if _is_cached(internal, bucket, bin_key):
        return _ready_build(
            tag,
            external=external,
            bucket=bucket,
            firmware_path=firmware_path,
            expiry_hours=expiry_hours,
        )

    # Slow path: upload under lock
    async with _build_lock:
        # Double-checked locking: re-verify after acquiring the lock to handle
        # the case where another coroutine completed the upload while we waited.
        if _is_cached(internal, bucket, bin_key):
            return _ready_build(
                tag,
                external=external,
                bucket=bucket,
                firmware_path=firmware_path,
                expiry_hours=expiry_hours,
            )

        built_at = datetime.now(tz=timezone.utc)
        sha256 = _compute_sha256(firmware_path)

        # Upload the firmware binary (D-05: single generic image, no per-device data)
        internal.fput_object(bucket, bin_key, str(firmware_path))
        logger.info(
            "Uploaded firmware binary: bucket=%s key=%s sha256=%.16s…",
            bucket,
            bin_key,
            sha256,
        )

        # Upload the manifest for traceability and downstream verification
        manifest_bytes = json.dumps(
            {
                "sha256": sha256,
                "builtAt": built_at.isoformat(),
                "firmwareTag": tag,
            }
        ).encode("utf-8")
        internal.put_object(
            bucket,
            _manifest_key(tag),
            io.BytesIO(manifest_bytes),
            len(manifest_bytes),
            content_type="application/json",
        )

        url = _presigned_url(external, bucket, bin_key, expiry_hours)
        return Build(
            firmwareTag=tag,
            status="ready",
            binarySha256=sha256,
            binaryUrl=url,
            builtAt=built_at,
        )


async def get_cached_build(
    tag: str,
    *,
    internal: Minio,
    external: Minio,
    bucket: str,
    firmware_path: Path,
    expiry_hours: int,
) -> Optional[Build]:
    """
    Return a ready Build if the binary is cached in MinIO, else None.

    Called by GET /v1/builds/{tag}: does NOT trigger an upload — cache-check
    only. Returns None when the tag has never been built (drives 404 in the
    router). Parameters are injectable for test isolation (minio_mock fixture).
    """
    bin_key = _bin_key(tag)
    if not _is_cached(internal, bucket, bin_key):
        return None

    return _ready_build(
        tag,
        external=external,
        bucket=bucket,
        firmware_path=firmware_path,
        expiry_hours=expiry_hours,
    )
