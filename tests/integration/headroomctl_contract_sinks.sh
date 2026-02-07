#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_contract_sinks.sh /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

"$HARNESS" -- "$HEADROOMCTL" sinks --json | jq -e '
  type == "array"
  and all(.[]; type == "object"
    and (.id | type == "number")
    and (.name | type == "string")
    and (.description | type == "string")
    and (.mediaClass | type == "string")
    and (.controls | type == "object")
    and (.controls.hasMute | type == "boolean")
    and (.controls.hasVolume | type == "boolean")
    and (.controls.mute | type == "boolean")
    and (.controls.volume | type == "number")
    and (.controls.channelVolumes | type == "array")
    and all(.controls.channelVolumes[]?; type == "number")
  )
' >/dev/null
