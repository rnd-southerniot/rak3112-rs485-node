#!/usr/bin/env python3
"""provision_node.py — provision this rak3112-rs485-node end-to-end THROUGH the SCOMM CRM
onboarding workflow (system of record), which registers the device in ChirpStack (AS923).

Contract (siot-crm-review): login -> find/create LoRaWAN product -> create onboarding task ->
walk workflow states -> submit DevEUI/AppKey at HARDWARE_PREPARED_COMPLETE -> complete
pre-installation checklist -> READY_FOR_INSTALLATION (triggers ChirpStack registration) ->
read back -> verify device in ChirpStack.

All values (product code, hardware name, task metadata, workflow bodies, identity refs) come
from tools/provision_template.json (planeA_crm) via provtemplate.py. Secrets stay as ${ENV:...}
references resolved at run time:
  CRM_BASE / CRM_EMAIL / CRM_PASSWORD   from env (source ~/.config/siot/rak3112-crm.env)
  LORAWAN_DEVEUI / LORAWAN_APPKEY       from firmware/.env (gitignored)

The AppKey is a SECRET: it is read from firmware/.env, sent only to the CRM, and never
printed (redacted in all logs) or committed. Full contract: docs/PROVISIONING_API_CONTRACT.md
"""
import argparse
import copy
import json
import os
import sys
import time
import urllib.error
import urllib.request

import provtemplate as pt

_BASE = None  # set from CRM_BASE in main() on the live path; not needed for --dry-run


def api(method, path, token=None, body=None):
    url = _BASE + path
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            txt = r.read().decode()
            return r.status, (json.loads(txt) if txt else {})
    except urllib.error.HTTPError as e:
        return e.code, {"_error": e.read().decode()[:400]}


def items(resp):
    return resp.get("data", resp) if isinstance(resp, dict) else resp


def _redact(obj):
    """Deep-copy with any appKey/appkey value masked — for printing request bodies."""
    if isinstance(obj, dict):
        return {k: ("<redacted 32 hex>" if k.lower() == "appkey" else _redact(v))
                for k, v in obj.items()}
    if isinstance(obj, list):
        return [_redact(v) for v in obj]
    return obj


def _show(label, body):
    print(f"\n# {label}\n" + json.dumps(_redact(body), indent=2, ensure_ascii=False))


def dry_run(tmpl, env, idv):
    """Resolve and print every CRM request body that a live run would send, with the AppKey
    redacted and the catalog-resolved hardware_id shown as a placeholder. No network calls."""
    A = tmpl["planeA_crm"]
    hw_id = "<hardware_id@catalog>"
    ctx = {**idv, "resolved": {"hardware_id": hw_id}}
    print("=== DRY RUN — no CRM calls; bodies below are what WOULD be sent ===")
    print(f"[*] node: DevEUI={idv['deveui']}  serial={idv['serial']}  "
          f"AppKey=<redacted {len(idv['appkey'])} hex>")
    print(f"[*] product find-or-create by code: {A['product']['code']}")
    print(f"[*] hardware-catalog lookup by name: {A['hardware']['name']}")

    _show("POST /products  (only when product absent)",
          pt.strip_keys(A["product"], "findOrCreateBy", "source"))
    _show("POST /products/{id}/sop-template",
          {"productId": "<product_id>", "version": A["sopTemplate"]["version"],
           "steps": A["sopTemplate"]["steps"]})
    task = pt.strip_keys(A["task"], "source")
    task["productId"] = "<product_id>"
    _show("POST /workflow/tasks", task)
    for step in A["workflow"]:
        _show(f"PUT /workflow/tasks/{{id}}/status/{step['status']}",
              pt.interpolate(step["body"], ctx, env))
    _show("PUT /workflow/tasks/{id}/pre-installation-checklist",
          pt.strip_keys(A["preInstallationChecklist"], "source"))
    print("\n=== end dry run (validate the above, then run without --dry-run) ===")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Provision a rak3112-rs485-node via the SCOMM CRM.")
    ap.add_argument("--dry-run", action="store_true",
                    help="resolve + print request bodies (AppKey redacted); make no CRM calls")
    ap.add_argument("--env", default=pt.DEFAULT_FW_ENV,
                    help="path to firmware/.env (default: ../firmware/.env)")
    args = ap.parse_args()

    tmpl = pt.load_template()
    env = pt.build_env(args.env)
    A = tmpl["planeA_crm"]

    idv = pt.resolve_identity(tmpl, env)
    pt.validate_identity(idv)
    deveui, appkey, serial = idv["deveui"], idv["appkey"], idv["serial"]

    if args.dry_run:
        return dry_run(tmpl, env, idv)

    global _BASE
    _BASE = os.environ["CRM_BASE"].rstrip("/")
    crm_email = os.environ["CRM_EMAIL"]
    crm_password = os.environ["CRM_PASSWORD"]
    print(f"[*] node: DevEUI={deveui}  serial={serial}  AppKey=<redacted {len(appkey)} hex>")

    # 1. login
    st, r = api("POST", "/auth/login", body={"email": crm_email, "password": crm_password})
    token = r.get("access_token")
    assert token, f"login failed: {st} {r}"
    print(f"[1] login OK as {crm_email} (role {r.get('user', {}).get('role', {}).get('name')})")

    # 2. find-or-create LoRaWAN product (values from template planeA_crm.product)
    prod_t = A["product"]
    product_code = prod_t["code"]
    _, r = api("GET", "/products", token)
    prod = next((p for p in items(r) if p.get("code") == product_code), None)
    if not prod:
        body = pt.strip_keys(prod_t, "findOrCreateBy", "source")
        st, prod = api("POST", "/products", token, body)
        assert prod.get("id"), f"create product failed: {st} {prod}"
        print(f"[2] product CREATED {product_code} id={prod['id']}")
    else:
        print(f"[2] product exists {product_code} id={prod['id']}")
    product_id = prod["id"]

    # 2b. ensure an SOP template exists (prereq for task creation). productId in the BODY;
    # each step needs id/title/description/order (CreateSOPTemplateDto).
    sop_t = A["sopTemplate"]
    st, sop = api("GET", f"/products/{product_id}/sop-template", token)
    if st >= 400 or not (isinstance(sop, dict) and sop.get("id")):
        st2, r2 = api("POST", f"/products/{product_id}/sop-template", token, {
            "productId": product_id, "version": sop_t["version"], "steps": sop_t["steps"]})
        assert st2 < 400 and r2.get("id"), f"  SOP create failed: {st2} {r2.get('_error', r2)}"
        print("[2b] SOP template created")
    else:
        print("[2b] SOP template present")

    # 3. hardware-catalog id (lookup by template name)
    hardware_name = A["hardware"]["name"]
    _, r = api("GET", "/hardware-catalog", token)
    hw = next((h for h in items(r) if h.get("name") == hardware_name), None)
    assert hw, f"hardware '{hardware_name}' not in catalog"
    hardware_id = hw["id"]
    print(f"[3] hardware '{hardware_name}' id={hardware_id}")

    # 4. create onboarding task (client metadata from template + runtime productId)
    task_body = pt.strip_keys(A["task"], "source")
    task_body["productId"] = product_id
    st, task = api("POST", "/workflow/tasks", token, task_body)
    task_id = task.get("id")
    assert task_id, f"create task failed: {st} {task}"
    print(f"[4] task CREATED id={task_id} status={task.get('currentStatus', task.get('status'))}")

    # 5. walk the workflow — bodies come from the template, interpolated with identity +
    # the runtime-resolved hardware_id. DevEUI/AppKey enter at HARDWARE_PREPARED_COMPLETE.
    ctx = {**idv, "resolved": {"hardware_id": hardware_id}}

    def advance(status, body):
        st, r = api("PUT", f"/workflow/tasks/{task_id}/status/{status}", token, body)
        cur = r.get("currentStatus") or r.get("status")
        assert st < 400 and cur == status, f"  -> {status} FAILED: {st} {r.get('_error', r)}"
        print(f"    -> {status} OK")

    print("[5] advancing workflow:")
    for step in A["workflow"]:
        advance(step["status"], pt.interpolate(step["body"], ctx, env))

    # 6. pre-installation checklist (all flags from template, sans metadata)
    st, r = api("PUT", f"/workflow/tasks/{task_id}/pre-installation-checklist", token,
                pt.strip_keys(A["preInstallationChecklist"], "source"))
    assert st < 400, f"  checklist FAILED: {st} {r.get('_error', r)}"
    print("[6] pre-installation checklist complete")
    print("[7] READY_FOR_INSTALLATION advanced above (ChirpStack registration triggered, async)")

    # 8. poll task for provisioning status, then verify in ChirpStack
    status = None
    for _ in range(12):
        time.sleep(2)
        _, t = api("GET", f"/workflow/tasks/{task_id}", token)
        dps = t.get("deviceProvisionings", [])
        dp = next((d for d in dps if d.get("devEui", "").lower() == deveui), dps[0] if dps else {})
        status = dp.get("lorawanProvisioningStatus")
        err = dp.get("lorawanProvisioningError")
        if status in ("COMPLETED", "FAILED"):
            print(f"[8] CRM lorawanProvisioningStatus={status}" + (f" error={err}" if err else ""))
            break
        print(f"    ...provisioning status={status}")

    st, cs = api("GET", f"/chirpstack/device/{deveui}", token)
    found = cs.get("found")
    print(f"[9] ChirpStack device {deveui}: found={found}")
    if found:
        d = cs.get("device", {})
        print(f"    name={d.get('name')} appId={d.get('applicationId')} profile={d.get('deviceProfileId')}")

    print("\n=== SUMMARY ===")
    print(f"  product   : {product_code} ({product_id})")
    print(f"  task      : {task_id}")
    print(f"  device    : {serial}  DevEUI={deveui}")
    print(f"  CRM status: {status}")
    print(f"  ChirpStack: {'registered' if found else 'NOT FOUND'}")
    return 0 if found else 2


if __name__ == "__main__":
    sys.exit(main())
