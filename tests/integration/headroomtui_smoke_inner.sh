#!/usr/bin/env bash
set -euo pipefail

TUI="${1:-}"
CTL="${2:-}"
if [[ -z "$TUI" || -z "$CTL" ]]; then
  echo "Usage: headroomtui_smoke_inner.sh /path/to/headroom-tui /path/to/headroomctl" >&2
  exit 2
fi

# Wait for sinks so the TUI has devices to operate on.
sinks_before='[]'
for _ in $(seq 1 120); do
  sinks_before="$("$CTL" sinks --json 2>/dev/null || echo '[]')"
  if jq -e 'type=="array" and length>0' <<<"$sinks_before" >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
done
if ! jq -e 'type=="array" and length>0' <<<"$sinks_before" >/dev/null 2>&1; then
  echo "Failed to list sinks." >&2
  echo "$sinks_before" >&2
  exit 1
fi
if ! jq -e 'any(.[]; .name=="Headroom-TestSink")' <<<"$sinks_before" >/dev/null 2>&1; then
  echo "Missing required sink: Headroom-TestSink" >&2
  echo "$sinks_before" >&2
  exit 1
fi
if ! jq -e 'any(.[]; .name=="Headroom-AltSink")' <<<"$sinks_before" >/dev/null 2>&1; then
  echo "Missing required sink: Headroom-AltSink" >&2
  echo "$sinks_before" >&2
  exit 1
fi

# Ensure Headroom-TestSink is first in the Outputs list so the test doesn't rely on arrow keys.
order_before='{"order":[]}'
for _ in $(seq 1 80); do
  "$CTL" sinks order reset --json >/dev/null 2>&1 || true
  "$CTL" sinks order move Headroom-TestSink top --json >/dev/null 2>&1 || true
  order_before="$("$CTL" sinks order get --json 2>/dev/null || echo '{"order":[]}')"
  if jq -e '.order | type=="array" and length>0 and .[0].name=="Headroom-TestSink"' <<<"$order_before" >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
done
target_sink_name="$(jq -re '.order[0].name' <<<"$order_before")"
if [[ "$target_sink_name" != "Headroom-TestSink" ]]; then
  echo "Expected Headroom-TestSink to be first in sink order, got: $target_sink_name" >&2
  echo "$order_before" >&2
  exit 1
fi
target_sink_id="$(jq -re --arg name "$target_sink_name" '.[] | select(.name==$name) | .id' <<<"$sinks_before")"
vol_before="$(jq -re --argjson id "$target_sink_id" '.[] | select(.id==$id) | .controls.volume' <<<"$sinks_before")"
mute_before="$(jq -r --argjson id "$target_sink_id" '.[] | select(.id==$id) | .controls.mute' <<<"$sinks_before")"
if [[ "$mute_before" != "true" && "$mute_before" != "false" ]]; then
  echo "Failed to read mute state for sink $target_sink_name ($target_sink_id)." >&2
  echo "$sinks_before" >&2
  exit 1
fi

eq_before='{}'
eq_enabled_before=''
for _ in $(seq 1 80); do
  eq_before="$("$CTL" eq get "$target_sink_name" --json 2>/dev/null || echo '{}')"
  if jq -e '.preset.enabled | type=="boolean"' <<<"$eq_before" >/dev/null 2>&1; then
    eq_enabled_before="$(jq -r '.preset.enabled' <<<"$eq_before")"
    break
  fi
  sleep 0.05
done
if [[ -z "$eq_enabled_before" ]]; then
  echo "Failed to read EQ state for $target_sink_name" >&2
  echo "$eq_before" >&2
  exit 1
fi

nodes='[]'
out_node_id=''
in_node_id=''
for _ in $(seq 1 120); do
  nodes="$("$CTL" nodes --json 2>/dev/null || echo '[]')"
  out_node_id="$(jq -r '.[] | select(.name=="Headroom-TestTone") | .id' <<<"$nodes" | head -n 1)"
  in_node_id="$(jq -r '.[] | select(.name=="Headroom-AltSink") | .id' <<<"$nodes" | head -n 1)"
  if [[ -n "$out_node_id" && "$out_node_id" != "null" && -n "$in_node_id" && "$in_node_id" != "null" ]]; then
    break
  fi
  sleep 0.05
done
if [[ -z "$out_node_id" || "$out_node_id" == "null" || -z "$in_node_id" || "$in_node_id" == "null" ]]; then
  echo "Failed to find required nodes for Patchbay connect (TestTone/AltSink)" >&2
  echo "$nodes" >&2
  exit 1
fi

ports='[]'
out_port_id=''
in_port_id=''
for _ in $(seq 1 120); do
  ports="$("$CTL" ports --json 2>/dev/null || echo '[]')"
  out_port_id="$(jq -r '.[] | select(.nodeName=="Headroom-TestTone" and .direction=="out" and .name=="output_FL") | .id' <<<"$ports" | head -n 1)"
  in_port_id="$(jq -r '.[] | select(.nodeName=="Headroom-AltSink" and .direction=="in" and .name=="playback_FL") | .id' <<<"$ports" | head -n 1)"
  if [[ -n "$out_port_id" && "$out_port_id" != "null" && -n "$in_port_id" && "$in_port_id" != "null" ]]; then
    break
  fi
  sleep 0.05
done
if [[ -z "$out_port_id" || "$out_port_id" == "null" || -z "$in_port_id" || "$in_port_id" == "null" ]]; then
  echo "Failed to find required ports for Patchbay connect (TestTone output_FL -> AltSink playback_FL)" >&2
  echo "$ports" >&2
  exit 1
fi

idx_json="$(
  jq -n \
    --argjson nodes "$nodes" \
    --argjson ports "$ports" \
    --argjson outNodeId "$out_node_id" \
    --argjson outPortId "$out_port_id" \
    --argjson inNodeId "$in_node_id" \
    --argjson inPortId "$in_port_id" '
    def lc: ascii_downcase;
    def contains_ci($s; $sub): ($s | tostring | lc | contains($sub));

    def portKind:
      (.mediaType // "" | lc) as $mt
      | if $mt == "midi" then "midi"
        elif $mt == "audio" then "audio"
        elif ((.audioChannel // "") | length) > 0 then "audio"
        elif contains_ci(.formatDsp // ""; "midi") then "midi"
        elif contains_ci(.formatDsp // ""; "audio") then "audio"
        elif contains_ci(.nodeMediaClass // ""; "midi") then "midi"
        elif contains_ci(.nodeMediaClass // ""; "audio") then "audio"
        elif contains_ci(.name // ""; "midi") or contains_ci(.alias // ""; "midi") then "midi"
        else "other" end;

    def nodeSort:
      sort_by(.mediaClass // "", .description // "", .name // "");

    def outNodes:
      ($ports | map(select(.direction == "out" and (.nodeId // 0) != 0) | .nodeId) | unique) as $allowed
      | ($nodes | map(select(.id as $id | $allowed | index($id))) | nodeSort);

    def portsForNode($nodeId; $dir):
      ($ports | [ .[] | select(.nodeId == $nodeId and .direction == $dir) ] | sort_by(.id));

    ($ports | map(select(.id == $outPortId)) | .[0]) as $selectedOutPort
    | ($selectedOutPort | portKind) as $wantKind
    | ($ports
        | map(select(.direction == "in" and (.nodeId // 0) != 0 and ((. | portKind) == $wantKind)) | .nodeId)
        | unique
      ) as $allowedIn
    | ($nodes | map(select(.id as $id | $allowedIn | index($id))) | nodeSort) as $inNodes
    | {
        outNodeSteps: (outNodes | map(.id) | index($outNodeId)),
        outPortSteps: (portsForNode($outNodeId; "out") | map(.id) | index($outPortId)),
        inNodeSteps: ($inNodes | map(.id) | index($inNodeId)),
        inPortSteps: (portsForNode($inNodeId; "in") | map(select((. | portKind) == $wantKind)) | map(.id) | index($inPortId))
      }
  '
)"
if ! jq -e '
    (.outNodeSteps | type == "number" and . >= 0)
    and (.outPortSteps | type == "number" and . >= 0)
    and (.inNodeSteps | type == "number" and . >= 0)
    and (.inPortSteps | type == "number" and . >= 0)
  ' <<<"$idx_json" >/dev/null 2>&1; then
  echo "Failed to compute Patchbay prompt indices." >&2
  echo "$idx_json" >&2
  exit 1
fi
out_node_steps="$(jq -r '.outNodeSteps' <<<"$idx_json")"
out_port_steps="$(jq -r '.outPortSteps' <<<"$idx_json")"
in_node_steps="$(jq -r '.inNodeSteps' <<<"$idx_json")"
in_port_steps="$(jq -r '.inPortSteps' <<<"$idx_json")"

# Wait for at least one link so Patchbay has something to delete.
links="[]"
for _ in $(seq 1 80); do
  links="$("$CTL" links --json 2>/dev/null || echo "[]")"
  if jq -e 'type=="array" and length>0' <<<"$links" >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
done
links="$("$CTL" links --json)"
if ! jq -e 'type=="array" and length>0' <<<"$links" >/dev/null 2>&1; then
  echo "Failed to list links (Patchbay needs at least one link to delete)." >&2
  echo "$links" >&2
  exit 1
fi
first_link_id="$(jq -re 'sort_by(.outputNodeId,.inputNodeId,.outputPortId,.inputPortId) | .[0].id' <<<"$links")"
first_link_out_node_id="$(jq -re 'sort_by(.outputNodeId,.inputNodeId,.outputPortId,.inputPortId) | .[0].outputNodeId' <<<"$links")"
first_link_out_port_id="$(jq -re 'sort_by(.outputNodeId,.inputNodeId,.outputPortId,.inputPortId) | .[0].outputPortId' <<<"$links")"
first_link_in_node_id="$(jq -re 'sort_by(.outputNodeId,.inputNodeId,.outputPortId,.inputPortId) | .[0].inputNodeId' <<<"$links")"
first_link_in_port_id="$(jq -re 'sort_by(.outputNodeId,.inputNodeId,.outputPortId,.inputPortId) | .[0].inputPortId' <<<"$links")"

vol_key="+"
if jq -e -n --argjson v "$vol_before" '$v >= 1.95' >/dev/null 2>&1; then
  vol_key="-"
fi

export HEADROOM_TUI="$TUI"
export OUT_NODE_ID="$out_node_id"
export IN_NODE_ID="$in_node_id"
export OUT_PORT_ID="$out_port_id"
export IN_PORT_ID="$in_port_id"
export OUT_NODE_STEPS="$out_node_steps"
export OUT_PORT_STEPS="$out_port_steps"
export IN_NODE_STEPS="$in_node_steps"
export IN_PORT_STEPS="$in_port_steps"
export VOL_KEY="$vol_key"

expect <<'EOF'
  if {[info exists env(HEADROOM_TUI_EXPECT_LOG)] && $env(HEADROOM_TUI_EXPECT_LOG) != ""} {
    log_user 1
    log_file -noappend $env(HEADROOM_TUI_EXPECT_LOG)
  } else {
    log_user 0
  }
  set timeout 25
  set tui $env(HEADROOM_TUI)
  set out_node_id $env(OUT_NODE_ID)
  set in_node_id $env(IN_NODE_ID)
  set out_port_id $env(OUT_PORT_ID)
  set in_port_id $env(IN_PORT_ID)
  set out_node_steps $env(OUT_NODE_STEPS)
  set out_port_steps $env(OUT_PORT_STEPS)
  set in_node_steps $env(IN_NODE_STEPS)
  set in_port_steps $env(IN_PORT_STEPS)
  set vol_key $env(VOL_KEY)

  proc fail {msg} {
    puts stderr $msg
    exit 1
  }

  proc reset_to_top {} { for {set i 0} {$i < 80} {incr i} { send "k" } }
  proc move_down {n} { for {set i 0} {$i < $n} {incr i} { send "j" } }

  spawn -noecho $tui
  # Give PipeWireGraph a moment to populate nodes/ports before we open Patchbay prompts.
  after 2200

  # Patchbay: delete selected link, then connect TestTone -> AltSink.
  send "4"
  # Give the Patchbay view time to populate links before we try to delete.
  after 1600
  send "d"
  after 600
  send "c"
  after 350

  reset_to_top
  move_down $out_node_steps
  send "\n"
  after 350

  reset_to_top
  move_down $out_port_steps
  send "\n"
  after 350

  reset_to_top
  move_down $in_node_steps
  send "\n"
  after 350

  reset_to_top
  move_down $in_port_steps
  send "\n"

  after 450

  # Outputs: volume + mute + set default.
  send "1"
  after 350
  send $vol_key
  after 250
  send "m"
  after 250
  send "\n"
  after 1600

  # EQ: toggle.
  send "5"
  after 350
  send "e"
  after 350

  # Outputs: reorder (move the current sink down by one).
  send "1"
  after 350
  send "]"
  after 350

  send "q"
  expect eof
  catch wait result
  set exit_status [lindex $result 3]
  exit $exit_status
EOF

order_after="$("$CTL" sinks order get --json)"
if [[ "$(jq -c '.order' <<<"$order_before")" == "$(jq -c '.order' <<<"$order_after")" ]]; then
  echo "Expected sinks order to change after TUI reorder action." >&2
  echo "Before: $(jq -c '.order' <<<"$order_before")" >&2
  echo "After:  $(jq -c '.order' <<<"$order_after")" >&2
  exit 1
fi

default_after="$("$CTL" default-sink get --json)"
if ! jq -e --arg name "$target_sink_name" '.defaultSink != null and .defaultSink.name == $name' <<<"$default_after" >/dev/null; then
  echo "default: mismatch (expected $target_sink_name)" >&2
  echo "$default_after" >&2
  exit 1
fi

sinks_after="$("$CTL" sinks --json)"
vol_after="$(jq -re --argjson id "$target_sink_id" '.[] | select(.id==$id) | .controls.volume' <<<"$sinks_after")"
mute_after="$(jq -r --argjson id "$target_sink_id" '.[] | select(.id==$id) | .controls.mute' <<<"$sinks_after")"
if [[ "$mute_after" != "true" && "$mute_after" != "false" ]]; then
  echo "Failed to read mute state for sink $target_sink_name ($target_sink_id) after TUI run." >&2
  echo "$sinks_after" >&2
  exit 1
fi
if [[ "$mute_after" == "$mute_before" ]] && jq -e -n --argjson a "$vol_after" --argjson b "$vol_before" '$a == $b' >/dev/null 2>&1; then
  echo "Expected volume or mute to change for sink $target_sink_name ($target_sink_id)." >&2
  echo "Before: volume=$vol_before mute=$mute_before" >&2
  echo "After:  volume=$vol_after mute=$mute_after" >&2
  exit 1
fi

eq_after="$("$CTL" eq get "$target_sink_name" --json)"
eq_enabled_after="$(jq -r '.preset.enabled' <<<"$eq_after")"
if [[ "$eq_enabled_after" != "true" && "$eq_enabled_after" != "false" ]]; then
  echo "Failed to read EQ state for $target_sink_name after TUI run." >&2
  echo "$eq_after" >&2
  exit 1
fi
if [[ "$eq_enabled_after" == "$eq_enabled_before" ]]; then
  echo "Expected EQ enabled state to change for $target_sink_name." >&2
  echo "Before: $eq_enabled_before" >&2
  echo "After:  $eq_enabled_after" >&2
  exit 1
fi

links_after="$("$CTL" links --json)"
if ! jq -e \
  --argjson outNodeId "$first_link_out_node_id" \
  --argjson outPortId "$first_link_out_port_id" \
  --argjson inNodeId "$first_link_in_node_id" \
  --argjson inPortId "$first_link_in_port_id" \
  'all(.[]; (.outputNodeId==$outNodeId and .outputPortId==$outPortId and .inputNodeId==$inNodeId and .inputPortId==$inPortId) | not)' \
  <<<"$links_after" >/dev/null; then
  echo "patchbay: expected selected link to be deleted (before id=$first_link_id out=$first_link_out_node_id/$first_link_out_port_id in=$first_link_in_node_id/$first_link_in_port_id)" >&2
  jq -r \
    --argjson outNodeId "$first_link_out_node_id" \
    --argjson outPortId "$first_link_out_port_id" \
    --argjson inNodeId "$first_link_in_node_id" \
    --argjson inPortId "$first_link_in_port_id" \
    '.[] | select(.outputNodeId==$outNodeId and .outputPortId==$outPortId and .inputNodeId==$inNodeId and .inputPortId==$inPortId)' \
    <<<"$links_after" >&2 || true
  exit 1
fi

if ! jq -e '
    any(.[]; .outputNodeName=="Headroom-TestTone"
      and .outputPortName=="output_FL"
      and .inputNodeName=="Headroom-AltSink"
      and .inputPortName=="playback_FL")
  ' <<<"$links_after" >/dev/null; then
  echo "patchbay: expected TestTone output_FL -> AltSink playback_FL link" >&2
  jq -r '.[] | select(.outputNodeName=="Headroom-TestTone") | "have: id=\(.id) out=\(.outputPortName) inNode=\(.inputNodeName) inPort=\(.inputPortName)"' <<<"$links_after" >&2 || true
  exit 1
fi
