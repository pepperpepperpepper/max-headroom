#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_integration_sessions.sh /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

"$HARNESS" -- bash -lc '
  set -euo pipefail
  HEADROOMCTL="$1"
  snap="test-snapshot"

  "$HEADROOMCTL" default-sink set Headroom-AltSink --json | jq -e '"'"'.ok == true'"'"' >/dev/null
  "$HEADROOMCTL" default-source set Headroom-TestSource --json | jq -e '"'"'.ok == true'"'"' >/dev/null
  "$HEADROOMCTL" eq preset Headroom-TestSink Flat --json | jq -e '"'"'.ok == true'"'"' >/dev/null

  "$HEADROOMCTL" session list --json | jq -e '"'"'type == "array"'"'"' >/dev/null
  "$HEADROOMCTL" session save "$snap" --json | jq -e '"'"'.ok == true and .name == "'"'"'"$snap"'"'"'"'"'"' >/dev/null
  "$HEADROOMCTL" session list --json | jq -e '"'"'any(.[]; . == "'"'"'"$snap"'"'"'")'"'"' >/dev/null
  "$HEADROOMCTL" session apply "$snap" --json | jq -e '"'"'.ok == true and .name == "'"'"'"$snap"'"'"'" and (.patchbay | type == "object")'"'"' >/dev/null
  "$HEADROOMCTL" session delete "$snap" --json | jq -e '"'"'.ok == true and .name == "'"'"'"$snap"'"'"'"'"'"' >/dev/null
  "$HEADROOMCTL" session list --json | jq -e '"'"'all(.[]; . != "'"'"'"$snap"'"'"'")'"'"' >/dev/null
' bash "$HEADROOMCTL"

