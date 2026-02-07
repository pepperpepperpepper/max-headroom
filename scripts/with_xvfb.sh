#!/usr/bin/env bash
set -euo pipefail

SCREEN="1100x700x24"
WITH_WM=1
WITH_TRAY=0

usage() {
  cat >&2 <<'EOF'
Usage:
  scripts/with_xvfb.sh [--screen WxHxD] [--no-wm] [--tray] -- <command...>

Runs <command...> inside an Xvfb X11 session.
  - Starts Openbox by default (needed for reliable minimize/close behavior).
  - Optionally starts stalonetray (needed for QSystemTrayIcon / tray-only tests).
EOF
}

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing dependency: $1" >&2
    exit 1
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --screen)
      SCREEN="${2:-}"
      shift 2
      ;;
    --no-wm)
      WITH_WM=0
      shift
      ;;
    --tray)
      WITH_TRAY=1
      shift
      ;;
    --)
      shift
      break
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ $# -lt 1 ]]; then
  usage
  exit 2
fi

need xvfb-run
if [[ "$WITH_WM" -eq 1 ]]; then
  need openbox
fi
if [[ "$WITH_TRAY" -eq 1 ]]; then
  need stalonetray
fi

export HEADROOM_XVFB_WITH_WM="$WITH_WM"
export HEADROOM_XVFB_WITH_TRAY="$WITH_TRAY"

xvfb-run -a -s "-screen 0 $SCREEN -ac -nolisten tcp -extension GLX" env \
  QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}" \
  bash -lc '
    set -euo pipefail
    WM_PID=""
    TRAY_PID=""

    cleanup() {
      set +e
      [[ -n "$TRAY_PID" ]] && kill -TERM "$TRAY_PID" 2>/dev/null || true
      [[ -n "$WM_PID" ]] && kill -TERM "$WM_PID" 2>/dev/null || true
    }
    trap cleanup EXIT

    if [[ "${HEADROOM_XVFB_WITH_WM:-0}" -eq 1 ]]; then
      nohup openbox >/tmp/headroom-xvfb-openbox.log 2>&1 &
      WM_PID="$!"
      # Give the WM a moment to claim the root window.
      sleep 0.15
    fi

    if [[ "${HEADROOM_XVFB_WITH_TRAY:-0}" -eq 1 ]]; then
      nohup stalonetray --geometry 1x1-0+0 --icon-size 22 --background "#202020" --kludges force_icons_size >/tmp/headroom-xvfb-tray.log 2>&1 &
      TRAY_PID="$!"
      sleep 0.15
    fi

    exec "$@"
  ' bash "$@"

