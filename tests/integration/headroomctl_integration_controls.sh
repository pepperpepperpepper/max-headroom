#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_integration_controls.sh /path/to/headroomctl" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

"$HARNESS" -- bash -lc '
  set -euo pipefail
  HEADROOMCTL="$1"

  sinks="$("$HEADROOMCTL" sinks --json)"
  test_sink_id="$(jq -re '"'"'.[] | select(.name=="Headroom-TestSink") | .id'"'"' <<<"$sinks")"

  "$HEADROOMCTL" set-volume "$test_sink_id" 50%
  sinks2="$("$HEADROOMCTL" sinks --json)"
  jq -e --argjson id "$test_sink_id" '"'"'
    .[] | select(.id == $id) | (.controls.hasVolume == true) and ((.controls.volume - 0.5 | fabs) < 0.05)
  '"'"' <<<"$sinks2" >/dev/null

  before_mute="$(jq -re --argjson id "$test_sink_id" '"'"'.[] | select(.id == $id) | .controls.mute | tostring'"'"' <<<"$sinks2")"
  "$HEADROOMCTL" mute "$test_sink_id" toggle
  sinks3="$("$HEADROOMCTL" sinks --json)"
  after_mute="$(jq -re --argjson id "$test_sink_id" '"'"'.[] | select(.id == $id) | .controls.mute | tostring'"'"' <<<"$sinks3")"
  [[ "$before_mute" != "$after_mute" ]] || { echo "mute toggle did not change state" >&2; exit 1; }

  "$HEADROOMCTL" default-sink set Headroom-AltSink --json | jq -e '"'"'.ok == true and .defaultSink.name == "Headroom-AltSink"'"'"' >/dev/null
  "$HEADROOMCTL" default-source set Headroom-TestSource --json | jq -e '"'"'.ok == true and .defaultSource.name == "Headroom-TestSource"'"'"' >/dev/null

  "$HEADROOMCTL" sinks order move Headroom-AltSink bottom --json | jq -e '"'"'
    .ok == true and .moved == true and (.order | type == "array") and (.order[-1].name == "Headroom-AltSink")
  '"'"' >/dev/null
  "$HEADROOMCTL" sinks order move Headroom-AltSink top --json | jq -e '"'"'
    .ok == true and .moved == true and (.order | type == "array") and (.order[0].name == "Headroom-AltSink")
  '"'"' >/dev/null
  "$HEADROOMCTL" sinks order reset --json | jq -e '"'"'.ok == true and (.order | type == "array")'"'"' >/dev/null

  "$HEADROOMCTL" eq preset Headroom-TestSink "Bass Boost" --json | jq -e '"'"'.ok == true and .nodeName == "Headroom-TestSink"'"'"' >/dev/null
  "$HEADROOMCTL" eq get Headroom-TestSink --json | jq -e '"'"'
    .nodeName == "Headroom-TestSink"
    and (.preset | type == "object")
    and (.preset.enabled == true)
    and (.preset.bands | type == "array" and length > 0)
  '"'"' >/dev/null
  "$HEADROOMCTL" eq enable Headroom-TestSink off --json | jq -e '"'"'.ok == true and .nodeName == "Headroom-TestSink" and .enabled == false'"'"' >/dev/null
' bash "$HEADROOMCTL"
