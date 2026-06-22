---
name: mcp-gateway-health
description: Verify the SIOT MCP gateway (10.10.8.113, siot-mcp-gateway) is reachable and authenticated, and list its upstream servers and tools. Run before using gateway/firmware-knowledge tools in the rak3112-rs485-node project, or when gateway tools are missing or a call returns "Unauthorized: Invalid API key".
---

# mcp-gateway-health

Pre-flight for the `siot-mcp-gateway` MCP **proxy** (`10.10.8.113:8000`). Confirms the gateway
is up, the Bearer token is valid, and reports the upstream servers + tools so downstream work
(e.g. [rak-lorawan-knowledge](../rak-lorawan-knowledge/SKILL.md)) knows what's available.
Full context: [docs/MCP_GATEWAY.md](../../../docs/MCP_GATEWAY.md).

## Steps

1. **Health (no auth):**
   ```bash
   curl -s http://10.10.8.113:8000/health      # expect: "status":"healthy"
   ```
   Fails → service/VM down. Stop and report (do not touch the VM; global §14).

2. **Load token** (never print it):
   ```bash
   set -a; source ~/.config/siot/mcp-gateway.env; set +a
   [ ${#SIOT_MCP_GATEWAY_TOKEN} -eq 64 ] && echo "token present" || echo "TOKEN MISSING/WRONG (expect 64 chars)"
   ```
   Missing/short → the key file isn't populated. Recover the valid 64-char key from the VM
   (`/home/mcp/mcp-gateway/configs/api-key.txt`, hash-matches the active config) into the env
   file, or have the user provide it. See docs/MCP_GATEWAY.md "Token status".

3. **Auth + list proxy tools:**
   ```bash
   curl -s -X POST http://10.10.8.113:8000/mcp \
     -H "Authorization: Bearer $SIOT_MCP_GATEWAY_TOKEN" \
     -H "Content-Type: application/json" -H "Accept: application/json, text/event-stream" \
     -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
   ```
   - `"Unauthorized: Invalid API key"` → token wrong/stale; do not proceed.
   - Result with 6 tools (`gateway_info`, `list_upstream_servers`, `check_upstream_health`,
     `discover_upstream_tools`, `call_upstream_tool`, `reload_configuration`) → usable.

4. **In-session** (after Claude Code restart with the token set): the 6 proxy tools are
   deferred MCP tools — surface them with `ToolSearch` (`select:call_upstream_tool` etc.).
   Optionally call `list_upstream_servers` / `check_upstream_health` / `discover_upstream_tools`
   to confirm `firmware-knowledge` (and others) are healthy.

## Output

`GATEWAY OK — 6 proxy tools; upstreams: firmware-knowledge, local-pi, careflow-docker, navbot-knowledge`
or `GATEWAY BLOCKED — <reason>` naming the exact failing step.

## Notes

- This gateway is a **proxy**; upstream tools are reached via `call_upstream_tool`
  (`server_name`, `tool_name`, `arguments`), not exposed directly.
- `10.10.8.113` (this gateway) ≠ Pi gateway `192.168.20.150`. Token rotation + recovery in
  [docs/MCP_GATEWAY.md](../../../docs/MCP_GATEWAY.md).
