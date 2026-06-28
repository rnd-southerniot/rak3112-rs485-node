# _shared — helpers for the lorawan-* skills

[`cs_grpcweb.py`](cs_grpcweb.py) — a minimal ChirpStack **gRPC-web** client. The production cluster
(`chirpstack.siot.solutions`) exposes only gRPC-web behind Cloudflare (no REST gateway; native gRPC is
blocked), so every call is a framed `POST /api.<Service>/<Method>` with a browser `User-Agent`.

- `call(path, req, resp_cls, jwt=None)` — send one protobuf request, return the parsed response (or
  `None` if the server replied with only a gRPC trailer, e.g. `ALREADY_EXISTS` / `NOT_FOUND` /
  `UNAUTHENTICATED`). Transport failures (Cloudflare 403, unreachable host) raise `urllib` errors,
  which the skill scripts catch and report.
- `auth()` — returns `CS_API_TOKEN` if set (preferred), else logs in with `CS_ADMIN_USER`/`CS_ADMIN_PASS`.

Used by [`lorawan-provision-device`](../lorawan-provision-device/SKILL.md),
[`lorawan-verify-join`](../lorawan-verify-join/SKILL.md),
[`lorawan-deprovision`](../lorawan-deprovision/SKILL.md). Full guide:
[docs/LORAWAN_PROVISIONING.md](../../../docs/LORAWAN_PROVISIONING.md).

## Setup (one-time)

```bash
export CS_BASE=https://chirpstack.siot.solutions
export UA="Mozilla/5.0"
export CS_API_TOKEN=…                 # never commit
uv venv .v && . .v/bin/activate && uv pip install chirpstack-api grpcio
```
