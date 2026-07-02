# Cluster CHIRPSTACK_API_TOKEN is NOT a usable ChirpStack API key

**Category:** gotcha
**Tags:** chirpstack, lorawan, provisioning, grpc-web, credentials, cluster
**Date:** 2026-06-29

## Context
The `lorawan-provision-device` / `lorawan-deprovision` skills authenticate to production
ChirpStack (`https://chirpstack.siot.solutions`) with `CS_API_TOKEN` (preferred) or
`CS_ADMIN_USER`/`CS_ADMIN_PASS` (fallback). A natural shortcut is to pull the token from the
RKE2 cluster via `cluster-connect` (`/root/kuber-installs/chirpstack/secrets.env`). **That token
does not work as a ChirpStack Bearer key.**

## Detail
`chirpstack/secrets.env` exposes two values:
- `CHIRPSTACK_JWT_SECRET` — the server's JWT signing secret (used to sign/verify keys internally)
- `CHIRPSTACK_API_TOKEN` — a **64-char hex** opaque shared secret consumed by another component

ChirpStack v4 API keys are **JWTs** (`eyJ...`, ~150+ chars). The 64-hex value is not a JWT, so as a
gRPC-web `authorization: Bearer` it is rejected.

**Failure signature (the confusing part):** every gRPC-web call returns **HTTP 200, content-type
`application/grpc-web+proto`, but a 0-byte body and no trailer frame** — NOT a clean
`grpc-status:16 UNAUTHENTICATED` trailer. So `provision.py` reports the downstream symptom
`device not present after Create` and `TenantService/List` returns zero tenants. Easy to misread as
network/scope. Quick disambiguator that classifies the token without printing it:
```bash
printf '%s' "$TOKEN" | awk '{print "len="length($0), "dots="gsub(/\./,"."), "hex="($0~/^[0-9a-fA-F]+$/)}'
# len=64 dots=0 hex=1  => opaque secret, NOT a ChirpStack API JWT
```

## Usage
Do NOT use the cluster `CHIRPSTACK_API_TOKEN` for provisioning. Use one of:
1. **Admin login fallback** (works, verified 2026-06-29): `CS_ADMIN_USER=admin CS_ADMIN_PASS=admin`
   → `cs_grpcweb.auth()` calls `InternalService/Login` and mints a real JWT. (Bench/default creds;
   ChirpStack admin login lives in its Postgres, not in any env file.)
2. A real API key minted in the UI: Tenant *Southern IoT* → API keys (a long `eyJ...` JWT).

If admin login ALSO returned an empty body at the `login` step, that would indicate a real
transport/Cloudflare problem — but it succeeded, proving transport is fine and the empty bodies were
purely the bad-token case.

## Related
- `.claude/skills/lorawan-provision-device/provision.py`, `.claude/skills/_shared/cs_grpcweb.py`
  (`call()` discards the gRPC trailer — that's why bad-auth status is invisible)
- `.planning/knowledge/devops/demo-provision-simulated-mfm384.md`
- `cluster-connect` skill (reads `/root/kuber-installs/chirpstack/secrets.env`)
