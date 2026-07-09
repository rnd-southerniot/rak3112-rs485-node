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
5. Draft a **phase-gated deploy plan** with rollback: build + push image (pinned to the git sha, **no
   `:latest`**) → `kubectl apply`/rollout → verify `/healthz`, `/v1/products`, `/v1/sensors` (should now
   list `honeywell-eem400-scanned`), `/v1/flash-manifest`, `/v1/provisioning-protocol` → confirm the Pi
   scanner station reaches it. Present it to Arif before running it.

**Guardrails (hard):**
- **Two ChirpStack stacks** — dev `10.10.8.140` vs **production** (`crm/chirpstack.siot.solutions`,
  Fahim's k8s `10.10.8.168`). Production is **READ-ONLY** unless Arif explicitly authorizes a write.
  **Always prompt dev-vs-production before any CRM/ChirpStack action.**
- No secrets in the repo — creds via k8s secrets / `.env` (gitignored); `gitleaks` gates CI.
- No `:latest` image tags. Small reversible steps; every change has a documented rollback.
- Green CI is the merge gate to `main`; no direct pushes (PR flow enforced).

Deliverable for the morning: a reviewed, phase-gated deploy runbook ready to execute with Arif.

---

## Quick reference (bench state as of tonight)

- Bench Pi `192.168.68.109` (`pi@`, hostname `mcp-gateway`, aarch64, labwc/Wayland): scanner kiosk live
  + boot-persistent. App at `~/scanner/pi-scanner`, service `careflow-scanner`, kiosk via
  `~/.config/autostart/careflow-kiosk.desktop` → `~/scanner/kiosk.sh`.
- Scanner node `3c:dc:75:6f:7d:c4` on the Pi USB (`/dev/ttyACM0`); EEM400 slave-sim `3c:dc:75:6f:81:78`
  (firmware on branch `test/eem400-slave-sim`) with CN1 jumpered to the scanner node.
- Merged today: PR #16 (scanner station), #17 (eem400-scanned profile), #18 (kiosk touch controls).
