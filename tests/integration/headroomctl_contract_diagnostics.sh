#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_contract_diagnostics.sh /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

"$HARNESS" -- "$HEADROOMCTL" diagnostics status --json | jq -e '
  type == "object"
  and (.ok | type == "boolean")
  and (.seq | type == "number")

  and (.info | type == "object")
  and (.info.available | type == "boolean")
  and (.info.counter | type == "number")
  and (.info.cpuLoadFast | type == "number")
  and (.info.cpuLoadMedium | type == "number")
  and (.info.cpuLoadSlow | type == "number")
  and (.info.xrunCount | type == "number")

  and (.clock | type == "object")
  and (.clock.available | type == "boolean")
  and (.clock.cycle | type == "number")
  and (.clock.durationMs | (type == "number" or type == "null"))
  and (.clock.delayMs | (type == "number" or type == "null"))
  and (.clock.xrunDurationMs | (type == "number" or type == "null"))

  and (.drivers | type == "array")
  and all(.drivers[]; type == "object"
    and (.id | type == "number")
    and (.name | type == "string")
    and (.status | type == "string")
    and (.xrunCount | type == "number")
    and (.latencyMs | (type == "number" or type == "null"))
    and (.waitMs | (type == "number" or type == "null"))
    and (.busyMs | (type == "number" or type == "null"))
    and (.waitRatio | (type == "number" or type == "null"))
    and (.busyRatio | (type == "number" or type == "null"))
  )

  and (.followers | type == "array")
  and all(.followers[]; type == "object"
    and (.id | type == "number")
    and (.name | type == "string")
    and (.status | type == "string")
    and (.xrunCount | type == "number")
    and (.latencyMs | (type == "number" or type == "null"))
    and (.waitMs | (type == "number" or type == "null"))
    and (.busyMs | (type == "number" or type == "null"))
    and (.waitRatio | (type == "number" or type == "null"))
    and (.busyRatio | (type == "number" or type == "null"))
  )

  and ((has("note") | not) or (.note | type == "string"))
' >/dev/null

