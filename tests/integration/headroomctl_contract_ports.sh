#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_contract_ports.sh /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

"$HARNESS" -- "$HEADROOMCTL" ports --json | jq -e '
  type == "array"
  and all(.[]; type == "object"
    and (.id | type == "number")
    and (.name | type == "string")
    and (.direction | type == "string")
    and (.alias | type == "string")
    and (.mediaType | type == "string")
    and (.formatDsp | type == "string")
    and (.audioChannel | type == "string")
    and (.nodeId | type == "number")
    and (.nodeName | type == "string")
    and (.nodeDescription | type == "string")
    and (.nodeMediaClass | type == "string")
  )
' >/dev/null
