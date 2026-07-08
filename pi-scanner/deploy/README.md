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

## On-screen keyboard (touch, no physical keyboard)

The kiosk is touch-only, so it needs an on-screen keyboard visible over the app. This relies on
Raspberry Pi OS's default **squeekboard** plus two bits of session config:

- **Chromium runs as a borderless `--app` window, maximized — NOT `--kiosk`.** Exclusive fullscreen
  (`--kiosk`) stacks above every wlr-layer-shell layer, so *no* on-screen keyboard can appear over
  it (confirmed for both the `top` and `overlay` layers under labwc). A normal maximized window
  stays below those layers, so squeekboard shows above it (and its exclusive zone reflows the page).
  `deploy/kiosk.sh` already launches it this way.

- **labwc must maximize + de-decorate that window.** Add to `~/.config/labwc/rc.xml` (inside
  `<openbox_config>`; note XML comments must not contain `--`):

  ```xml
  <windowRules>
    <windowRule identifier="*" serverDecoration="no" skipTaskbar="yes" skipWindowSwitcher="yes">
      <action name="Maximize"/>
    </windowRule>
  </windowRules>
  ```

- **Hide the desktop panel** so the window fills the screen — in `~/.config/wf-panel-pi.ini` under
  `[panel]` add `autohide=true` (and `autohide_duration=300`).

Reload after editing: `labwc --reconfigure` and restart `wf-panel-pi`.

The keyboard is driven two ways: squeekboard auto-shows when a text field is focused (Wayland
input-method), and the header ⌨ button toggles it via `POST /api/kiosk/keyboard` → the backend calls
`busctl --user ... sm.puri.OSK0 SetVisible` (hence the session-bus env in `careflow-scanner.service`).
squeekboard is enabled by default on Pi OS — no action needed; just don't disable it.

## Notes

- `careflow-scanner.service` binds `127.0.0.1:8080` only (the kiosk is same-host). Don't expose it.
- `CS_BASE` must be the **dev** ChirpStack (`10.10.8.140`); the app's `DEV_GUARD` refuses any other
  base, so a misconfiguration fails safe rather than writing production.
- The Careflow node connects over USB; the app auto-detects the port (or set `SCANNER_NODE_PORT`).
