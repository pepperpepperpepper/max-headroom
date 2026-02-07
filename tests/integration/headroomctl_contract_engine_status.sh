#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_contract_engine_status.sh /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

"$HARNESS" -- "$HEADROOMCTL" engine status --json | jq -e '
  type == "object"
  and (.ok | type == "boolean")
  and (.systemctlAvailable | type == "boolean")
  and (.userSystemdAvailable | type == "boolean")
  and (.pipewireReachable | type == "boolean")

  and (.pipewire | type == "object")
  and (.pipewire.reachable | type == "boolean")
  and (if .pipewire.reachable then
      (.pipewire.nodes | type == "number")
      and (.pipewire.sinks | type == "number")
      and (.pipewire.sources | type == "number")
      and (.pipewire.playbackStreams | type == "number")
      and (.pipewire.captureStreams | type == "number")
      and (.pipewire.profiler | type == "boolean")
      and (.pipewire.clock | type == "boolean")
    else true end)

  and (.processes | type == "object")
  and (.processes.pipewire | (type == "boolean" or type == "null"))
  and (.processes.wireplumber | (type == "boolean" or type == "null"))
  and (.processes.pipewirePulse | (type == "boolean" or type == "null"))

  and (.units | type == "array")
  and all(.units[]; type == "object"
    and (.unit | type == "string")
    and (.loadState | type == "string")
    and (.activeState | type == "string")
    and (.subState | type == "string")
    and (.description | type == "string")
    and (.exists | type == "boolean")
    and (.active | type == "boolean")
    and ((has("error") | not) or (.error | type == "string"))
  )
  and ((has("error") | not) or (.error | type == "string"))
' >/dev/null

