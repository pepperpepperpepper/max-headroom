#!/usr/bin/env bash
set -euo pipefail

HEADROOM_TUI="${1:-}"
HEADROOMCTL="${2:-}"
if [[ -z "$HEADROOM_TUI" || -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomtui_smoke.sh /path/to/headroom-tui /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing dependency: $1" >&2
    exit 1
  fi
}

need jq
need expect

"$HARNESS" -- bash "$ROOT/tests/integration/headroomtui_smoke_inner.sh" "$HEADROOM_TUI" "$HEADROOMCTL"
