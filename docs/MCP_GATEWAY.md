# MCP Gateway — `siot-mcp-gateway`

> Project knowledge for the SIOT MCP gateway this firmware repo attaches to.
> Canonical infra reference: global `~/.claude/CLAUDE.md` §11. The gateway VM is read-only
> reference infra — do not reconfigure it from firmware work (global §14).

---

## What it actually is

`siot-mcp-gateway` (`10.10.8.113:8000`, **mcp-gateway v1.0.0**, uvicorn) is an **MCP proxy**.
It runs as `mcp-proxy.service` (systemd) on host `mcp-gateway` and fronts several upstream MCP
servers under one authenticated endpoint. It does **not** itself expose ChirpStack/device-CRM
tools — those live on the CRM (`10.10.8.140`) and are not reachable through this gateway.

### Gateway (proxy) tools — what `tools/list` returns

| Tool | Purpose |
|---|---|
| `gateway_info` | Gateway name/version/status |
| `list_upstream_servers` | The configured upstream servers (name, url, prefix, enabled) |
| `check_upstream_health` | Health of each upstream |
| `discover_upstream_tools` | All tools exposed by each upstream |
| `call_upstream_tool` | **Invoke an upstream tool** — args: `server_name`, `tool_name`, `arguments{}` |
| `reload_configuration` | Reload proxy config (admin) |

Upstream tools are **not** surfaced directly — you reach them via `call_upstream_tool`.

### Upstream servers

| Server (`server_name`) | Prefix | Description | Relevance to this repo |
|---|---|---|---|
| **`firmware-knowledge`** | `fw` | **RAK firmware + LoRaWAN auto-provisioning knowledge** (OTAA AS923-1, ChirpStack v4, FUOTA, bring-up). | ✅ **The fit.** Phase 5 join/uplink + provisioning. |
| `local-pi` | `pi` | Raspberry Pi system tools (stats, services, docker, logs, run_command). | Infra ops only. |
| `careflow-docker` | `docker` | Careflow Docker stack on `10.10.8.114` (33 docker tools). | Unrelated. |
| `navbot-knowledge` | `nav` | claude-navbot robot project knowledge. | Unrelated. |

### `firmware-knowledge` tools (call via `call_upstream_tool`, `server_name:"firmware-knowledge"`)

`list_docs` · `get_doc` · `search` · `get_provisioning_guide` · `get_sop_guide` ·
`get_gate_status` · `get_pin_map`

Its doc set is RAK4630/nRF52840-centric but the **LoRaWAN OTAA AS923-1 + ChirpStack +
auto-provisioning** material transfers directly to this RAK3112/ESP32-S3 node. Notably
`get_provisioning_guide` returns a workflow validated end-to-end with a **real OTAA join +
confirmed uplink** — directly useful for the Phase 5 uplink debug.

---

## Why this project attaches it

Phase 5 is LoRaWAN OTAA AS923 join + uplink against the CRM ChirpStack (`10.10.8.140`). The
`firmware-knowledge` upstream is the proven, queryable knowledge base for exactly that
workflow. This is the one MCP server that fits the project; it is pinned at **project scope**
via `.mcp.json`. The generic global servers (filesystem/git/fetch/sequential-thinking/memory)
stay global.

---

## Endpoint & auth

| Item | Value |
|---|---|
| Health | `GET /health` (open) |
| Dashboard | `GET /` (open, HTML) |
| MCP endpoint | `POST /mcp` — Streamable HTTP, JSON-RPC 2.0, **Bearer required** |
| Auth | `Authorization: Bearer ${SIOT_MCP_GATEWAY_TOKEN}` — SHA-256 hashed server-side |

The Bearer token is loaded from a per-machine file, never committed:

```bash
set -a; source ~/.config/siot/mcp-gateway.env; set +a   # exports SIOT_MCP_GATEWAY_TOKEN
```

`.mcp.json` references it via `${SIOT_MCP_GATEWAY_TOKEN}` env indirection (Guardrail §3 #4) —
no secret enters the tree. Run Claude Code from a shell that sourced it; otherwise the server
just fails to connect (harmless, no gateway tools available).

> ✅ **Token status (2026-06-22):** sorted. The valid 64-char key was recovered from the VM
> (`/home/mcp/mcp-gateway/configs/api-key.txt`, hash-verified against the active config) and
> written to `~/.config/siot/mcp-gateway.env` (perms 600). `tools/list` returns the 6 proxy
> tools. The server stores only a **SHA-256 hash** of the key — the plaintext cannot be read
> back from the running config, only from that key file.

---

## Verify connectivity

```bash
curl -s http://10.10.8.113:8000/health                      # "status":"healthy"
set -a; source ~/.config/siot/mcp-gateway.env; set +a
curl -s -X POST http://10.10.8.113:8000/mcp \
  -H "Authorization: Bearer $SIOT_MCP_GATEWAY_TOKEN" \
  -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'   # 6 tools, not "Invalid API key"
```

The `mcp-gateway-health` skill automates this.

## Calling an upstream tool (the firmware KB)

In-session, the 6 proxy tools are deferred MCP tools — surface `call_upstream_tool` via
ToolSearch (`select:call_upstream_tool`), then:

```jsonc
// e.g. fetch the proven provisioning workflow
call_upstream_tool(
  server_name = "firmware-knowledge",
  tool_name   = "get_provisioning_guide",
  arguments   = {}
)
// or browse / search
call_upstream_tool("firmware-knowledge", "list_docs", {})
call_upstream_tool("firmware-knowledge", "get_doc", { "path": "docs/04-lorawan-chirpstack-fuota-plan.md" })
```

---

## Project skills

| Skill | Use |
|---|---|
| `mcp-gateway-health` | Load token, verify `/health` + auth, list upstreams + tools. Pre-flight. |
| `rak-lorawan-knowledge` | Query `firmware-knowledge` for RAK LoRaWAN AS923 provisioning / OTAA / uplink guidance (Phase 5). |

## Safety

- Read/knowledge use is the default. `reload_configuration`, `manage_service`, `run_command`
  (local-pi), and the `careflow-docker` write tools mutate infra → **off-limits** without a
  phase plan + rollback (global §14).
- Gateway VM `10.10.8.113` and CRM `10.10.8.140` are read-only reference infra for this repo.
