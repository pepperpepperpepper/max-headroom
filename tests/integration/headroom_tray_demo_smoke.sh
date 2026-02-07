#!/usr/bin/env bash
set -euo pipefail

HEADROOM_BIN="${1:-}"
HEADROOMCTL_BIN="${2:-}"
if [[ -z "$HEADROOM_BIN" || -z "$HEADROOMCTL_BIN" ]]; then
  echo "Usage: headroom_tray_demo_smoke.sh /path/to/headroom /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="$(mktemp -d "/tmp/headroom-tray-demo-smoke-XXXXXX")"
chmod 700 "$OUT_DIR"

HEADROOM_BIN="$HEADROOM_BIN" HEADROOMCTL_BIN="$HEADROOMCTL_BIN" "$ROOT/scripts/make_tray_demo_screenshots.sh" "$OUT_DIR"
