#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

HEADROOM_BIN=""
DISPLAY_VALUE="${DISPLAY:-:0}"
QT_PLATFORM="xcb"

DURATION_VISIBLE_S=60
DURATION_HIDDEN_S=300
INTERVAL_S=1

OUT_DIR=""
WITH_AUDIO_STIM=1
KEEP_HEADROOM=0
DEBUG=0

MAX_HIDDEN_CPU_AVG="0.20"
MAX_HIDDEN_CPU_MAX="1.00"
MAX_HIDDEN_CTX_MAX_PER_S=10

usage() {
  cat >&2 <<'EOF'
Usage:
  scripts/host_gui_efficiency_matrix.sh [options]

Runs a real-X11 efficiency matrix:
  1) visible (Mixer, meters forced on)
  2) minimized (window still running)
  3) tray-only (close-to-tray)

Asserts:
  - while minimized/tray-only: no headroom meter/visualizer PipeWire nodes
  - while minimized/tray-only: CPU + ctx/s stay low (best-effort thresholds)

Options:
  --headroom PATH          Path to headroom binary (default: auto-pick build_test/build/PATH)
  --display DISPLAY        X11 DISPLAY (default: $DISPLAY or :0)
  --qt-platform PLATFORM   Qt QPA platform (default: xcb)
  --visible-duration SEC   Visible phase duration (default: 60)
  --hidden-duration SEC    Minimized + tray-only duration (default: 300)
  --interval SEC           Sampling interval for host_efficiency_audit (default: 1)
  --out DIR                Output directory (default: /tmp/headroom-gui-eff-matrix-<timestamp>)
  --no-audio-stimulus      Don't start a background pw-cat stream
  --keep-headroom          Leave headroom running at the end (no cleanup kill)
  --debug                  Enable shell tracing
  --help                   Show this help
EOF
}

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing dependency: $1" >&2
    exit 1
  fi
}

have() { command -v "$1" >/dev/null 2>&1; }

re_grep_o() {
  local re="$1"
  if have rg; then
    rg -o "$re"
  else
    grep -Eo "$re"
  fi
}

float_le() {
  awk -v a="$1" -v b="$2" 'BEGIN { exit (a <= b) ? 0 : 1 }'
}

pick_headroom_bin() {
  if [[ -n "$HEADROOM_BIN" ]]; then
    if [[ ! -x "$HEADROOM_BIN" ]]; then
      echo "error: --headroom is not executable: $HEADROOM_BIN" >&2
      exit 2
    fi
    echo "$HEADROOM_BIN"
    return 0
  fi

  if [[ -x "$ROOT/build_test/headroom" ]]; then
    echo "$ROOT/build_test/headroom"
    return 0
  fi
  if [[ -x "$ROOT/build/headroom" ]]; then
    echo "$ROOT/build/headroom"
    return 0
  fi
  if command -v headroom >/dev/null 2>&1; then
    command -v headroom
    return 0
  fi
  echo "error: could not find headroom binary (pass --headroom PATH)" >&2
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --headroom)
      HEADROOM_BIN="${2:-}"
      shift 2
      ;;
    --display)
      DISPLAY_VALUE="${2:-}"
      shift 2
      ;;
    --qt-platform)
      QT_PLATFORM="${2:-}"
      shift 2
      ;;
    --visible-duration)
      DURATION_VISIBLE_S="${2:-}"
      shift 2
      ;;
    --hidden-duration)
      DURATION_HIDDEN_S="${2:-}"
      shift 2
      ;;
    --interval)
      INTERVAL_S="${2:-}"
      shift 2
      ;;
    --out)
      OUT_DIR="${2:-}"
      shift 2
      ;;
    --no-audio-stimulus)
      WITH_AUDIO_STIM=0
      shift
      ;;
    --keep-headroom)
      KEEP_HEADROOM=1
      shift
      ;;
    --debug)
      DEBUG=1
      shift
      ;;
    *)
      echo "error: unknown arg: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ "$DEBUG" -eq 1 ]]; then
  set -x
fi

need awk
need date
need mktemp
need pw-cli
need xdotool
need xwininfo

if [[ "$WITH_AUDIO_STIM" -eq 1 ]]; then
  need pw-cat
fi

if have rg; then
  HAVE_RG=1
else
  HAVE_RG=0
fi

if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
  guess_runtime_dir="/run/user/$(id -u 2>/dev/null || echo '')"
  if [[ -n "$guess_runtime_dir" && -d "$guess_runtime_dir" ]]; then
    export XDG_RUNTIME_DIR="$guess_runtime_dir"
  fi
fi

HEADROOM_BIN="$(pick_headroom_bin)"

export DISPLAY="$DISPLAY_VALUE"

if [[ -z "$OUT_DIR" ]]; then
  OUT_DIR="/tmp/headroom-gui-eff-matrix-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$OUT_DIR"
chmod 700 "$OUT_DIR" 2>/dev/null || true

eff_audit="$ROOT/scripts/host_efficiency_audit.sh"
if [[ ! -x "$eff_audit" ]]; then
  echo "error: missing required script: $eff_audit" >&2
  exit 1
fi

CONF_DIR="$(mktemp -d /tmp/headroom-conf-XXXXXX)"
mkdir -p "$CONF_DIR/maxheadroom" "$CONF_DIR/cache"
cat >"$CONF_DIR/maxheadroom/Headroom.conf" <<'INI'
[mixer]
metersMode=3
INI

FAIL=0

cleanup() {
  set +e
  [[ "$KEEP_HEADROOM" -eq 1 ]] || { [[ -n "${HR_PID:-}" ]] && kill -TERM "$HR_PID" 2>/dev/null || true; }
  [[ -n "${AUDIO_STIM_PID:-}" ]] && kill -TERM "$AUDIO_STIM_PID" 2>/dev/null || true
  rm -rf "$CONF_DIR" 2>/dev/null || true
}
trap cleanup EXIT

{
  echo "date: $(date -Is 2>/dev/null || date)"
  echo "user: $(id -un 2>/dev/null || true)"
  echo "host: $(hostname 2>/dev/null || true)"
  echo "kernel: $(uname -sr 2>/dev/null || true)"
  echo "headroom: $HEADROOM_BIN"
  echo "display: $DISPLAY_VALUE"
  echo "qt_platform: $QT_PLATFORM"
  echo "duration_visible_s: $DURATION_VISIBLE_S"
  echo "duration_hidden_s: $DURATION_HIDDEN_S"
  echo "interval_s: $INTERVAL_S"
  echo "XDG_RUNTIME_DIR: ${XDG_RUNTIME_DIR:-}"
  echo "DISPLAY (exported): ${DISPLAY:-}"
} >"$OUT_DIR/env.txt"

pw_node_count_meter() {
  pw-cli ls Node 2>/dev/null | grep -c 'node.name = "headroom.meter\.' || true
}
pw_node_count_visualizer() {
  pw-cli ls Node 2>/dev/null | grep -c 'node.name = "headroom.visualizer"' || true
}
dump_pw_headroom_nodes() {
  pw-cli ls Node 2>/dev/null | grep -E 'node.name = "headroom\.(meter\.|visualizer)"' || true
}

window_map_state() {
  local id="$1"
  xwininfo -id "$id" 2>/dev/null | awk '/Map State:/ {print $3; exit}' || true
}

list_headroom_windows() {
  local pid="${1:-}"
  local ids=""

  if [[ -n "$pid" ]]; then
    ids="$(xdotool search --pid "$pid" --name "Headroom" 2>/dev/null || true)"
    if [[ -n "$ids" ]]; then
      echo "$ids"
      return 0
    fi
  fi
  xdotool search --name "Headroom" 2>/dev/null || true
}

dump_headroom_windows() {
  local pid="$1"
  local id map w h area name

  echo "pid=$pid"
  for id in $(list_headroom_windows "$pid"); do
    map="$(window_map_state "$id")"
    w="$(xwininfo -id "$id" 2>/dev/null | awk '/Width:/ {print $2; exit}' || echo 0)"
    h="$(xwininfo -id "$id" 2>/dev/null | awk '/Height:/ {print $2; exit}' || echo 0)"
    area=$((w*h))
    name="$(xdotool getwindowname "$id" 2>/dev/null | tr '\n' ' ' || true)"
    printf "id=%s map=%s w=%s h=%s area=%s name=%s\n" "$id" "$map" "$w" "$h" "$area" "$name"
  done
}

pick_main_headroom_window() {
  local pid="$1"
  local best_viewable=""
  local best_viewable_area=0
  local best_any=""
  local best_any_area=0
  local id map w h area

  for id in $(list_headroom_windows "$pid"); do
    map="$(window_map_state "$id")"
    w="$(xwininfo -id "$id" 2>/dev/null | awk '/Width:/ {print $2; exit}' || echo 0)"
    h="$(xwininfo -id "$id" 2>/dev/null | awk '/Height:/ {print $2; exit}' || echo 0)"
    area=$((w*h))

    if [[ "$area" -le 0 ]]; then
      continue
    fi

    if [[ "$area" -gt "$best_any_area" ]]; then
      best_any_area="$area"
      best_any="$id"
    fi
    if [[ "$map" == "IsViewable" && "$area" -gt "$best_viewable_area" ]]; then
      best_viewable_area="$area"
      best_viewable="$id"
    fi
  done

  if [[ -n "$best_viewable" ]]; then
    echo "$best_viewable"
    return 0
  fi
  echo "$best_any"
}

wait_for_main_window() {
  local pid="$1"
  local win=""
  for _ in $(seq 1 200); do
    win="$(pick_main_headroom_window "$pid")"
    if [[ -n "$win" && "$(window_map_state "$win")" == "IsViewable" ]]; then
      break
    fi
    sleep 0.05
  done
  [[ -n "$win" ]] || return 1
  echo "$win"
}

phase() {
  local msg="$1"
  printf "%s %s\n" "$(date -Is 2>/dev/null || date)" "$msg" | tee -a "$OUT_DIR/phase.log" >/dev/null
}

wait_for_no_taps() {
  local label="$1"
  local timeout_s="${2:-5}"
  local start now
  start="$(date +%s 2>/dev/null || echo 0)"

  while :; do
    local meters viz
    meters="$(pw_node_count_meter)"
    viz="$(pw_node_count_visualizer)"
    if [[ "$meters" -eq 0 && "$viz" -eq 0 ]]; then
      return 0
    fi

    now="$(date +%s 2>/dev/null || echo 0)"
    if [[ "$((now - start))" -ge "$timeout_s" ]]; then
      echo "error: timed out waiting for taps to stop ($label): meters=$meters visualizer=$viz" >&2
      return 1
    fi
    sleep 0.2
  done
}

if [[ "$WITH_AUDIO_STIM" -eq 1 ]]; then
  phase "start audio stimulus (pw-cat /dev/zero)"
  pw-cat --playback --raw --format f32 --rate 48000 --channels 2 --volume 0.02 \
    --properties "node.name=Headroom-AudioStim node.description=AudioStim" - </dev/zero >/dev/null 2>&1 &
  AUDIO_STIM_PID=$!
  sleep 0.2
fi

phase "launch headroom (Mixer, metersMode=3)"
XDG_CONFIG_HOME="$CONF_DIR" \
XDG_CACHE_HOME="$CONF_DIR/cache" \
DISPLAY="$DISPLAY_VALUE" \
QT_QPA_PLATFORM="$QT_PLATFORM" \
"$HEADROOM_BIN" --tab mixer >"$OUT_DIR/headroom.log" 2>&1 &
HR_PID=$!

phase "wait for main window"
WIN="$(wait_for_main_window "$HR_PID")" || { echo "error: could not find Headroom X11 window on DISPLAY=$DISPLAY_VALUE" >&2; exit 1; }
echo "$WIN" >"$OUT_DIR/mainwin-id.txt"

xdotool windowmap "$WIN" >/dev/null 2>&1 || true
xdotool windowactivate "$WIN" >/dev/null 2>&1 || true
sleep 1.0

{
  echo "initial_map_state=$(window_map_state "$WIN")"
  echo "initial_xwininfo:"
  xwininfo -id "$WIN" 2>/dev/null | sed -n '1,25p' || true
} >"$OUT_DIR/mainwin-mapstate.txt"

dump_headroom_windows "$HR_PID" >"$OUT_DIR/windows.txt" 2>&1 || true

phase "visible: capture pw nodes + audit"
{
  echo "meters=$(pw_node_count_meter)"
  echo "visualizer=$(pw_node_count_visualizer)"
  dump_pw_headroom_nodes
} >"$OUT_DIR/pw-visible.txt"

if [[ "$DURATION_VISIBLE_S" -gt 0 ]]; then
  "$eff_audit" --pid "$HR_PID" --duration "$DURATION_VISIBLE_S" --interval "$INTERVAL_S" | tee "$OUT_DIR/eff-visible.txt"
fi

phase "minimize: ensure taps stop + audit"
pre_min_meters="$(pw_node_count_meter)"
pre_min_viz="$(pw_node_count_visualizer)"
echo "pre_minimize meters=$pre_min_meters visualizer=$pre_min_viz" | tee -a "$OUT_DIR/phase.log" >/dev/null

xdotool windowactivate "$WIN" >/dev/null 2>&1 || true
sleep 0.2
xdotool windowminimize "$WIN" >/dev/null 2>&1 || true
sleep 1.0

wait_for_no_taps "minimized" 8 || FAIL=1

map_after_min="$(window_map_state "$WIN")"
post_min_meters="$(pw_node_count_meter)"
post_min_viz="$(pw_node_count_visualizer)"
{
  echo "pre_minimize meters=$pre_min_meters visualizer=$pre_min_viz"
  echo "map_state_after_minimize=$map_after_min"
  echo "post_minimize meters=$post_min_meters visualizer=$post_min_viz"
  dump_pw_headroom_nodes
} >"$OUT_DIR/pw-minimized.txt"

if [[ "$post_min_meters" -ne 0 || "$post_min_viz" -ne 0 ]]; then
  echo "error: expected 0 taps while minimized; got meters=$post_min_meters visualizer=$post_min_viz" >&2
  FAIL=1
fi

if [[ "$DURATION_HIDDEN_S" -gt 0 ]]; then
  "$eff_audit" --pid "$HR_PID" --duration "$DURATION_HIDDEN_S" --interval "$INTERVAL_S" | tee "$OUT_DIR/eff-minimized.txt"
fi

phase "tray-only: close-to-tray (Alt+F4) + audit"
xdotool windowactivate "$WIN" >/dev/null 2>&1 || true
sleep 0.3
xdotool windowclose "$WIN" >/dev/null 2>&1 || true
sleep 1.0

wait_for_no_taps "tray-only" 8 || FAIL=1

map_after_tray="$(window_map_state "$WIN")"
if [[ "$map_after_tray" == "IsViewable" ]]; then
  xdotool windowclose "$WIN" >/dev/null 2>&1 || true
  sleep 1.0
  map_after_tray="$(window_map_state "$WIN")"
fi
post_tray_meters="$(pw_node_count_meter)"
post_tray_viz="$(pw_node_count_visualizer)"
{
  echo "map_state_after_trayonly=$map_after_tray"
  echo "post_trayonly meters=$post_tray_meters visualizer=$post_tray_viz"
  dump_pw_headroom_nodes
} >"$OUT_DIR/pw-trayonly.txt"

if [[ "$post_tray_meters" -ne 0 || "$post_tray_viz" -ne 0 ]]; then
  echo "error: expected 0 taps while tray-only; got meters=$post_tray_meters visualizer=$post_tray_viz" >&2
  FAIL=1
fi

if [[ "$DURATION_HIDDEN_S" -gt 0 ]]; then
  "$eff_audit" --pid "$HR_PID" --duration "$DURATION_HIDDEN_S" --interval "$INTERVAL_S" | tee "$OUT_DIR/eff-trayonly.txt"
fi

check_hidden_summary() {
  local label="$1"
  local path="$2"
  [[ -f "$path" ]] || return 0

  local cpu_avg cpu_max ctx_max
  cpu_avg="$(re_grep_o 'cpu_avg_pct=[0-9.]+' <"$path" | head -n 1 | cut -d= -f2 || true)"
  cpu_max="$(re_grep_o 'cpu_max_pct=[0-9.]+' <"$path" | head -n 1 | cut -d= -f2 || true)"
  ctx_max="$(re_grep_o 'ctx_max_per_s=[0-9]+' <"$path" | head -n 1 | cut -d= -f2 || true)"
  if [[ -z "$cpu_avg" || -z "$cpu_max" || -z "$ctx_max" ]]; then
    echo "warn: could not parse efficiency summary for $label ($path)" >&2
    return 0
  fi

  if ! float_le "$cpu_avg" "$MAX_HIDDEN_CPU_AVG"; then
    echo "error: $label cpu_avg_pct=$cpu_avg exceeds $MAX_HIDDEN_CPU_AVG" >&2
    FAIL=1
  fi
  if ! float_le "$cpu_max" "$MAX_HIDDEN_CPU_MAX"; then
    echo "error: $label cpu_max_pct=$cpu_max exceeds $MAX_HIDDEN_CPU_MAX" >&2
    FAIL=1
  fi
  if [[ "$ctx_max" -gt "$MAX_HIDDEN_CTX_MAX_PER_S" ]]; then
    echo "error: $label ctx_max_per_s=$ctx_max exceeds $MAX_HIDDEN_CTX_MAX_PER_S" >&2
    FAIL=1
  fi
}

check_hidden_summary "minimized" "$OUT_DIR/eff-minimized.txt"
check_hidden_summary "tray-only" "$OUT_DIR/eff-trayonly.txt"

phase "done (out=$OUT_DIR)"
echo "out=$OUT_DIR"

if [[ "$FAIL" -eq 0 ]]; then
  echo "PASS"
  exit 0
fi
echo "FAIL (see logs in $OUT_DIR)" >&2
exit 1
