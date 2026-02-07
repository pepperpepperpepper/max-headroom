#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_integration_autoconnect.sh /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"
INNER="$ROOT/tests/integration/headroomctl_integration_autoconnect_inner.sh"

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

"$HARNESS" -- bash "$INNER" "$HEADROOMCTL"
