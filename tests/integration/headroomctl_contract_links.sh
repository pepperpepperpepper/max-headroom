#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_contract_links.sh /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

"$HARNESS" -- "$HEADROOMCTL" links --json | jq -e '
  type == "array"
  and all(.[]; type == "object"
    and (.id | type == "number")

    and (.inputNodeId | type == "number")
    and (.inputNodeName | type == "string")
    and (.inputNodeDescription | type == "string")
    and (.inputNodeMediaClass | type == "string")

    and (.inputPortId | type == "number")
    and (.inputPortName | type == "string")
    and (.inputPortAlias | type == "string")
    and (.inputPortMediaType | type == "string")
    and (.inputPortAudioChannel | type == "string")

    and (.outputNodeId | type == "number")
    and (.outputNodeName | type == "string")
    and (.outputNodeDescription | type == "string")
    and (.outputNodeMediaClass | type == "string")

    and (.outputPortId | type == "number")
    and (.outputPortName | type == "string")
    and (.outputPortAlias | type == "string")
    and (.outputPortMediaType | type == "string")
    and (.outputPortAudioChannel | type == "string")
  )
' >/dev/null
