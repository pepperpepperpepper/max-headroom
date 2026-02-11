#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_integration_autoconnect_inner.sh /path/to/headroomctl" >&2
  exit 2
fi

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

rule="auto-test"

cleanup() {
  set +e
  "$HEADROOMCTL" patchbay autoconnect rule delete "$rule" --json >/dev/null 2>&1 || true
}
trap cleanup EXIT

"$HEADROOMCTL" patchbay autoconnect status --json | jq -e '.enabled | type == "boolean"' >/dev/null
"$HEADROOMCTL" patchbay autoconnect enable on --json | jq -e '.ok == true and .enabled == true' >/dev/null

"$HEADROOMCTL" patchbay autoconnect rule add "$rule" "Headroom-TestSink" "monitor_" "Headroom-AltSink" "playback_" --json \
  | jq -e '.ok == true and .name == "'"$rule"'" and .enabled == true' >/dev/null

"$HEADROOMCTL" patchbay autoconnect rules --json | jq -e 'any(.[]; .name == "'"$rule"'" and .enabled == true)' >/dev/null

apply1='{}'
for _ in $(seq 1 12); do
  apply1="$("$HEADROOMCTL" patchbay autoconnect apply --json)"
  if jq -e '
      .ok == true
      and (.createdLinks | type == "number")
      and (.alreadyPresentLinks | type == "number")
      and ((.createdLinks + .alreadyPresentLinks) >= 2)
      and (.errors | type == "array" and length == 0)
    ' <<<"$apply1" >/dev/null; then
    break
  fi
  sleep 0.1
done
if ! jq -e '
    .ok == true
    and (.createdLinks | type == "number")
    and (.alreadyPresentLinks | type == "number")
    and ((.createdLinks + .alreadyPresentLinks) >= 2)
    and (.errors | type == "array" and length == 0)
  ' <<<"$apply1" >/dev/null; then
  echo "autoconnect apply: expected >=2 links (created+alreadyPresent)" >&2
  echo "$apply1" >&2
  exit 1
fi

for _ in $(seq 1 80); do
  links="$("$HEADROOMCTL" links --json)"
  if jq -e 'any(.[]; .outputNodeName == "Headroom-TestSink" and .inputNodeName == "Headroom-AltSink")' <<<"$links" >/dev/null; then
    break
  fi
  sleep 0.05
done
links="$("$HEADROOMCTL" links --json)"
jq -e 'any(.[]; .outputNodeName == "Headroom-TestSink" and .inputNodeName == "Headroom-AltSink")' <<<"$links" >/dev/null

apply2='{}'
for _ in $(seq 1 12); do
  apply2="$("$HEADROOMCTL" patchbay autoconnect apply --json)"
  if jq -e '
      .ok == true
      and (.createdLinks | type == "number") and (.createdLinks == 0)
      and (.alreadyPresentLinks | type == "number") and (.alreadyPresentLinks >= 2)
      and (.errors | type == "array" and length == 0)
    ' <<<"$apply2" >/dev/null; then
    break
  fi
  sleep 0.1
done
if ! jq -e '
    .ok == true
    and (.createdLinks | type == "number") and (.createdLinks == 0)
    and (.alreadyPresentLinks | type == "number") and (.alreadyPresentLinks >= 2)
    and (.errors | type == "array" and length == 0)
  ' <<<"$apply2" >/dev/null; then
  echo "autoconnect apply: expected idempotent result" >&2
  echo "$apply2" >&2
  exit 1
fi

"$HEADROOMCTL" patchbay autoconnect rule delete "$rule" --json | jq -e '.ok == true and .name == "'"$rule"'"' >/dev/null
"$HEADROOMCTL" patchbay autoconnect rules --json | jq -e 'all(.[]; .name != "'"$rule"'")' >/dev/null
