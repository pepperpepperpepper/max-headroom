#!/usr/bin/env bash
set -euo pipefail

HEADROOM_BIN="${1:-}"
if [[ -z "$HEADROOM_BIN" ]]; then
  echo "Usage: headroom_gui_efficiency_visualizer_minimized.sh /path/to/headroom" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

"$HARNESS" -- bash -lc '
  set -euo pipefail
  HEADROOM_BIN="$1"
  QT_QPA_PLATFORM=offscreen "$HEADROOM_BIN" --self-test-visualizer-minimized
' bash "$HEADROOM_BIN"

