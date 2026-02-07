#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

FAIL=0
NON_INTERACTIVE=0
PIPEWIRE_OK=1
READ_ONLY=0
RUN_GUI_SELF_TESTS=0
RUN_EFFICIENCY_AUDIT=0
RUN_GUI_EFFICIENCY_MATRIX=0
EFFICIENCY_DURATION_S=300
SKIP_SYSTEMD=0

if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
  guess_runtime_dir="/run/user/$(id -u 2>/dev/null || echo '')"
  if [[ -n "$guess_runtime_dir" && -d "$guess_runtime_dir" ]]; then
    export XDG_RUNTIME_DIR="$guess_runtime_dir"
  fi
fi

usage() {
  cat >&2 <<'EOF'
Usage:
  scripts/host_test.sh [--non-interactive] [--read-only]
                     [--skip-systemd]
                     [--run-gui-self-tests]
                     [--gui-efficiency-matrix]
                     [--efficiency-audit [--efficiency-duration SEC]]
                     [--headroomctl PATH] [--headroom PATH] [--headroom-tui PATH]

Runs a best-effort checklist for validating Headroom on a real PipeWire host:
- systemd --user unit status (PipeWire/WirePlumber)
- headroomctl engine + diagnostics status
- device controls (default sink, volume, mute)
- optional: clock preset and MIDI bridge (interactive prompt)
- recording smoke test (short WAV)

Notes:
- This script may change your default sink, volume, and mute state during the test.
- Use a disposable user/session if you want a completely clean run.
EOF
}

HEADROOMCTL=""
HEADROOM=""
HEADROOM_TUI=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --non-interactive)
      NON_INTERACTIVE=1
      shift
      ;;
    --read-only)
      READ_ONLY=1
      shift
      ;;
    --skip-systemd)
      SKIP_SYSTEMD=1
      shift
      ;;
    --run-gui-self-tests)
      RUN_GUI_SELF_TESTS=1
      shift
      ;;
    --gui-efficiency-matrix)
      RUN_GUI_EFFICIENCY_MATRIX=1
      shift
      ;;
    --efficiency-audit)
      RUN_EFFICIENCY_AUDIT=1
      shift
      ;;
    --efficiency-duration)
      EFFICIENCY_DURATION_S="${2:-}"
      shift 2
      ;;
    --headroomctl)
      HEADROOMCTL="${2:-}"
      shift 2
      ;;
    --headroom)
      HEADROOM="${2:-}"
      shift 2
      ;;
    --headroom-tui)
      HEADROOM_TUI="${2:-}"
      shift 2
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage
      exit 2
      ;;
  esac
done

pick_bin() {
  local want="$1"
  shift
  local candidate=""
  for candidate in "$@"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  done
  if command -v "$want" >/dev/null 2>&1; then
    command -v "$want"
    return 0
  fi
  return 1
}

if [[ -z "$HEADROOMCTL" ]]; then
  HEADROOMCTL="$(
    pick_bin headroomctl \
      "$ROOT/build_test/headroomctl" \
      "$ROOT/build/headroomctl" \
      "$ROOT/build_nogui/headroomctl" \
      "$ROOT/build-debug/headroomctl" \
      "$ROOT/build-release/headroomctl" \
      || true
  )"
fi
if [[ -z "$HEADROOM" ]]; then
  HEADROOM="$(
    pick_bin headroom \
      "$ROOT/build_test/headroom" \
      "$ROOT/build/headroom" \
      "$ROOT/build-debug/headroom" \
      "$ROOT/build-release/headroom" \
      || true
  )"
fi
if [[ -z "$HEADROOM_TUI" ]]; then
  HEADROOM_TUI="$(
    pick_bin headroom-tui \
      "$ROOT/build_test/headroom-tui" \
      "$ROOT/build/headroom-tui" \
      "$ROOT/build_nogui/headroom-tui" \
      "$ROOT/build-debug/headroom-tui" \
      "$ROOT/build-release/headroom-tui" \
      || true
  )"
fi

log() { printf "\n== %s ==\n" "$*"; }
info() { printf "info: %s\n" "$*"; }
warn() { printf "warn: %s\n" "$*" >&2; }

pause() {
  local msg="$1"
  if [[ "$NON_INTERACTIVE" -eq 1 ]]; then
    info "skip prompt: $msg"
    return 0
  fi
  read -r -p "$msg [Enter to continue] " _ || true
}

try() {
  local title="$1"
  shift
  echo "+ $title"
  "$@" || { warn "FAILED: $title"; FAIL=1; return 0; }
}

confirm() {
  local msg="$1"
  if [[ "$NON_INTERACTIVE" -eq 1 ]]; then
    return 1
  fi
  local ans=""
  read -r -p "$msg [y/N] " ans || true
  [[ "${ans,,}" == "y" || "${ans,,}" == "yes" ]]
}

need_soft() {
  local cmd="$1"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    warn "missing dependency: $cmd"
    return 1
  fi
  return 0
}

json_val() {
  local expr="$1"
  if command -v jq >/dev/null 2>&1; then
    jq -r "$expr" 2>/dev/null || true
    return 0
  fi
  if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import json,sys
expr=sys.argv[1].strip()
if not expr.startswith("."):
    sys.exit(0)
expr=expr[1:]
data=json.load(sys.stdin)
cur=data
for part in expr.split("."):
    if part == "":
        continue
    if part.endswith("]") and "[" in part:
        key, idx = part[:-1].split("[", 1)
        if key:
            cur = cur.get(key, None) if isinstance(cur, dict) else None
        if cur is None:
            sys.exit(0)
        if isinstance(cur, list) and idx.isdigit():
            i=int(idx)
            cur = cur[i] if i < len(cur) else None
        else:
            cur = None
    else:
        cur = cur.get(part, None) if isinstance(cur, dict) else None
    if cur is None:
        sys.exit(0)
if isinstance(cur, bool):
    print("true" if cur else "false")
elif isinstance(cur, (int, float, str)):
    print(cur)
else:
    print(json.dumps(cur, separators=(",", ":")))' "$expr" 2>/dev/null || true
    return 0
  fi
  return 1
}

log "Environment"
info "date: $(date -Is 2>/dev/null || date)"
info "user: $(id -un 2>/dev/null || true)"
info "host: $(hostname 2>/dev/null || true)"
info "XDG_RUNTIME_DIR: ${XDG_RUNTIME_DIR:-}"
if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
  warn "XDG_RUNTIME_DIR is not set; PipeWire + systemd --user checks will likely fail (run from a normal login session; avoid sudo)."
fi
info "Headroom binaries:"
info "  headroomctl: ${HEADROOMCTL:-<not found>}"
info "  headroom: ${HEADROOM:-<not found>}"
info "  headroom-tui: ${HEADROOM_TUI:-<not found>}"
if [[ "$NON_INTERACTIVE" -eq 1 && "$READ_ONLY" -ne 1 ]]; then
  warn "--non-interactive may change audio settings (pass --read-only to skip state-changing checks)"
fi

if [[ -z "$HEADROOMCTL" ]]; then
  warn "headroomctl not found; most checks will be skipped."
  exit 2
fi

log "systemd --user"
HAS_PW_PULSE=0
SESSION_MGR_UNIT=""
if [[ "$SKIP_SYSTEMD" -eq 1 ]]; then
  info "skip: systemd checks (--skip-systemd)"
elif need_soft systemctl; then
  if ! systemctl --user show-environment >/dev/null 2>&1; then
    warn "systemctl --user cannot connect to the user bus (is your user session/systemd --user running?)"
    systemctl --user show-environment >/dev/null 2>&1 || true
    FAIL=1
  else
    if systemctl --user --quiet is-active pipewire.service; then
      info "pipewire.service: active"
    elif systemctl --user --quiet is-active pipewire.socket; then
      info "pipewire.socket: active (pipewire.service may be socket-activated)"
    else
      warn "pipewire: not active (checked pipewire.service + pipewire.socket)"
      FAIL=1
    fi

    if systemctl --user --quiet is-active wireplumber.service; then
      SESSION_MGR_UNIT="wireplumber.service"
      info "wireplumber.service: active"
    elif systemctl --user --quiet is-active pipewire-media-session.service; then
      SESSION_MGR_UNIT="pipewire-media-session.service"
      info "pipewire-media-session.service: active"
    else
      warn "session manager: not active (checked wireplumber.service + pipewire-media-session.service)"
      FAIL=1
    fi

    if systemctl --user --quiet is-active pipewire-pulse.service; then
      HAS_PW_PULSE=1
      info "pipewire-pulse.service: active"
    elif systemctl --user --quiet is-active pipewire-pulse.socket; then
      HAS_PW_PULSE=1
      info "pipewire-pulse.socket: active (pipewire-pulse.service may be socket-activated)"
    else
      info "pipewire-pulse: not active (ok if you don't use PulseAudio compatibility)"
    fi

    systemctl --user status pipewire.service pipewire.socket wireplumber.service pipewire-media-session.service pipewire-pulse.service pipewire-pulse.socket --no-pager --full 2>/dev/null || true
  fi
else
  warn "systemctl not found; skipping systemd checks."
fi

log "PipeWire basics"
need_soft pw-dump || true
need_soft pw-cli || true

try "headroomctl nodes" "$HEADROOMCTL" nodes
try "headroomctl sinks" "$HEADROOMCTL" sinks
try "headroomctl sources" "$HEADROOMCTL" sources

log "Engine + diagnostics"
echo "+ headroomctl engine status --json"
engine_status_json="$("$HEADROOMCTL" engine status --json 2>/dev/null || true)"
if [[ -n "$engine_status_json" ]]; then
  echo "$engine_status_json"
else
  warn "FAILED: headroomctl engine status --json"
  FAIL=1
fi

PIPEWIRE_OK=1
if [[ -n "$engine_status_json" ]]; then
  if grep -q '"pipewireReachable"[[:space:]]*:[[:space:]]*false' <<<"$engine_status_json"; then
    PIPEWIRE_OK=0
  fi
else
  PIPEWIRE_OK=0
fi

if [[ "$PIPEWIRE_OK" -eq 0 ]]; then
  warn "PipeWire not reachable (skipping device-control and recording checks; run on a real host with PipeWire + user session)."
fi
try "headroomctl diagnostics status --json" "$HEADROOMCTL" diagnostics status --json

log "Engine control (optional)"
if confirm "Restart PipeWire (brief audio interruption)?"; then
  try "headroomctl engine restart pipewire --json" "$HEADROOMCTL" engine restart pipewire --json
  if [[ -n "$SESSION_MGR_UNIT" ]]; then
    try "headroomctl engine restart $SESSION_MGR_UNIT --json" "$HEADROOMCTL" engine restart "$SESSION_MGR_UNIT" --json
  fi
  if [[ "$HAS_PW_PULSE" -eq 1 ]]; then
    try "headroomctl engine restart pipewire-pulse --json" "$HEADROOMCTL" engine restart pipewire-pulse --json
  fi
  try "headroomctl engine status --json (after restart)" "$HEADROOMCTL" engine status --json
fi

log "Device controls (default sink + volume/mute)"
if [[ "$PIPEWIRE_OK" -eq 0 ]]; then
  info "skip: device controls (PipeWire not reachable)"
elif [[ "$READ_ONLY" -eq 1 ]]; then
  info "skip: device controls (--read-only)"
elif ! command -v jq >/dev/null 2>&1 && ! command -v python3 >/dev/null 2>&1; then
  warn "missing dependency: jq or python3 (needed for JSON parsing); skipping device control checks"
  FAIL=1
else

default_sink_json="$("$HEADROOMCTL" default-sink get --json 2>/dev/null || echo '{}')"
default_sink_id="$(json_val '.defaultSinkId' <<<"$default_sink_json")"
configured_sink_id="$(json_val '.configuredSinkId' <<<"$default_sink_json")"

sinks_json="$("$HEADROOMCTL" sinks --json 2>/dev/null || echo '[]')"
if command -v jq >/dev/null 2>&1; then
sink_ids="$(jq -r '.[].id' <<<"$sinks_json" 2>/dev/null | head -n 3)"
elif command -v python3 >/dev/null 2>&1; then
  sink_ids="$(python3 -c 'import json,sys
data=json.load(sys.stdin)
for item in data:
    v=item.get("id")
    if v is not None:
        print(v)' 2>/dev/null <<<"$sinks_json" | head -n 3 || true)"
else
  sink_ids=""
fi
first_sink_id="$(printf "%s\n" "$sink_ids" | head -n 1 || true)"
second_sink_id="$(printf "%s\n" "$sink_ids" | sed -n '2p' || true)"

if [[ -z "$first_sink_id" || "$first_sink_id" == "null" ]]; then
  warn "no sinks detected; cannot run device control checks"
  FAIL=1
else
  volume_sink_id="$first_sink_id"
  if [[ -n "${default_sink_id:-}" && "$default_sink_id" != "null" ]]; then
    volume_sink_id="$default_sink_id"
  fi

  switch_sink_id=""
  while IFS= read -r cand; do
    if [[ -n "$cand" && "$cand" != "null" && "$cand" != "$volume_sink_id" ]]; then
      switch_sink_id="$cand"
      break
    fi
  done <<<"$sink_ids"

  if command -v jq >/dev/null 2>&1; then
    orig_volume="$(jq -r --argjson id "$volume_sink_id" '.[] | select(.id == $id) | .controls.volume // empty' <<<"$sinks_json" 2>/dev/null || true)"
    orig_mute="$(jq -r --argjson id "$volume_sink_id" '.[] | select(.id == $id) | (.controls // {}) | if has("mute") then (.mute | tostring) else empty end' <<<"$sinks_json" 2>/dev/null || true)"
  elif command -v python3 >/dev/null 2>&1; then
    orig_volume="$(python3 -c 'import json,sys
data=json.load(sys.stdin)
want=int(sys.argv[1])
for item in data:
    try:
        item_id=int(item.get("id", -1))
    except Exception:
        continue
    if item_id == want:
        c=item.get("controls") or {}
        v=c.get("volume")
        if v is None:
            sys.exit(0)
        print(v)
        sys.exit(0)' "$volume_sink_id" 2>/dev/null <<<"$sinks_json" || true)"
    orig_mute="$(python3 -c 'import json,sys
data=json.load(sys.stdin)
want=int(sys.argv[1])
for item in data:
    try:
        item_id=int(item.get("id", -1))
    except Exception:
        continue
    if item_id == want:
        c=item.get("controls") or {}
        m=c.get("mute")
        if m is None:
            sys.exit(0)
        print("true" if bool(m) else "false")
        sys.exit(0)' "$volume_sink_id" 2>/dev/null <<<"$sinks_json" || true)"
  else
    orig_volume=""
    orig_mute=""
  fi

  info "default sink id: ${default_sink_id:-<none>}"
  info "configured sink id: ${configured_sink_id:-<none>}"
  info "picked sink ids: first=$first_sink_id second=${second_sink_id:-<none>}"
  info "test sink ids: volume/mute=$volume_sink_id switch-default=${switch_sink_id:-<none>}"

  set_default_sink_verify() {
    local target_id="$1"
    local out_json
    out_json="$("$HEADROOMCTL" default-sink set "$target_id" --json 2>/dev/null || true)"
    if [[ -z "$out_json" ]]; then
      echo "error: default-sink set produced no output" >&2
      return 1
    fi
    echo "$out_json"
    local got_id
    got_id="$(json_val '.configuredSinkId' <<<"$out_json")"
    if [[ "$got_id" != "$target_id" ]]; then
      echo "error: expected configuredSinkId=$target_id but got ${got_id:-<none>}" >&2
      return 1
    fi
    return 0
  }

  if [[ -n "$switch_sink_id" && "$switch_sink_id" != "null" ]]; then
    pause "About to set default sink to id=$switch_sink_id (will restore after test)"
    try "set default sink -> $switch_sink_id (verify configured)" set_default_sink_verify "$switch_sink_id"
  fi

  pause "About to set volume on sink $volume_sink_id and toggle mute"
  try "set-volume $volume_sink_id 30%" "$HEADROOMCTL" set-volume "$volume_sink_id" 30%
  try "mute $volume_sink_id toggle" "$HEADROOMCTL" mute "$volume_sink_id" toggle

  # Best-effort restore.
  if [[ -n "$configured_sink_id" && "$configured_sink_id" != "null" ]]; then
    pause "Restore configured default sink to id=$configured_sink_id"
    "$HEADROOMCTL" default-sink set "$configured_sink_id" --json >/dev/null 2>&1 || true
  elif [[ -n "$default_sink_id" && "$default_sink_id" != "null" ]]; then
    pause "Restore default sink to id=$default_sink_id"
    "$HEADROOMCTL" default-sink set "$default_sink_id" --json >/dev/null 2>&1 || true
  fi

  if [[ -n "${orig_volume:-}" ]]; then
    pause "Restore sink $volume_sink_id volume to $orig_volume"
    "$HEADROOMCTL" set-volume "$volume_sink_id" "$orig_volume" >/dev/null 2>&1 || true
  fi

  if [[ "${orig_mute:-}" == "true" ]]; then
    pause "Restore sink $volume_sink_id mute=on"
    "$HEADROOMCTL" mute "$volume_sink_id" on >/dev/null 2>&1 || true
  elif [[ "${orig_mute:-}" == "false" ]]; then
    pause "Restore sink $volume_sink_id mute=off"
    "$HEADROOMCTL" mute "$volume_sink_id" off >/dev/null 2>&1 || true
  fi
fi
fi

log "Clock presets (optional)"
if [[ "$NON_INTERACTIVE" -ne 1 ]]; then
  "$HEADROOMCTL" engine clock presets --json 2>/dev/null | jq -r '.presets[]? | "\(.id)\t\(.name)"' 2>/dev/null || true
  pause "If you want, apply a clock preset now from another terminal: headroomctl engine clock preset <preset-id>"
fi

log "MIDI bridge (optional)"
if [[ "$NON_INTERACTIVE" -ne 1 ]]; then
  "$HEADROOMCTL" engine midi-bridge status --json 2>/dev/null || true
  pause "If you want, toggle the MIDI bridge now: headroomctl engine midi-bridge enable toggle"
fi

log "Recording smoke test"
if [[ "$PIPEWIRE_OK" -eq 0 ]]; then
  info "skip: recording smoke test (PipeWire not reachable)"
  rec_out=""
elif [[ "$READ_ONLY" -eq 1 ]]; then
  info "skip: recording smoke test (--read-only)"
else
rec_out="/tmp/headroom-host-test-{datetime}-{target}.{ext}"
pause "Start a 2s WAV recording to $rec_out (will stop automatically)"
try "record start (2s wav)" "$HEADROOMCTL" record start "$rec_out" --format wav --duration 2
try "record status --json" "$HEADROOMCTL" record status --json
fi

log "GUI/TUI manual checks"
if [[ "$NON_INTERACTIVE" -ne 1 ]]; then
  if [[ -n "$HEADROOM" ]]; then
    info "Run GUI: $HEADROOM"
    info "Try: tray icon, Mixer meters, Patchbay connect/disconnect, EQ dialog."
  else
    warn "headroom GUI binary not found."
  fi
  if [[ -n "$HEADROOM_TUI" ]]; then
    info "Run TUI: $HEADROOM_TUI"
    info "Try: Outputs reorder ([/]), Patchbay delete/connect, EQ toggle/preset."
  else
    warn "headroom-tui binary not found."
  fi
fi

log "Efficiency quick checks (optional)"
if [[ "$NON_INTERACTIVE" -ne 1 ]]; then
  if need_soft pw-cli; then
    info "Check that meter/visualizer taps stop when GUI is hidden/minimized:"
    info "  1) Start the GUI, open Mixer, enable meters"
    info "  2) Minimize (or close to tray)"
    info "  3) Run: pw-cli ls Node | grep -E 'node.name = \"headroom\\.(meter|visualizer)'"
    info ""
    info "Optionally run the automated GUI efficiency self-tests (headless: prefix QT_QPA_PLATFORM=offscreen):"
    info "  $HEADROOM --self-test-meters-hide"
    info "  $HEADROOM --self-test-meters-minimized"
    info "  $HEADROOM --self-test-meters-visible"
    info "  $HEADROOM --self-test-visualizer-hide"
    info "  $HEADROOM --self-test-visualizer-minimized"
    info ""
    info "Optional 5-minute tray-only idle audit (CPU + ctx/s proxy):"
    info "  scripts/host_efficiency_audit.sh --name headroom --duration 300"
    pause "Press Enter after checking (optional)"
  else
    warn "pw-cli not found; skipping efficiency stream checks."
  fi
fi

if [[ "$RUN_GUI_SELF_TESTS" -eq 1 ]]; then
  log "GUI self-tests"
  if [[ -z "$HEADROOM" ]]; then
    warn "headroom GUI binary not found; cannot run GUI self-tests."
    FAIL=1
  else
    run_gui_self_test() {
      if [[ -n "${QT_QPA_PLATFORM:-}" ]]; then
        "$HEADROOM" "$@"
        return 0
      fi
      # Prefer the user's session platform if available; fall back to offscreen for headless runs.
      if [[ -n "${DISPLAY:-}" || -n "${WAYLAND_DISPLAY:-}" ]]; then
        "$HEADROOM" "$@"
      else
        QT_QPA_PLATFORM=offscreen "$HEADROOM" "$@"
      fi
    }

    try "GUI self-test: meters hide" run_gui_self_test --self-test-meters-hide
    try "GUI self-test: meters minimized" run_gui_self_test --self-test-meters-minimized
    try "GUI self-test: visualizer hide" run_gui_self_test --self-test-visualizer-hide
    try "GUI self-test: visualizer minimized" run_gui_self_test --self-test-visualizer-minimized
    try "GUI self-test: meters visible" run_gui_self_test --self-test-meters-visible
  fi
fi

if [[ "$RUN_EFFICIENCY_AUDIT" -eq 1 ]]; then
  log "Efficiency audit"
  audit_script="$ROOT/scripts/host_efficiency_audit.sh"
  if [[ ! -x "$audit_script" ]]; then
    warn "missing: $audit_script"
    FAIL=1
  else
    if ! [[ "$EFFICIENCY_DURATION_S" =~ ^[0-9]+$ ]] || [[ "$EFFICIENCY_DURATION_S" -le 0 ]]; then
      warn "invalid --efficiency-duration: $EFFICIENCY_DURATION_S"
      FAIL=1
    else
      if [[ "$NON_INTERACTIVE" -ne 1 ]]; then
        pause "Ensure Headroom is running tray-only (hidden/minimized), then run a ${EFFICIENCY_DURATION_S}s idle audit"
      fi
      try "host efficiency audit (pid via name=headroom)" "$audit_script" --name headroom --duration "$EFFICIENCY_DURATION_S"
    fi
  fi
fi

if [[ "$RUN_GUI_EFFICIENCY_MATRIX" -eq 1 ]]; then
  log "GUI efficiency matrix (real X11)"
  matrix_script="$ROOT/scripts/host_gui_efficiency_matrix.sh"
  if [[ ! -x "$matrix_script" ]]; then
    warn "missing: $matrix_script"
    FAIL=1
  elif [[ -z "$HEADROOM" ]]; then
    warn "headroom GUI binary not found; cannot run GUI efficiency matrix."
    FAIL=1
  else
    display_guess="${DISPLAY:-}"
    if [[ -z "$display_guess" && -S /tmp/.X11-unix/X0 ]]; then
      display_guess=":0"
    fi
    if [[ -z "$display_guess" ]]; then
      warn "DISPLAY is not set and /tmp/.X11-unix/X0 was not found; cannot run GUI efficiency matrix."
      FAIL=1
    else
      try "GUI efficiency matrix (DISPLAY=$display_guess)" "$matrix_script" --headroom "$HEADROOM" --display "$display_guess"
    fi
  fi
fi

log "Done"
if [[ "$FAIL" -eq 0 ]]; then
  info "PASS (best-effort)"
else
  warn "FAIL (one or more checks failed)"
fi
exit "$FAIL"
