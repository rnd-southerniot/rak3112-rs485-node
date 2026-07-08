# Pi 5 kiosk deployment

Boots the Raspberry Pi 5 into the scanner app. Two systemd units: the FastAPI service and a
Chromium kiosk pointed at `localhost:8080`.

## Install (on the Pi)

```bash
# 1. clone + venv
cd ~ && git clone <repo> rak3112-rs485-node
cd rak3112-rs485-node/pi-scanner
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt

# 2. edit the service env (firmware-service URL, ChirpStack dev base, token)
sudoedit /etc/systemd/system/careflow-scanner.service   # copy from deploy/, adjust paths/IPs

# 3. install + enable both units
sudo cp deploy/careflow-scanner.service deploy/careflow-kiosk.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now careflow-scanner.service
sudo systemctl enable --now careflow-kiosk.service     # needs a desktop session (DISPLAY=:0)
```

The operator sees only the wizard; no terminal is needed for normal use. `journalctl -u
careflow-scanner -f` for logs.

## Notes

- `careflow-scanner.service` binds `127.0.0.1:8080` only (the kiosk is same-host). Don't expose it.
- `CS_BASE` must be the **dev** ChirpStack (`10.10.8.140`); the app's `DEV_GUARD` refuses any other
  base, so a misconfiguration fails safe rather than writing production.
- The Careflow node connects over USB; the app auto-detects the port (or set `SCANNER_NODE_PORT`).
