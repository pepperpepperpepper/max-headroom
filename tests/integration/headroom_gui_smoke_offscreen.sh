#!/usr/bin/env bash
set -euo pipefail

HEADROOM_BIN="${1:-}"
if [[ -z "$HEADROOM_BIN" ]]; then
  echo "Usage: headroom_gui_smoke_offscreen.sh /path/to/headroom" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

"$HARNESS" -- bash -lc '
  set -euo pipefail
  HEADROOM_BIN="$1"

  out_dir="$(mktemp -d "/tmp/headroom-gui-smoke-XXXXXX")"
  chmod 700 "$out_dir"

  run_shot() {
    local which="$1"
    local out="$2"
    QT_QPA_PLATFORM=offscreen "$HEADROOM_BIN" \
      --screenshot "$out" \
      --screenshot-window "$which" \
      --screenshot-delay-ms 250 \
      ${3:-}
    [[ -s "$out" ]] || { echo "Expected non-empty screenshot: $out" >&2; exit 1; }
  }

  run_shot main "$out_dir/main.png" "--tab mixer --screenshot-wait-node Headroom-TestSink --screenshot-wait-timeout-ms 9000"
  run_shot settings "$out_dir/settings.png"
  run_shot eq "$out_dir/eq.png"
  run_shot engine "$out_dir/engine.png"
  run_shot logs "$out_dir/logs.png"
' bash "$HEADROOM_BIN"
