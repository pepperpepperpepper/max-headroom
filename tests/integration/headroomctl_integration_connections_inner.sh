#!/usr/bin/env bash
set -euo pipefail

HEADROOMCTL="${1:-}"
if [[ -z "$HEADROOMCTL" ]]; then
  echo "Usage: headroomctl_integration_connections_inner.sh /path/to/headroomctl" >&2
  exit 2
fi

command -v jq >/dev/null 2>&1 || { echo "Missing dependency: jq" >&2; exit 1; }

ports="$("$HEADROOMCTL" ports --json)"

# Use device ports (not a client stream) to avoid session-policy auto-reconnecting "expected" stream routes.
out_fl="$(jq -re '.[] | select(.nodeName=="Headroom-TestSink" and .name=="monitor_FL") | .id' <<<"$ports")"
out_fr="$(jq -re '.[] | select(.nodeName=="Headroom-TestSink" and .name=="monitor_FR") | .id' <<<"$ports")"
in_fl="$(jq -re '.[] | select(.nodeName=="Headroom-AltSink" and .name=="playback_FL") | .id' <<<"$ports")"
in_fr="$(jq -re '.[] | select(.nodeName=="Headroom-AltSink" and .name=="playback_FR") | .id' <<<"$ports")"

links0="$("$HEADROOMCTL" links --json)"
jq -e --argjson ofl "$out_fl" --argjson ifl "$in_fl" 'all(.[]; (.outputPortId != $ofl) or (.inputPortId != $ifl))' <<<"$links0" >/dev/null
jq -e --argjson ofr "$out_fr" --argjson ifr "$in_fr" 'all(.[]; (.outputPortId != $ofr) or (.inputPortId != $ifr))' <<<"$links0" >/dev/null

res_fl="$("$HEADROOMCTL" connect "$out_fl" "$in_fl" --json)"
res_fr="$("$HEADROOMCTL" connect "$out_fr" "$in_fr" --json)"
jq -e '.ok == true' <<<"$res_fl" >/dev/null
jq -e '.ok == true' <<<"$res_fr" >/dev/null

link_fl=''
link_fr=''
for _ in $(seq 1 60); do
  links1="$("$HEADROOMCTL" links --json)"
  link_fl="$(jq -r --argjson ofl "$out_fl" --argjson ifl "$in_fl" '.[] | select(.outputPortId==$ofl and .inputPortId==$ifl) | .id' <<<"$links1" | head -n 1)"
  link_fr="$(jq -r --argjson ofr "$out_fr" --argjson ifr "$in_fr" '.[] | select(.outputPortId==$ofr and .inputPortId==$ifr) | .id' <<<"$links1" | head -n 1)"
  if [[ -n "$link_fl" && "$link_fl" != "null" && -n "$link_fr" && "$link_fr" != "null" ]]; then
    break
  fi
  sleep 0.05
done
if [[ -z "$link_fl" || "$link_fl" == "null" || -z "$link_fr" || "$link_fr" == "null" ]]; then
  echo "connect: expected both links to exist" >&2
  echo "res_fl=$res_fl" >&2
  echo "res_fr=$res_fr" >&2
  echo "$("$HEADROOMCTL" links --json)" >&2
  exit 1
fi

"$HEADROOMCTL" disconnect "$link_fl" --json | jq -e --argjson lid "$link_fl" '.ok == true and (.linkId == $lid)' >/dev/null
"$HEADROOMCTL" disconnect "$link_fr" --json | jq -e --argjson lid "$link_fr" '.ok == true and (.linkId == $lid)' >/dev/null

for _ in $(seq 1 60); do
  links2="$("$HEADROOMCTL" links --json)"
  if jq -e --argjson ofl "$out_fl" --argjson ifl "$in_fl" 'all(.[]; (.outputPortId != $ofl) or (.inputPortId != $ifl))' <<<"$links2" >/dev/null \
    && jq -e --argjson ofr "$out_fr" --argjson ifr "$in_fr" 'all(.[]; (.outputPortId != $ofr) or (.inputPortId != $ifr))' <<<"$links2" >/dev/null; then
    break
  fi
  sleep 0.05
done

links2="$("$HEADROOMCTL" links --json)"
jq -e --argjson ofl "$out_fl" --argjson ifl "$in_fl" 'all(.[]; (.outputPortId != $ofl) or (.inputPortId != $ifl))' <<<"$links2" >/dev/null
jq -e --argjson ofr "$out_fr" --argjson ifr "$in_fr" 'all(.[]; (.outputPortId != $ofr) or (.inputPortId != $ifr))' <<<"$links2" >/dev/null
