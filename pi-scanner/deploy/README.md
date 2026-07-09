# Pi 5 kiosk deployment

Boots the Raspberry Pi 5 into the scanner app. Two systemd units: the FastAPI service and a
Chromium kiosk pointed at `localhost:8080`.

## Install (on the Pi)

```bash
# 1. clone + venv
cd ~ && git clone <repo> rak3112-rs485-node
cd rak3112-rs485-node/pi-scanner
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt

# 2. scanner backend — a SYSTEM unit (holds the service env: firmware-service URL, dev ChirpStack
#    base, token). Copy from deploy/, set the env, enable.
sudo cp deploy/careflow-scanner.service /etc/systemd/system/
sudoedit /etc/systemd/system/careflow-scanner.service   # adjust paths/IPs, set the token
sudo systemctl daemon-reload
sudo systemctl enable --now careflow-scanner.service

# 3. kiosk — a systemd USER unit (NOT a system unit: it must share the graphical session so Chromium
#    can attach to the Wayland socket). The XDG autostart entry starts it on login; Restart=always
#    auto-recovers it.
cp deploy/kiosk.sh ~/scanner/kiosk.sh && chmod +x ~/scanner/kiosk.sh
mkdir -p ~/.config/systemd/user ~/.config/autostart
cp deploy/careflow-kiosk.service ~/.config/systemd/user/
cp deploy/careflow-kiosk.desktop ~/.config/autostart/    # runs `systemctl --user start careflow-kiosk`
systemctl --user daemon-reload
systemctl --user enable --now careflow-kiosk.service     # needs the graphical (labwc) session
```

The operator sees only the wizard; no terminal is needed for normal use. Logs: `journalctl -u
careflow-scanner -f` (backend) and `systemctl --user status careflow-kiosk` / `journalctl --user -u
careflow-kiosk -f` (kiosk). Reload the kiosk after a config change with `systemctl --user restart
careflow-kiosk`.

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

- **Chromium must speak Wayland text-input** — `kiosk.sh` launches it with `--enable-wayland-ime
  --wayland-text-input-version=3`. Without it squeekboard has no input context: it shows but can't
  type into the field and dismisses itself on the first keypress.

Reload after editing: `labwc --reconfigure` and restart `wf-panel-pi`.

The keyboard is driven by squeekboard's own input-method: it auto-shows when a text field is focused
and hides when it's blurred — the app does NOT drive show/hide from focus events (tapping the
keyboard's surface briefly blurs the field, so a focusout-hide would dismiss it mid-type). The header
⌨ button is a manual override via `POST /api/kiosk/keyboard` → `busctl --user ... sm.puri.OSK0
SetVisible` (hence the session-bus env in `careflow-scanner.service`). squeekboard is enabled by
default on Pi OS — no action needed; just don't disable it.

## Notes

- `careflow-scanner.service` binds `127.0.0.1:8080` only (the kiosk is same-host). Don't expose it.
- `CS_BASE` must be the **dev** ChirpStack (`10.10.8.140`); the app's `DEV_GUARD` refuses any other
  base, so a misconfiguration fails safe rather than writing production.
- The Careflow node connects over USB; the app auto-detects the port (or set `SCANNER_NODE_PORT`).
