#!/usr/bin/env python3
"""
provision_node.py — provision this rak3112-rs485-node end-to-end THROUGH the SCOMM CRM
onboarding workflow (system of record), which registers the device in ChirpStack (AS923).

Contract (siot-crm-review): login -> find/create LoRaWAN product -> create onboarding task ->
walk workflow states -> submit DevEUI/AppKey at HARDWARE_PREPARED_COMPLETE -> complete
pre-installation checklist -> READY_FOR_INSTALLATION (triggers ChirpStack registration) ->
read back -> verify device in ChirpStack.

Credentials:
  CRM_BASE / CRM_EMAIL / CRM_PASSWORD   from env (source ~/.config/siot/rak3112-crm.env)
  LORAWAN_DEVEUI / LORAWAN_APPKEY       from firmware/.env (gitignored)

The AppKey is a SECRET: it is read from firmware/.env, sent only to the CRM, and never
printed (redacted in all logs) or committed.
"""
import argparse
import json
import os
import sys
import time
import urllib.request
import urllib.error

CRM_BASE = os.environ["CRM_BASE"].rstrip("/")
CRM_EMAIL = os.environ["CRM_EMAIL"]
CRM_PASSWORD = os.environ["CRM_PASSWORD"]

# Per-product CRM identity. The onboarding workflow is identical across products; only the LoRaWAN
# product record + device serial prefix differ. Default is careflow (unchanged from the single-product
# tool). Mirrors tools/provision_template*.json planeA_crm.
PRODUCTS = {
    "careflow": {
        "code": "RAK3112-RS485-AS923",
        "name": "RAK3112 RS-485 ⇄ LoRaWAN AS923 Node",
        "description": "RS-485 ⇄ LoRaWAN AS923 industrial sensor/gateway node",
        "serial_prefix": "RAK3112-RS485-",
    },
    "senseflow": {
        "code": "SENSEFLOW-EINK-AS923",
        "name": "Senseflow e-ink / I²C ⇄ LoRaWAN AS923 Node",
        "description": "I²C environmental sensor e-ink display node (BME280/SGP40/SHTC3) over LoRaWAN AS923",
        "serial_prefix": "SENSEFLOW-EINK-",
    },
}
HARDWARE_NAME = "ESP32-WROOM-32"  # reuse a valid catalog entry; true identity carried in serial/DevEUI


def _env_file(path):
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    return out


def api(method, path, token=None, body=None):
    url = CRM_BASE + path
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


def main():
    ap = argparse.ArgumentParser(description="Register a rak3112 node in the SCOMM CRM → ChirpStack.")
    ap.add_argument("--product", default="careflow", choices=sorted(PRODUCTS),
                    help="which product's CRM identity to register under (default: careflow)")
    args = ap.parse_args()
    P = PRODUCTS[args.product]
    product_code, product_name = P["code"], P["name"]

    fw_env = _env_file(os.path.join(os.path.dirname(__file__), "..", "firmware", ".env"))
    deveui = fw_env["LORAWAN_DEVEUI"].lower()
    appkey = fw_env["LORAWAN_APPKEY"].lower()
    serial = P["serial_prefix"] + deveui[-6:].upper()
    print(f"[*] product={args.product}  node: DevEUI={deveui}  serial={serial}  "
          f"AppKey=<redacted {len(appkey)} hex>")

    # 1. login
    st, r = api("POST", "/auth/login", body={"email": CRM_EMAIL, "password": CRM_PASSWORD})
    token = r.get("access_token")
    assert token, f"login failed: {st} {r}"
    print(f"[1] login OK as {CRM_EMAIL} (role {r.get('user', {}).get('role', {}).get('name')})")

    # 2. find-or-create LoRaWAN product
    _, r = api("GET", "/products", token)
    prod = next((p for p in items(r) if p.get("code") == product_code), None)
    if not prod:
        st, prod = api("POST", "/products", token, {
            "name": product_name, "code": product_code,
            "description": P["description"],
            "isLorawanProduct": True, "lorawanRegion": "AS923",
        })
        assert prod.get("id"), f"create product failed: {st} {prod}"
        print(f"[2] product CREATED {product_code} id={prod['id']}")
    else:
        print(f"[2] product exists {product_code} id={prod['id']}")
    product_id = prod["id"]

    # 2b. ensure an SOP template exists (prereq for task creation). The POST needs productId
    # in the BODY and each step needs id/title/description/order (CreateSOPTemplateDto).
    st, sop = api("GET", f"/products/{product_id}/sop-template", token)
    if st >= 400 or not (isinstance(sop, dict) and sop.get("id")):
        st2, r2 = api("POST", f"/products/{product_id}/sop-template", token, {
            "productId": product_id, "version": 1,
            "steps": [{"id": "step-1", "title": "Bench bring-up",
                       "description": "AS923 OTAA bench bring-up", "order": 1}]})
        assert st2 < 400 and r2.get("id"), f"  SOP create failed: {st2} {r2.get('_error', r2)}"
        print("[2b] SOP template created")
    else:
        print("[2b] SOP template present")

    # 3. hardware-catalog id
    _, r = api("GET", "/hardware-catalog", token)
    hw = next((h for h in items(r) if h.get("name") == HARDWARE_NAME), None)
    assert hw, f"hardware '{HARDWARE_NAME}' not in catalog"
    hardware_id = hw["id"]
    print(f"[3] hardware '{HARDWARE_NAME}' id={hardware_id}")

    # 4. create onboarding task
    st, task = api("POST", "/workflow/tasks", token, {
        "clientName": "Southern IoT — Bench (rak3112-rs485-node)",
        "clientEmail": "rnd@southerniot.net",
        "clientPhone": "+8800000000000",
        "clientAddress": "R&D Bench, Dhaka, Bangladesh",
        "contactPerson": "Arif",
        "productId": product_id,
    })
    task_id = task.get("id")
    assert task_id, f"create task failed: {st} {task}"
    print(f"[4] task CREATED id={task_id} status={task.get('currentStatus', task.get('status'))}")

    # 5. walk the workflow
    def advance(status, body):
        st, r = api("PUT", f"/workflow/tasks/{task_id}/status/{status}", token, body)
        cur = r.get("currentStatus") or r.get("status")
        assert st < 400 and cur == status, f"  -> {status} FAILED: {st} {r.get('_error', r)}"
        print(f"    -> {status} OK")

    print("[5] advancing workflow:")
    advance("SCHEDULED_VISIT", {"scheduledDate": "2026-06-25T10:00:00Z"})
    advance("REQUIREMENTS_COMPLETE", {"reportData": {
        "siteConditions": "Bench", "signalStrength": "-60 dBm",
        "powerAvailability": "USB", "installationLocation": "R&D bench", "notes": "AS923 OTAA bring-up"}})
    advance("HARDWARE_PROCUREMENT_COMPLETE", {"hardwareList": [
        {"hardwareId": hardware_id, "quantity": 1, "notes": "RAK3112 RS-485 node"}]})
    advance("HARDWARE_PREPARED_COMPLETE", {"deviceList": [{
        "hardwareId": hardware_id, "deviceSerial": serial, "firmwareVersion": "phase5-bringup",
        "devEui": deveui, "appKey": appkey, "notes": "AS923 OTAA bench bring-up"}]})

    # 6. pre-installation checklist (all true)
    st, r = api("PUT", f"/workflow/tasks/{task_id}/pre-installation-checklist", token, {
        "devicesTestComplete": True, "firmwareLoaded": True, "qrCodesPrinted": True,
        "clientConfirmedDate": True, "accessArranged": True, "contactAvailable": True,
        "installationGuide": True, "networkConfig": True, "credentialsReady": True})
    assert st < 400, f"  checklist FAILED: {st} {r.get('_error', r)}"
    print("[6] pre-installation checklist complete")

    # 7. READY_FOR_INSTALLATION.
    # NOTE: this transition takes an EMPTY body — the status DTO whitelist rejects
    # latitude/longitude (and any extra field) with a 400 ValidationError.
    # D-11 (Phase 2 design): ChirpStack registration NO LONGER fires here.
    # It moves to the factory QR scan (POST /provisioning/scan), which is triggered
    # on-site when the engineer scans the device QR. This script covers the CRM
    # workflow-walk leg only (Plane-B: NVS credential injection via prov-* console
    # is handled separately by the WebSerial flasher). The Plane-A ChirpStack
    # auto-register call that was previously at this step is REMOVED per D-11.
    advance("READY_FOR_INSTALLATION", {})
    print("[7] READY_FOR_INSTALLATION (task ready for on-site installation)")
    print("    NOTE: ChirpStack registration deferred to factory QR scan (D-11).")
    print("          Use the WebSerial flasher to write NVS creds, then scan the")
    print("          printed QR on-site to trigger ChirpStack device registration.")

    print("\n=== SUMMARY ===")
    print(f"  product   : {product_code} ({product_id})")
    print(f"  task      : {task_id}")
    print(f"  device    : {serial}  DevEUI={deveui}")
    print(f"  task status: READY_FOR_INSTALLATION")
    print(f"  ChirpStack : NOT YET registered (pending factory QR scan per D-11)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
