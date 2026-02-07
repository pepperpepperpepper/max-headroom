#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_contract_default_devices.sh /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

"$HARNESS" -- "$HEADROOMCTL" default-sink get --json | jq -e '
  type == "object"
  and .ok == true
  and (.defaultSinkId | type == "number")
  and (.defaultSink | type == "object")
  and (.defaultSink.id == .defaultSinkId)
  and (.defaultSink.name | type == "string")
  and (.defaultSink.description | type == "string")
  and (.defaultSink.mediaClass | type == "string")
' >/dev/null

"$HARNESS" -- "$HEADROOMCTL" default-source get --json | jq -e '
  type == "object"
  and .ok == true
  and (.defaultSourceId | type == "number")
  and (.defaultSource | type == "object")
  and (.defaultSource.id == .defaultSourceId)
  and (.defaultSource.name | type == "string")
  and (.defaultSource.description | type == "string")
  and (.defaultSource.mediaClass | type == "string")
' >/dev/null

