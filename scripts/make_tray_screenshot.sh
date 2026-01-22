#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

OUT_PATH="${1:-$ROOT/screenshots/tray-menu.png}"
OUT_DIR="$(dirname "$OUT_PATH")"
mkdir -p "$OUT_DIR"

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing dependency: $1" >&2
    exit 1
  fi
}

need xvfb-run
need stalonetray
need xdotool
need import
need pipewire
need pw-cli

RUNTIME_DIR="/tmp/headroom-tray-runtime-$RANDOM-$RANDOM"
mkdir -p "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"

PIPEWIRE_LOG_OUT="/tmp/headroom-tray-pipewire.out"
PIPEWIRE_LOG_ERR="/tmp/headroom-tray-pipewire.err"

cleanup() {
  set +e
  [[ -n "${HEADROOM_PID:-}" ]] && kill -TERM "$HEADROOM_PID" 2>/dev/null || true
  [[ -n "${TRAY_PID:-}" ]] && kill -TERM "$TRAY_PID" 2>/dev/null || true
  [[ -n "${PIPEWIRE_PID:-}" ]] && kill -TERM "$PIPEWIRE_PID" 2>/dev/null || true
  rm -rf "$RUNTIME_DIR" 2>/dev/null || true
}
trap cleanup EXIT

echo "[1/4] Build"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/build" -j >/dev/null

echo "[2/4] Start private PipeWire"
XDG_RUNTIME_DIR="$RUNTIME_DIR" nohup pipewire -c /usr/share/pipewire/pipewire.conf >"$PIPEWIRE_LOG_OUT" 2>"$PIPEWIRE_LOG_ERR" &
PIPEWIRE_PID=$!

for _ in $(seq 1 200); do
  [[ -S "$RUNTIME_DIR/pipewire-0" ]] && break
  sleep 0.05
done

XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-cli -r pipewire-0-manager create-node spa-node-factory \
  factory.name=support.null-audio-sink node.name=Headroom-NullSink node.description=NullSink \
  media.class=Audio/Sink object.linger=true >/dev/null

echo "[3/4] Launch Xvfb + tray + Headroom, open tray menu, screenshot"
xvfb-run -a -s "-screen 0 1100x700x24 -ac -nolisten tcp -extension GLX" bash -lc "
  set -euo pipefail
  export XDG_RUNTIME_DIR='$RUNTIME_DIR'

  stalonetray --geometry 1x1+6+6 --decorations none --window-type dock --skip-taskbar --icon-size 24 --slot-size 24 --log-level err &
  TRAY_PID=\$!

  # Give the tray time to claim the system-tray selection.
  sleep 0.3

  '$ROOT/build/headroom' &
  HEADROOM_PID=\$!

  # Wait for the main window to appear, then close it to hide-to-tray.
  for _ in \$(seq 1 120); do
    WIN=\$(xdotool search --onlyvisible --name 'Headroom' 2>/dev/null | head -n 1 || true)
    [[ -n \"\$WIN\" ]] && break
    sleep 0.05
  done
  if [[ -n \"\${WIN:-}\" ]]; then
    xdotool windowclose \"\$WIN\" || true
  fi

  # Find the tray window and right-click to open Headroom's menu.
  TRAY_WIN=\$(xdotool search --classname stalonetray 2>/dev/null | head -n 1 || true)
  if [[ -z \"\$TRAY_WIN\" ]]; then
    echo 'Could not find stalonetray window' >&2
    exit 1
  fi

  eval \$(xdotool getwindowgeometry --shell \"\$TRAY_WIN\")
  CX=\$((X + WIDTH / 2))
  CY=\$((Y + HEIGHT / 2))
  xdotool mousemove --sync \"\$CX\" \"\$CY\" click 3

  sleep 0.2
  import -window root '$OUT_PATH'

  kill -TERM \"\$HEADROOM_PID\" 2>/dev/null || true
  kill -TERM \"\$TRAY_PID\" 2>/dev/null || true
  wait \"\$HEADROOM_PID\" 2>/dev/null || true
  wait \"\$TRAY_PID\" 2>/dev/null || true
"

echo "[4/4] Done"
echo "Wrote: $OUT_PATH"
