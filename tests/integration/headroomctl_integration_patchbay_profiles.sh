#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_integration_patchbay_profiles.sh /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

"$HARNESS" -- bash -lc '
  set -euo pipefail
  HEADROOMCTL="$1"
  profile="test-profile"

  "$HEADROOMCTL" patchbay profiles --json | jq -e '"'"'type == "array"'"'"' >/dev/null
  "$HEADROOMCTL" patchbay save "$profile" --json | jq -e '"'"'.ok == true and .name == "'"'"'"$profile"'"'"'" and (.links | type == "number")'"'"' >/dev/null
  "$HEADROOMCTL" patchbay profiles --json | jq -e '"'"'any(.[]; . == "'"'"'"$profile"'"'"'")'"'"' >/dev/null
  "$HEADROOMCTL" patchbay apply "$profile" --no-hooks --json | jq -e '"'"'.ok == true and .name == "'"'"'"$profile"'"'"'" and .hooksEnabled == false'"'"' >/dev/null
  "$HEADROOMCTL" patchbay delete "$profile" --json | jq -e '"'"'.ok == true and .name == "'"'"'"$profile"'"'"'"'"'"' >/dev/null
  "$HEADROOMCTL" patchbay profiles --json | jq -e '"'"'all(.[]; . != "'"'"'"$profile"'"'"'")'"'"' >/dev/null
' bash "$HEADROOMCTL"

