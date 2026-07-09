# Deployment kickoff — prompt for Fahim's Claude Code

> Hand this to Fahim's Claude Code tomorrow morning (Arif will be present). It orients the session and
> says where to start. Confirm the exact target with Arif before executing anything outward-facing.

---

## Paste this into Fahim's Claude Code

You're helping deploy the **Careflow RS-485 ⇄ LoRaWAN** firmware-automation service, with Arif present.
Repo: `github.com/rnd-southerniot/rak3112-rs485-node` (branch `main`).

**What just landed on `main` (context for the deploy):**
- A **Pi 5 scanner/profiling station** (`pi-scanner/`): discovers an unknown RS-485 (Modbus) device
  through a Careflow node, infers + packages a reusable `device-profiles/profiles/<model>.json`, and
  drives the existing flash/provision/decoder flow. It runs as a touchscreen kiosk on the bench Pi
  (`192.168.68.109`) — systemd `careflow-scanner` service + Chromium `--app` kiosk (see
  `pi-scanner/deploy/`).
- A new scanner-discovered profile **`honeywell-eem400-scanned`** (type_byte 5) + the regenerated
  `device-profiles/chirpstack_fleet_decoder.js` and catalog.
- WS2812 status LED + the P7 production-hardening (all previously merged).

**The service being deployed:** the CRM firmware-build/provision service = `api/` (FastAPI) + `tools/`
+ `device-profiles/` in this repo, containerized by `Dockerfile`, deployed to **Fahim's k8s** as
`fahim0173/rak3112-firmware-service` (namespace `firmware`, ClusterIP, MinIO-backed for build
artifacts). Manifests in `k8s/` (`namespace.yaml`, `deployment.yaml`, `service.yaml`,
`secret.example.yaml`) and `deploy/cluster.yaml`. The canonical future home is the hub
`rnd-southerniot/siot-node-firmware-automation` (cutover deferred — this repo's `api/`+`tools/` are the
deployed source until then; see `CLAUDE.md` top "three-layer topology" note + §0.1).

**The Pi scanner station depends on this service** at `SCANNER_API_BASE` (currently
`http://10.10.8.169:8000`) for `/v1/flash-manifest`, `/v1/provisioning-protocol`, `/v1/profile-blob`,
`/v1/sensors`, and installs ChirpStack decoders on the **dev** stack `10.10.8.140`.

**Start here (orient, then plan with Arif — don't execute outward actions yet):**
1. `git pull` `main`; read `CLAUDE.md` (top topology note + §0.1 split-repo + §4 CI) and `README.md`.
2. Review deploy artifacts: `Dockerfile`, `k8s/*.yaml`, `deploy/cluster.yaml`, `.github/workflows/ci.yml`
   (jobs `idf-build`, `api-tests`, `device-profiles`, `pi-scanner-tests`).
3. Find the **currently-deployed** image/git-sha on the cluster (`kubectl -n firmware get deploy -o wide`)
   and diff it against `main` HEAD — what changed (new profiles, api/, tools/).
4. **Confirm the target with Arif:** (a) redeploy this repo's `api/` to Fahim's k8s, or (b) begin the
   hub (`siot-node-firmware-automation`) cutover. And confirm the **ChirpStack stack**.
5. Execute the **phase-gated runbook below** (§ "Executable runbook") — build + push image (pinned to
   the git sha, **no `:latest`**) → `kubectl` rollout → verify `/healthz`, `/v1/products`, `/v1/sensors`
   (should now list `honeywell-eem400-scanned`), `/v1/flash-manifest`, `/v1/provisioning-protocol` →
   confirm the Pi scanner station reaches it. Walk Arif through it before running outward actions.

> **Build blocker already fixed (2026-07-09).** P7 moved the LoRaWAN stack into the **private**
> `rnd-southerniot/siot-lorawan-node` (pulled via `firmware/main/idf_component.yml`). A clean
> `docker build` would fail at `idf.py build` (component manager can't clone a private repo without
> auth). The `Dockerfile` now fetches that component as a **local component** using a BuildKit secret
> (Path A) or a pre-fetched checkout (Path B) — see the runbook. **Fahim needs read access to
> `siot-lorawan-node`** (or a fine-grained PAT with read-only Contents = `COMPONENT_REPO_TOKEN`); Arif
> mints it if absent.

**Guardrails (hard):**
- **Two ChirpStack stacks** — dev `10.10.8.140` vs **production** (`crm/chirpstack.siot.solutions`,
  Fahim's k8s `10.10.8.168`). Production is **READ-ONLY** unless Arif explicitly authorizes a write.
  **Always prompt dev-vs-production before any CRM/ChirpStack action.**
- No secrets in the repo — creds via k8s secrets / `.env` (gitignored); `gitleaks` gates CI.
- No `:latest` image tags. Small reversible steps; every change has a documented rollback.
- Green CI is the merge gate to `main`; no direct pushes (PR flow enforced).

Deliverable for the morning: a reviewed, phase-gated deploy runbook ready to execute with Arif.

---

## Executable runbook (Option A — redeploy this repo's `api/`)

> Runs on **Fahim's side** (his `fahim0173` registry login + `kubectl` context for the `firmware`-ns
> cluster). Arif's Mac can't reach the registry/cluster. Every phase has a PASS gate; stop on any FAIL.
> `$SHA` = the git short-sha you are deploying (from `main` HEAD).

### Phase 0 — Prereqs + record the rollback target
```bash
# Fahim: confirm access before anything outward-facing
kubectl -n firmware get deploy firmware-service -o wide          # must succeed (context reaches cluster)
docker info >/dev/null                                            # daemon up + logged in to push fahim0173/*
git -C rak3112-rs485-node pull --ff-only origin main
SHA=$(git -C rak3112-rs485-node rev-parse --short HEAD)           # expect 3d6d338 (or later)
# Record current image = the rollback target (expect ...:b60ea9d)
kubectl -n firmware get deploy firmware-service -o jsonpath='{.spec.template.spec.containers[0].image}{"\n"}'
```
**PASS:** cluster reachable, `$SHA` resolved, current image recorded. Access to `siot-lorawan-node`
confirmed (or `COMPONENT_REPO_TOKEN` in hand).

### Phase 1 — Build the image, pinned to the sha
```bash
cd rak3112-rs485-node
export COMPONENT_REPO_TOKEN=<fine-grained PAT: read-only Contents on siot-lorawan-node>

# Path A (default): Dockerfile fetches the private component via the BuildKit secret.
DOCKER_BUILDKIT=1 docker build \
  --secret id=component_repo_token,env=COMPONENT_REPO_TOKEN \
  --build-arg FIRMWARE_TAG=$SHA \
  -t fahim0173/rak3112-firmware-service:$SHA .
```
Path B (fallback if `--secret` is unavailable) — pre-fetch the component, then a plain build:
```bash
git clone --depth 1 --branch v0.1.0 \
  "https://${COMPONENT_REPO_TOKEN}@github.com/rnd-southerniot/siot-lorawan-node.git" \
  firmware/components/siot-lorawan-node
rm -rf firmware/components/siot-lorawan-node/.git
DOCKER_BUILDKIT=1 docker build --build-arg FIRMWARE_TAG=$SHA \
  -t fahim0173/rak3112-firmware-service:$SHA .
rm -rf firmware/components/siot-lorawan-node   # keep the work tree clean (do NOT commit it)
```
**PASS:** build exits 0; the firmware stage prints `Project build complete.`

### Phase 2 — Local image smoke (best-effort, before push)
```bash
docker run -d --name fw-smoke -p 8000:8000 \
  -e API_TOKEN=smoke -e FIRMWARE_TAG=$SHA \
  -e MINIO_INTERNAL_ENDPOINT=x -e MINIO_EXTERNAL_ENDPOINT=x \
  -e MINIO_ACCESS_KEY=smoketest0 -e MINIO_SECRET_KEY=smoketest00000 -e MINIO_BUCKET=b \
  fahim0173/rak3112-firmware-service:$SHA
sleep 3
curl -sf localhost:8000/healthz
curl -sf -H "Authorization: Bearer smoke" localhost:8000/v1/sensors | grep -q 'eem400-scanned' && echo "eem400-scanned present"
docker rm -f fw-smoke
```
**PASS:** `/healthz` ok and `/v1/sensors` contains `eem400-scanned`. (MinIO-backed routes like
`/v1/build` need a real MinIO — the authoritative check is in-cluster, Phase 4. If startup requires
MinIO connectivity and the container isn't ready, skip to Phase 3–4 and verify in-cluster.)

### Phase 3 — Push + roll out (pinned sha; FIRMWARE_TAG in lockstep)
```bash
docker push fahim0173/rak3112-firmware-service:$SHA
kubectl -n firmware set image deploy/firmware-service firmware-service=fahim0173/rak3112-firmware-service:$SHA
# FIRMWARE_TAG in the env secret keys the MinIO build cache — keep it equal to the image tag:
kubectl -n firmware patch secret firmware-service-env --type merge -p "{\"stringData\":{\"FIRMWARE_TAG\":\"$SHA\"}}"
kubectl -n firmware rollout restart deploy/firmware-service      # pick up the secret change
kubectl -n firmware rollout status deploy/firmware-service --timeout=180s
```
**PASS:** rollout completes; new pod Ready.

### Phase 4 — In-cluster verification (hard gate; service is ClusterIP)
```bash
kubectl -n firmware port-forward deploy/firmware-service 8000:8000 >/tmp/pf.log 2>&1 &
PF=$!; sleep 2
TOKEN=$(kubectl -n firmware get secret firmware-service-env -o jsonpath='{.data.API_TOKEN}' | base64 -d)
curl -sf localhost:8000/healthz
curl -sf -H "Authorization: Bearer $TOKEN" localhost:8000/v1/products
curl -sf -H "Authorization: Bearer $TOKEN" localhost:8000/v1/sensors | grep -q 'eem400-scanned' && echo "catalog OK"
curl -sf -H "Authorization: Bearer $TOKEN" "localhost:8000/v1/flash-manifest?product=careflow" >/dev/null && echo "flash-manifest OK"
curl -sf -H "Authorization: Bearer $TOKEN" localhost:8000/v1/provisioning-protocol >/dev/null && echo "prov-protocol OK"
kill $PF
```
**PASS:** all endpoints 200; `/v1/sensors` lists `eem400-scanned`; flash-manifest + provisioning-protocol respond.

### Phase 5 — Pi scanner reachability
The bench Pi (`192.168.68.109`) consumes the service at `SCANNER_API_BASE` (docs say
`http://10.10.8.169:8000`). **Open item to resolve with Fahim, do not assume:** `deploy/cluster.yaml`
declares this a **ClusterIP-only PRIVATE** service (no NodePort/route), so an out-of-cluster Pi cannot
reach a bare ClusterIP. Confirm the actual path (NodePort / node-IP route / in-cluster proxy) and test:
```bash
# from the Pi:  ssh -i ~/.ssh/id_ed25519_siot_dev_m5 -o IdentitiesOnly=yes pi@192.168.68.109
curl -sf http://10.10.8.169:8000/healthz    # or the real reachable endpoint once confirmed
```
**PASS:** the Pi reaches `/healthz`. If not reachable, the deploy is container-healthy but not
Pi-complete — the scanner won't onboard until the route exists.

### Rollback (any phase FAIL after Phase 3)
```bash
kubectl -n firmware set image deploy/firmware-service firmware-service=fahim0173/rak3112-firmware-service:b60ea9d
kubectl -n firmware patch secret firmware-service-env --type merge -p '{"stringData":{"FIRMWARE_TAG":"b60ea9d"}}'
kubectl -n firmware rollout restart deploy/firmware-service
kubectl -n firmware rollout status deploy/firmware-service --timeout=180s
```

### GitOps follow-up (after a green deploy)
The repo manifest still pins the old sha. Open a small PR bumping **`k8s/deployment.yaml`** `image:` and
**`k8s/secret.example.yaml`** `FIRMWARE_TAG` to `$SHA`, so the tree reflects what's live (the live
`set image`/`patch` above mutate the cluster, not the repo).

### Guardrails during this runbook
- **No CRM/ChirpStack writes here** — this deploys only the service. Installing the fleet decoder on
  ChirpStack is a separate, later step (the Pi does it on **dev** only, `DEV_GUARD`). If any
  CRM/ChirpStack action comes up, **prompt dev (`10.10.8.140`) vs production first.**
- No `:latest`; the image tag **is** the git sha. Rollback = re-apply `b60ea9d`.

---

## Quick reference (bench state as of tonight)

- Bench Pi `192.168.68.109` (`pi@`, hostname `mcp-gateway`, aarch64, labwc/Wayland): scanner kiosk live
  + boot-persistent. App at `~/scanner/pi-scanner`, service `careflow-scanner`, kiosk via
  `~/.config/autostart/careflow-kiosk.desktop` → `~/scanner/kiosk.sh`.
- Scanner node `3c:dc:75:6f:7d:c4` on the Pi USB (`/dev/ttyACM0`); EEM400 slave-sim `3c:dc:75:6f:81:78`
  (firmware on branch `test/eem400-slave-sim`) with CN1 jumpered to the scanner node.
- Merged today: PR #16 (scanner station), #17 (eem400-scanned profile), #18 (kiosk touch controls).
