#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_integration_recording.sh /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

"$HARNESS" -- bash -lc '
  set -euo pipefail
  HEADROOMCTL="$1"

  stop_recorder() {
    "$HEADROOMCTL" record stop --json >/dev/null 2>&1 || true
  }
  trap stop_recorder EXIT

  "$HEADROOMCTL" record status --json | jq -e '"'"'.recording == false'"'"' >/dev/null

  out="$XDG_RUNTIME_DIR/headroom-test.wav"
  start="$("$HEADROOMCTL" record start "$out" --format wav --target Headroom-TestSink --sink --background --json)"
  pid="$(jq -re '"'"'.pid'"'"' <<<"$start")"
  [[ "$pid" -gt 0 ]] || { echo "invalid recorder pid: $pid" >&2; exit 1; }

  for _ in $(seq 1 80); do
    st="$("$HEADROOMCTL" record status --json)"
    if jq -e --argjson pid "$pid" '"'"'.recording == true and .pid == $pid'"'"' <<<"$st" >/dev/null; then
      break
    fi
    sleep 0.05
  done

  st="$("$HEADROOMCTL" record status --json)"
  jq -e --argjson pid "$pid" '"'"'
    .recording == true
    and .pid == $pid
    and (.filePath | type == "string" and length > 0)
    and (.format | type == "string" and length > 0)
    and (.bytesWritten | type == "number")
  '"'"' <<<"$st" >/dev/null

  "$HEADROOMCTL" record stop --json | jq -e --argjson pid "$pid" '"'"'.ok == true and .pid == $pid'"'"' >/dev/null

  for _ in $(seq 1 80); do
    st="$("$HEADROOMCTL" record status --json)"
    if jq -e '"'"'.recording == false'"'"' <<<"$st" >/dev/null; then
      break
    fi
    sleep 0.05
  done
  "$HEADROOMCTL" record status --json | jq -e '"'"'.recording == false'"'"' >/dev/null
' bash "$HEADROOMCTL"

