# ===========================================================================
# RAK3112 Firmware Service — Multi-stage Dockerfile
#
# Stage 1 (builder): espressif/idf:v5.5.4 — compiles firmware once at image-build time.
#   - Runs idf.py build for ESP32-S3 with the all-zero placeholder credentials (D-07).
#   - Generates compiled_readers.json from meter.h (the live flashable signal, D-01).
#
# Stage 2 (runtime): python:3.12-slim — ships only what the FastAPI service needs.
#   - No ESP-IDF toolchain in the final image (4.4 GB builder stays in the cache).
#   - Serves the cached firmware.bin via MinIO presigned URLs (D-06).
#
# Build (from the rak3112-rs485-node repo root, checked out at the git sha you are tagging).
# Since P7 the LoRaWAN stack lives in the PRIVATE component rnd-southerniot/siot-lorawan-node
# (firmware/main/idf_component.yml git: dep), so the builder needs read access to that repo.
# A fine-grained PAT (read-only Contents on siot-lorawan-node) is passed as a BuildKit secret;
# it is used only to fetch the component and is never written to any image layer:
#   export COMPONENT_REPO_TOKEN=<fine-grained PAT>
#   DOCKER_BUILDKIT=1 docker build \
#     --secret id=component_repo_token,env=COMPONENT_REPO_TOKEN \
#     --build-arg FIRMWARE_TAG=<git-sha> \
#     -t fahim0173/rak3112-firmware-service:<git-sha> .
# (Fallback without a Dockerfile secret: pre-fetch the component into
#  firmware/components/siot-lorawan-node/ before `docker build` — see docs/DEPLOY_KICKOFF.md.)
#
# Decisions implemented: D-01 (compiled_readers.json at build time), D-05 (generic .bin),
#   D-06 (build-once; tag-keyed MinIO cache), D-07 (placeholder creds; no secret in artifact).
# ===========================================================================

# ---------------------------------------------------------------------------
# Stage 1: Firmware builder
# Base: espressif/idf:v5.5.4 (4.41 GB compressed, amd64 + arm64)
# Source: rak3112-rs485-node CLAUDE.md §1 toolchain pin + CI ci.yml esp_idf_version: v5.5.4
# ---------------------------------------------------------------------------
FROM espressif/idf:v5.5.4 AS builder

# Copy entire firmware source tree into the builder.
# managed_components/, build/, sdkconfig, and lora_credentials.h are excluded by
# .dockerignore so only clean source enters the build context.
COPY firmware/ /build/firmware/

# D-07 enforcement — this COPY MUST come AFTER the firmware COPY above.
# It overwrites any lora_credentials.h that might have been in the context
# (defense-in-depth: .dockerignore also excludes the real file).
# lora_credentials.h.example defines LORA_APPKEY[16]={0} (all-zero placeholder)
# so the compiled binary carries no AppKey or secret (commit e28fe51 pattern).
COPY firmware/main/lora_credentials.h.example /build/firmware/main/lora_credentials.h

WORKDIR /build/firmware

# Ensure the PRIVATE shared LoRaWAN component is present as a LOCAL component in
# firmware/components/ — this takes precedence over the git: managed dependency in
# main/idf_component.yml, so the `idf.py build` below performs NO authenticated clone
# (mirrors the CI idf-build job). Two supported paths:
#   (A) Secret build (default): pass --secret id=component_repo_token; this RUN clones the
#       component with the token. The token is mounted only for this RUN (never in a layer);
#       the tokenized remote URL lives only inside .git, deleted immediately after the clone.
#   (B) Pre-fetch fallback: check the component out into firmware/components/siot-lorawan-node/
#       BEFORE `docker build` (it is copied in by `COPY firmware/` above, and is NOT excluded by
#       .dockerignore). This RUN then detects it and skips the clone — no secret needed.
# The pinned ref v0.1.0 must match the version in main/idf_component.yml.
RUN --mount=type=secret,id=component_repo_token \
    if [ -d /build/firmware/components/siot-lorawan-node ]; then \
      echo "siot-lorawan-node already present (pre-fetch fallback) — skipping clone"; \
    else \
      TOKEN="$(cat /run/secrets/component_repo_token)" && \
      git clone --depth 1 --branch v0.1.0 \
        "https://${TOKEN}@github.com/rnd-southerniot/siot-lorawan-node.git" \
        /build/firmware/components/siot-lorawan-node && \
      rm -rf /build/firmware/components/siot-lorawan-node/.git; \
    fi

# Compile the firmware.
# jgromes/radiolib==7.7.1 is pulled transitively through the siot-lorawan-node component (local,
# fetched above); espressif/led_strip is fetched from components.espressif.com (internet required).
# Output: build/rak3112_rs485_node.bin (project name from firmware/CMakeLists.txt).
RUN export IDF_PATH_FORCE=1 && . "$IDF_PATH/export.sh" && idf.py set-target esp32s3 && idf.py build

# Generate compiled_readers.json: regex-extract meter_read_<suffix>( declarations from
# meter.h. These suffixes (e.g. "mfm384", "rsfsjt") are the authoritative signal for
# which readers are compiled into this binary — the FastAPI service uses this file to
# compute the flashable flag on GET /v1/sensors without any static field in the JSON
# registry (D-01). Expected output on main: ["mfm384", "rsfsjt", "profile"] (the ADR-006 generic
# profile-driven reader is compiled in alongside the two model-specific readers).
RUN python3 -c "\
import re, json, pathlib; \
h = pathlib.Path('/build/firmware/components/meter/include/meter.h').read_text(); \
readers = re.findall(r'meter_read_(\w+)\s*\(', h); \
pathlib.Path('/build/compiled_readers.json').write_text(json.dumps(readers)); \
print('Compiled readers:', readers) \
"

# ---------------------------------------------------------------------------
# Stage 2: FastAPI runtime (slim — no ESP-IDF toolchain)
# Only the .bin, reader manifest, api package, and device profiles cross the stage.
# The final image is a few hundred MB, not 4.4 GB.
# ---------------------------------------------------------------------------
FROM python:3.12-slim AS runtime

# Surface the pinned firmware tag as a container env var so the service can report
# which firmware tag it carries without reading from disk.
ARG FIRMWARE_TAG=phase-6-modbus-green
ENV FIRMWARE_TAG=${FIRMWARE_TAG}

WORKDIR /app

# Firmware artifact — compiled with all-zero placeholder creds (D-07).
# This is the single generic .bin; per-device identity is NVS data injected at flash
# time, never baked into the binary (D-05).
COPY --from=builder /build/firmware/build/rak3112_rs485_node.bin /app/firmware/rak3112_rs485_node.bin

# Boot artifacts for the dual-OTA flash-set (GET /v1/flash-manifest / flash-part). Without these the
# flash endpoints 500 (missing_flash_part). ESP-IDF emits them under build/{bootloader,partition_table}
# and ota_data_initial.bin; flash.py resolves bootloader.bin / partition-table.bin / ota_data.bin from
# the firmware dir, so copy + rename the otadata.
COPY --from=builder /build/firmware/build/bootloader/bootloader.bin /app/firmware/bootloader.bin
COPY --from=builder /build/firmware/build/partition_table/partition-table.bin /app/firmware/partition-table.bin
COPY --from=builder /build/firmware/build/ota_data_initial.bin /app/firmware/ota_data.bin

# flashable-flag manifest — generated at build time from meter.h (D-01).
# The FastAPI service reads this at startup and computes which profiles are flashable.
COPY --from=builder /build/compiled_readers.json ./compiled_readers.json

# Python runtime dependencies — all packages slopcheck-[OK] per 01-RESEARCH.md audit.
# Versions pinned; no floating constraints.
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# FastAPI application package (api/main.py entrypoint, dual routers, lifespan).
COPY api/ ./api/

# Device profile registry (device-profiles/*.json — flashable computed at runtime, not stored).
COPY device-profiles/ ./device-profiles/

# Make the api package importable as a top-level module from /app.
ENV PYTHONPATH=/app

# FastAPI service listens on port 8000 (k8s/deployment.yaml containerPort: 8000).
EXPOSE 8000

# Run the ASGI server. Matches the k8s Deployment command and ClusterIP service target.
CMD ["uvicorn", "api.main:app", "--host", "0.0.0.0", "--port", "8000"]
