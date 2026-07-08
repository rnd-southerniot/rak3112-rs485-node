#!/usr/bin/env bash
# Careflow scanner kiosk launcher (Raspberry Pi OS / labwc / Wayland).
# Waits for the local FastAPI service to actually serve the page, then opens Chromium.
#
# Borderless --app window, NOT --kiosk: exclusive fullscreen stacks above the wlr-layer-shell layers,
# which hides the on-screen keyboard (squeekboard) behind the window. A borderless --app window that
# labwc *maximizes* stays below those layers, so the keyboard shows above it. The maximize + no-decor
# rule lives in ~/.config/labwc/rc.xml (and the panel is auto-hidden via ~/.config/wf-panel-pi.ini) —
# see this dir's README.
#
# EXIT the kiosk: the on-screen "⏻ Exit" button in the UI, or over SSH:
#   sudo systemctl stop careflow-scanner && pkill -f '[c]hromium'
# Install to /home/pi/scanner/kiosk.sh and launch it from ~/.config/autostart/careflow-kiosk.desktop.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
URL="http://127.0.0.1:8080"

# Wait (up to 90 s) for the service to serve — not just for the port to open.
for _ in $(seq 1 90); do
  curl -sf "$URL/api/session" >/dev/null 2>&1 && break
  sleep 1
done

# Clear Chromium's "not shut down cleanly" flag so a reboot doesn't leave a blank restore surface.
PREF="$HOME/.config/chromium/Default/Preferences"
[ -f "$PREF" ] && sed -i \
  's/"exited_cleanly":false/"exited_cleanly":true/; s/"exit_type":"[^"]*"/"exit_type":"Normal"/g' \
  "$PREF" 2>/dev/null

exec chromium-browser --app="$URL" --class=careflow-kiosk --ozone-platform=wayland \
  --no-first-run --noerrdialogs --disable-infobars --disable-session-crashed-bubble \
  --disable-features=TranslateUI --check-for-update-interval=31536000
