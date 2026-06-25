#!/usr/bin/env python3
"""
provision_nvs.py — inject LoRaWAN credentials + Modbus field config into a node's NVS over the
USB-Serial-JTAG provisioning console (Phase 7d).

The node drops into its provisioning console only when it has no credentials (empty NVS 'prov'
namespace + placeholder compiled key). This tool drives that console from firmware/.env, then
restarts the node into field mode. It writes the 'prov' NVS namespace ADDITIVELY — the LoRaWAN
nonces/session in the 'lorawan' namespace are preserved (no DevNonce regression on re-join).

  python3 tools/provision_nvs.py -p /dev/cu.usbmodem1301

Credentials are read from firmware/.env (gitignored); the AppKey is never printed (redacted).
"""
import argparse
import os
import sys
import time

try:
    import serial  # pyserial (ships with the ESP-IDF python env)
except ImportError:
    sys.exit("ERROR: pyserial not found — run inside the ESP-IDF env (. ~/esp/esp-idf-v5.5.4/export.sh)")


def read_env(path):
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    return out


def parity_code(s):
    return {"N": 0, "E": 1, "O": 2}.get((s or "N").upper(), 0)


def device_code(s):
    if s is None:
        return 0
    s = s.strip().lower()
    return 1 if s in ("1", "rsfsjt", "rs-fsjt", "wind") else 0


def main():
    ap = argparse.ArgumentParser(description="Provision a rak3112-rs485-node over its console.")
    ap.add_argument("-p", "--port", required=True, help="serial port (e.g. /dev/cu.usbmodem1301)")
    ap.add_argument("--env", default=os.path.join(os.path.dirname(__file__), "..", "firmware", ".env"),
                    help="path to firmware/.env (default: ../firmware/.env)")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    env = read_env(args.env)
    deveui = env["LORAWAN_DEVEUI"].lower()
    joineui = env.get("LORAWAN_JOINEUI", "0000000000000000").lower()
    appkey = env["LORAWAN_APPKEY"].lower()
    if not (len(deveui) == 16 and len(joineui) == 16 and len(appkey) == 32):
        sys.exit("ERROR: LORAWAN_DEVEUI/JOINEUI must be 16 hex chars, APPKEY 32 — check firmware/.env")
    dev = device_code(env.get("MODBUS_DEVICE"))
    baud = int(env.get("MODBUS_BAUD", "9600"))
    par = parity_code(env.get("MODBUS_PARITY", "N"))
    unit = int(env.get("MODBUS_SLAVE_ID", "1"))
    intv = int(env.get("SAMPLE_INTERVAL_S", "60"))

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
    if "OK" not in cmd(f"prov-lorawan {deveui} {joineui} {appkey}", secret=True):
        sys.exit("ERROR: prov-lorawan did not return OK — is the node in provisioning mode "
                 "(unprovisioned / placeholder firmware)?")
    if "OK" not in cmd(f"prov-modbus {dev} {baud} {par} {unit} {intv}"):
        sys.exit("ERROR: prov-modbus did not return OK")
    cmd("prov-show")
    cmd("prov-done")
    ser.close()
    print(f"\nprovisioned DevEUI={deveui} (dev={dev} baud={baud} par={par} unit={unit} intv={intv}s)"
          " — node restarting into field mode.")


if __name__ == "__main__":
    main()
