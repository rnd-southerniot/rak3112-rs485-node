#!/usr/bin/env python3
"""provision_nvs.py — inject LoRaWAN credentials + Modbus field config into a node's NVS over the
USB-Serial-JTAG provisioning console (Phase 7d).

The node drops into its provisioning console only when it has no credentials (empty NVS 'prov'
namespace + placeholder compiled key). This tool drives that console from the values resolved
through tools/provision_template.json (planeB_nvs) — identity from firmware/.env, Modbus config
from firmware/.env — then restarts the node into field mode. It writes the 'prov' NVS namespace
ADDITIVELY — the LoRaWAN nonces/session in the 'lorawan' namespace are preserved (no DevNonce
regression on re-join).

  python3 tools/provision_nvs.py -p /dev/cu.usbmodem1301

Credentials are read from firmware/.env (gitignored); the AppKey is never printed (redacted).
Full contract: docs/PROVISIONING_API_CONTRACT.md
"""
import argparse
import os
import sys
import time

import provtemplate as pt

try:
    import serial  # pyserial (ships with the ESP-IDF python env)
except ImportError:
    sys.exit("ERROR: pyserial not found — run inside the ESP-IDF env (. ~/esp/esp-idf-v5.5.4/export.sh)")


def parity_code(s):
    return {"N": 0, "E": 1, "O": 2}.get((s or "N").upper(), 0)


def device_code(s):
    if s is None:
        return 0
    s = str(s).strip().lower()
    return 1 if s in ("1", "rsfsjt", "rs-fsjt", "wind") else 0


def main():
    ap = argparse.ArgumentParser(description="Provision a rak3112-rs485-node over its console.")
    ap.add_argument("-p", "--port", required=True, help="serial port (e.g. /dev/cu.usbmodem1301)")
    ap.add_argument("--env", default=pt.DEFAULT_FW_ENV,
                    help="path to firmware/.env (default: ../firmware/.env)")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    tmpl = pt.load_template()
    env = pt.build_env(args.env)
    B = tmpl["planeB_nvs"]

    idv = pt.resolve_identity(tmpl, env)
    pt.validate_identity(idv)
    deveui, joineui, appkey = idv["deveui"], idv["joineui"], idv["appkey"]

    mb = B["modbus"]
    dev = device_code(pt.resolve_field(mb["dev"], env))
    baud = int(pt.resolve_field(mb["baud"], env))
    par = parity_code(pt.resolve_field(mb["par"], env))
    unit = int(pt.resolve_field(mb["unit"], env))
    intv = int(pt.resolve_field(mb["intv"], env))

    # command names come from the template (e.g. "prov-lorawan <...>") — take the verb only
    cmd_lorawan = B["lorawan"]["command"].split()[0]
    cmd_modbus = B["modbus"]["command"].split()[0]
    cmd_commit = B["commit"].split()[0]

    ser = serial.Serial(args.port, args.baud, timeout=0.4)

    def cmd(line, secret=False):
        ser.reset_input_buffer()
        ser.write((line + "\n").encode())
        time.sleep(1.0)
        out = ser.read(8192).decode(errors="replace")
        shown = (line.rsplit(" ", 1)[0] + " <redacted>") if secret else line
        print(f">> {shown}")
        for ln in out.splitlines():
            t = ln.strip()
            if not t or t.startswith("prov>"):
                continue
            # never echo a secret command line back to the operator's terminal
            if secret and ("OK" not in t and "ERR" not in t and "prov:" not in t):
                continue
            print(f"   {t}")
        return out

    cmd("")  # nudge a prompt
    cmd("prov-show")
    if "OK" not in cmd(f"{cmd_lorawan} {deveui} {joineui} {appkey}", secret=True):
        sys.exit(f"ERROR: {cmd_lorawan} did not return OK — is the node in provisioning mode "
                 "(unprovisioned / placeholder firmware)?")
    if "OK" not in cmd(f"{cmd_modbus} {dev} {baud} {par} {unit} {intv}"):
        sys.exit(f"ERROR: {cmd_modbus} did not return OK")
    cmd("prov-show")
    cmd(cmd_commit)
    ser.close()
    print(f"\nprovisioned DevEUI={deveui} (dev={dev} baud={baud} par={par} unit={unit} intv={intv}s)"
          " — node restarting into field mode.")


if __name__ == "__main__":
    main()
