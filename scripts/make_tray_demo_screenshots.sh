#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

OUT_DIR="${1:-$ROOT/screenshots}"
mkdir -p "$OUT_DIR"

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing dependency: $1" >&2
    exit 1
  fi
}

need xvfb-run
need openbox
need stalonetray
need xdotool
need import
need sha256sum
need pipewire
need pw-cli
need pw-dump
need pw-metadata
need wireplumber
need dbus-daemon
need jq

RUNTIME_DIR="/tmp/headroom-tray-demo-runtime-$RANDOM-$RANDOM"
mkdir -p "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"

export XDG_CONFIG_HOME="$RUNTIME_DIR/config"
export XDG_CACHE_HOME="$RUNTIME_DIR/cache"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"

PIPEWIRE_LOG_OUT="/tmp/headroom-tray-demo-pipewire.out"
PIPEWIRE_LOG_ERR="/tmp/headroom-tray-demo-pipewire.err"
WIREPLUMBER_LOG_OUT="/tmp/headroom-tray-demo-wireplumber.out"
WIREPLUMBER_LOG_ERR="/tmp/headroom-tray-demo-wireplumber.err"

cleanup() {
  set +e
  [[ -n "${WIREPLUMBER_PID:-}" ]] && kill -TERM "$WIREPLUMBER_PID" 2>/dev/null || true
  [[ -n "${PIPEWIRE_PID:-}" ]] && kill -TERM "$PIPEWIRE_PID" 2>/dev/null || true
  [[ -n "${DBUS_PID:-}" ]] && kill -TERM "$DBUS_PID" 2>/dev/null || true
  rm -rf "$RUNTIME_DIR" 2>/dev/null || true
}
trap cleanup EXIT

maybe_build() {
  if [[ -x "$ROOT/build/headroom" && -x "$ROOT/build/headroomctl" ]]; then
    return 0
  fi
  echo "info: missing ./build outputs; building first" >&2
  cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$ROOT/build" -j >/dev/null
}

echo "[1/4] Ensure build"
maybe_build

echo "[2/4] Start private PipeWire + WirePlumber"

# If available, try to load snd-aloop so WirePlumber can create a controllable sink.
if command -v sudo >/dev/null 2>&1; then
  sudo -n modprobe snd-aloop >/dev/null 2>&1 || true
  if [[ -d /dev/snd ]]; then
    sudo -n chmod a+rw /dev/snd/* >/dev/null 2>&1 || true
  fi
fi

# Private DBus session bus for WirePlumber.
DBUS_OUT="$(dbus-daemon --session --fork --nopidfile --print-address=1 --print-pid=1)"
export DBUS_SESSION_BUS_ADDRESS="$(echo "$DBUS_OUT" | head -n 1)"
DBUS_PID="$(echo "$DBUS_OUT" | tail -n 1)"

XDG_RUNTIME_DIR="$RUNTIME_DIR" nohup pipewire -c /usr/share/pipewire/pipewire.conf >"$PIPEWIRE_LOG_OUT" 2>"$PIPEWIRE_LOG_ERR" &
PIPEWIRE_PID=$!

for _ in $(seq 1 200); do
  [[ -S "$RUNTIME_DIR/pipewire-0" ]] && [[ -S "$RUNTIME_DIR/pipewire-0-manager" ]] && break
  sleep 0.05
done

XDG_RUNTIME_DIR="$RUNTIME_DIR" nohup env PIPEWIRE_REMOTE=pipewire-0-manager wireplumber >"$WIREPLUMBER_LOG_OUT" 2>"$WIREPLUMBER_LOG_ERR" &
WIREPLUMBER_PID=$!

for _ in $(seq 1 240); do
  if XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-dump -r pipewire-0 | jq -e 'any(.[]; .type=="PipeWire:Interface:Node" and (.info.props["media.class"]=="Audio/Sink"))' >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
done

# Prefer an snd-aloop ALSA sink when available; otherwise take the first sink.
SINK_ID="$(
  XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-dump -r pipewire-0 | jq -r '
    [.[] | select(.type=="PipeWire:Interface:Node")
      | select(.info.props["media.class"]=="Audio/Sink")
      | {id:.id, name:(.info.props["node.name"]//""), desc:(.info.props["node.description"]//"")} ] as $sinks
    | ($sinks | map(select(.name | test("snd_aloop"))) | .[0].id)
      // ($sinks | .[0].id)
      // empty
  '
)"
if [[ -z "$SINK_ID" || "$SINK_ID" == "null" ]]; then
  echo "Could not find a controllable Audio/Sink node for tray demo." >&2
  echo "PipeWire nodes:" >&2
  XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-dump -r pipewire-0 | jq -r '.[] | select(.type=="PipeWire:Interface:Node") | "\(.id)\t\(.info.props["media.class"]//"")\t\(.info.props["node.name"]//"")\t\(.info.props["node.description"]//"")"' >&2 || true
  exit 1
fi

# Ensure the tray controls the same sink we're manipulating in the demo.
XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-metadata -r pipewire-0 -n default 0 default.audio.sink "$SINK_ID" Spa:Id >/dev/null 2>&1 || true
XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-metadata -r pipewire-0 -n default 0 default.configured.audio.sink "$SINK_ID" Spa:Id >/dev/null 2>&1 || true
XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-metadata -r pipewire-0-manager -n default 0 default.audio.sink "$SINK_ID" Spa:Id >/dev/null 2>&1 || true
XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-metadata -r pipewire-0-manager -n default 0 default.configured.audio.sink "$SINK_ID" Spa:Id >/dev/null 2>&1 || true

# Seed a profile so the tray menu's Profiles submenu is non-empty.
XDG_RUNTIME_DIR="$RUNTIME_DIR" "$ROOT/build/headroomctl" patchbay save testprofile >/dev/null 2>&1 || true
XDG_RUNTIME_DIR="$RUNTIME_DIR" "$ROOT/build/headroomctl" patchbay apply testprofile >/dev/null 2>&1 || true

# Start from a known state (retry: headroomctl may race initial graph discovery).
for _ in $(seq 1 40); do
  if XDG_RUNTIME_DIR="$RUNTIME_DIR" "$ROOT/build/headroomctl" set-volume "$SINK_ID" 100% >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
for _ in $(seq 1 40); do
  if XDG_RUNTIME_DIR="$RUNTIME_DIR" "$ROOT/build/headroomctl" mute "$SINK_ID" off >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

echo "[3/4] Launch Xvfb + tray + Headroom; capture tray demo sequence"
SCREEN="1100x700x24"

xvfb-run -a -s "-screen 0 $SCREEN -ac -nolisten tcp -extension GLX" env \
  XDG_RUNTIME_DIR="$RUNTIME_DIR" \
  XDG_CONFIG_HOME="$XDG_CONFIG_HOME" \
  XDG_CACHE_HOME="$XDG_CACHE_HOME" \
  ROOT="$ROOT" \
  OUT_DIR="$OUT_DIR" \
  SINK_ID="$SINK_ID" \
  bash -lc '
    set -euo pipefail
    if [[ "${HEADROOM_TRAY_DEMO_DEBUG:-}" == "1" ]]; then
      set -x
    fi

    wait_for_window() {
      local name="$1"
      local win=""
      for _ in $(seq 1 160); do
        win="$(xdotool search --onlyvisible --name "$name" 2>/dev/null | head -n 1 || true)"
        [[ -n "$win" ]] && break
        sleep 0.05
      done
      [[ -n "$win" ]] || return 1
      echo "$win"
    }

    capture_root() {
      local path="$1"
      # Give Qt time to paint (and avoid the old “every image is the same” issue).
      sleep 0.25
      import -window root "$path"
    }

    capture_root_until_hash_differs() {
      local baseline_hash="$1"
      local out_path="$2"
      local tmp_path
      tmp_path="$(mktemp -p "${OUT_DIR}" ".tmp-shot-XXXXXX.png")"

      for _ in $(seq 1 30); do
        capture_root "$tmp_path"
        local h
        h="$(sha256sum "$tmp_path" | awk "{print \$1}")"
        if [[ "$h" != "$baseline_hash" ]]; then
          mv -f "$tmp_path" "$out_path"
          return 0
        fi
        sleep 0.25
      done

      echo "error: screenshot did not change after retries: $out_path" >&2
      mv -f "$tmp_path" "$out_path"
      return 1
    }

    tray_center_right_click() {
      local tray_win
      tray_win="$(xdotool search --classname stalonetray 2>/dev/null | head -n 1 || true)"
      [[ -n "$tray_win" ]] || { echo "Could not find stalonetray window" >&2; return 1; }
      eval "$(xdotool getwindowgeometry --shell "$tray_win")"
      TRAY_CX=$((X + WIDTH / 2))
      TRAY_CY=$((Y + HEIGHT / 2))
      xdotool mousemove --sync "$TRAY_CX" "$TRAY_CY" click 3
    }

    menu_open() {
      tray_center_right_click
      # Let the menu map.
      sleep 0.2
      # Nudge the pointer into the menu (away from the volume widget) so we can resolve its window id reliably.
      xdotool mousemove --sync $((TRAY_CX + 50)) $((TRAY_CY + 15))
    }

    menu_geometry() {
      local tray_win
      tray_win="$(xdotool search --classname stalonetray 2>/dev/null | head -n 1 || true)"

      # Prefer resolving the menu window from the pointer location; active-window focus is flaky under Xvfb.
      for _ in $(seq 1 120); do
        local MX MY SCREEN WINDOW
        eval "$(xdotool getmouselocation --shell 2>/dev/null || true)"
        if [[ -n "${WINDOW:-}" && -n "$tray_win" && "$WINDOW" != "$tray_win" ]]; then
          local X Y WIDTH HEIGHT
          eval "$(xdotool getwindowgeometry --shell "$WINDOW" 2>/dev/null || true)"
          if [[ -n "${WIDTH:-}" && "$WIDTH" -gt 80 && -n "${HEIGHT:-}" && "$HEIGHT" -gt 80 ]]; then
            echo "$WINDOW $X $Y $WIDTH $HEIGHT"
            return 0
          fi
        fi
        sleep 0.05
      done

      # Fallback: attempt active window.
      local menu_win=""
      for _ in $(seq 1 40); do
        menu_win="$(xdotool getactivewindow 2>/dev/null || true)"
        [[ -n "$menu_win" ]] && break
        sleep 0.05
      done
      [[ -n "$menu_win" ]] || return 1

      local X Y WIDTH HEIGHT
      eval "$(xdotool getwindowgeometry --shell "$menu_win" 2>/dev/null || true)"
      [[ -n "${WIDTH:-}" && -n "${HEIGHT:-}" ]] || return 1
      echo "$menu_win $X $Y $WIDTH $HEIGHT"
    }

    menu_click_row_ratio() {
      local ratio="$1"
      local geom=""
      geom="$(menu_geometry || true)"
      [[ -n "$geom" ]] || return 0
      local menu_win mx my mw mh
      read -r menu_win mx my mw mh <<<"$geom"
      local x=$((mx + mw / 2))
      local y=$((my + (mh * ratio) / 100))
      xdotool mousemove --sync "$x" "$y" click 1
    }

    menu_click_offset_y() {
      local offset="$1"
      local geom=""
      geom="$(menu_geometry || true)"
      [[ -n "$geom" ]] || return 0
      local menu_win mx my mw mh
      read -r menu_win mx my mw mh <<<"$geom"
      local x=$((mx + mw / 2))
      local y=$((my + offset))
      xdotool mousemove --sync "$x" "$y" click 1
    }

    menu_hover_row_ratio() {
      local ratio="$1"
      local geom=""
      geom="$(menu_geometry || true)"
      [[ -n "$geom" ]] || return 0
      local menu_win mx my mw mh
      read -r menu_win mx my mw mh <<<"$geom"
      local x=$((mx + mw / 2))
      local y=$((my + (mh * ratio) / 100))
      xdotool mousemove --sync "$x" "$y"
    }

    menu_close() {
      xdotool key --clearmodifiers Escape || true
      sleep 0.1
      xdotool key --clearmodifiers Escape || true
      sleep 0.1
    }

    menu_focus() {
      local geom
      geom="$(menu_geometry || true)"
      [[ -n "$geom" ]] || return 1
      local menu_win mx my mw mh
      read -r menu_win mx my mw mh <<<"$geom"
      xdotool windowfocus "$menu_win" 2>/dev/null || true
      return 0
    }

    ctl_retry() {
      local tries="$1"
      shift
      for _ in $(seq 1 "$tries"); do
        if XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" "$ROOT/build/headroomctl" "$@" >/dev/null 2>&1; then
          return 0
        fi
        sleep 0.1
      done
      return 1
    }

    wait_sink_volume_pct() {
      local expected="$1"
      for _ in $(seq 1 60); do
        local pct
        pct="$(
          XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" "$ROOT/build/headroomctl" sinks --json 2>/dev/null \
            | jq -r --argjson id "$SINK_ID" '"'"'.[] | select(.id==$id) | (.controls.volume*100 | round)'"'"' \
            | head -n 1
        )"
        if [[ -n "$pct" && "$pct" != "null" && "$pct" -eq "$expected" ]]; then
          return 0
        fi
        sleep 0.1
      done
      return 1
    }

    stalonetray --geometry 1x1+6+6 --decorations none --window-type dock --skip-taskbar --icon-size 24 --slot-size 24 --log-level err &
    TRAY_PID=$!

    # A window manager is required so "Open Mixer/Patchbay" results in a properly mapped/visible window.
    openbox --sm-disable >/dev/null 2>&1 &
    WM_PID=$!

    # Give the tray time to claim the system-tray selection.
    sleep 0.35

    "$ROOT/build/headroom" &
    HEADROOM_PID=$!

    # Wait for the main window to appear, then close it to hide-to-tray.
    echo "demo: wait for main window"
    WIN="$(wait_for_window "Headroom" || true)"
    if [[ -n "${WIN:-}" ]]; then
      # Let PipeWire discovery settle so the tray controls are enabled.
      sleep 0.9
      xdotool windowminimize "$WIN" 2>/dev/null || xdotool windowunmap "$WIN" 2>/dev/null || true
    fi

    # Give the tray icon time to dock.
    sleep 0.5

    echo "demo: capture tray icon"
    capture_root "$OUT_DIR/tray-icon.png"

    # Menu open (baseline)
    echo "demo: capture menu open"
    menu_open
    capture_root "$OUT_DIR/tray-menu.png"
    menu_close

    BASELINE_HASH="$(sha256sum "$OUT_DIR/tray-menu.png" | awk "{print \$1}")"

    # Profiles submenu open (hover)
    echo "demo: capture profiles submenu"
    menu_open
    menu_hover_row_ratio 78
    sleep 0.35
    capture_root "$OUT_DIR/tray-menu-profiles.png"
    menu_close

    # Muted state (toggle via tray menu, then capture menu).
    echo "demo: capture muted state"
    menu_open
    menu_focus || true
    xdotool key --clearmodifiers Down Down Return
    sleep 0.35
    menu_open
    capture_root "$OUT_DIR/tray-menu-muted.png"
    menu_close
    # Unmute to restore for subsequent steps.
    menu_open
    menu_focus || true
    xdotool key --clearmodifiers Down Down Return
    sleep 0.35

    # Slider positions: set volume externally, then capture open menu.
    echo "demo: capture volume 30%"
    ctl_retry 40 set-volume "$SINK_ID" 30% || {
      echo "error: failed to set sink $SINK_ID volume to 30%" >&2
      XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" "$ROOT/build/headroomctl" sinks >&2 || true
      exit 1
    }
    wait_sink_volume_pct 30 || {
      echo "error: sink $SINK_ID did not report 30% within timeout" >&2
      XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" "$ROOT/build/headroomctl" sinks >&2 || true
      exit 1
    }
    menu_open
    capture_root_until_hash_differs "$BASELINE_HASH" "$OUT_DIR/tray-menu-vol30.png"
    menu_close
    VOL30_HASH="$(sha256sum "$OUT_DIR/tray-menu-vol30.png" | awk "{print \$1}")"

    echo "demo: capture volume 80%"
    ctl_retry 40 set-volume "$SINK_ID" 80% || {
      echo "error: failed to set sink $SINK_ID volume to 80%" >&2
      XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" "$ROOT/build/headroomctl" sinks >&2 || true
      exit 1
    }
    wait_sink_volume_pct 80 || {
      echo "error: sink $SINK_ID did not report 80% within timeout" >&2
      XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" "$ROOT/build/headroomctl" sinks >&2 || true
      exit 1
    }
    menu_open
    capture_root_until_hash_differs "$VOL30_HASH" "$OUT_DIR/tray-menu-vol80.png"
    menu_close

    # Open Mixer from tray
    echo "demo: open mixer from tray"
    menu_open
    if [[ "${HEADROOM_TRAY_DEMO_DEBUG:-}" == "1" ]]; then
      eval "$(xdotool getmouselocation --shell 2>/dev/null || true)"
      echo "debug: pointer after menu_open: MX=${MX:-} MY=${MY:-} WINDOW=${WINDOW:-}" >&2
      if [[ -n "${WINDOW:-}" ]]; then
        eval "$(xdotool getwindowgeometry --shell "$WINDOW" 2>/dev/null || true)"
        echo "debug: window under pointer geometry: X=${X:-} Y=${Y:-} W=${WIDTH:-} H=${HEIGHT:-}" >&2
      fi
    fi
    xdotool click 1
    sleep 0.5
    WIN="$(wait_for_window "Headroom" || true)"
    if [[ -z "${WIN:-}" ]]; then
      echo "Expected Headroom window after Open Mixer" >&2
      echo "debug: xdotool search --name Headroom:" >&2
      xdotool search --name "Headroom" 2>/dev/null >&2 || true
      echo "debug: xdotool search --onlyvisible --name Headroom:" >&2
      xdotool search --onlyvisible --name "Headroom" 2>/dev/null >&2 || true
      echo "debug: xwininfo -root -tree | grep Headroom:" >&2
      xwininfo -root -tree 2>/dev/null | grep -i Headroom >&2 || true
      small_win="$(
        xwininfo -root -tree 2>/dev/null | awk "/\\\"Headroom\\\"/ && / 1x1\\\\+/{print \$1; exit}"
      )"
      if [[ -n "${small_win:-}" ]]; then
        echo "debug: xprop -id $small_win:" >&2
        xprop -id "$small_win" 2>/dev/null >&2 || true
      fi
      echo "debug: try tray icon left-click (should open Mixer via activated signal)" >&2
      xdotool mousemove --sync "$TRAY_CX" "$TRAY_CY" click 1 2>/dev/null || true
      sleep 0.6
      echo "debug: xwininfo -root -tree | grep Headroom (after tray left-click):" >&2
      xwininfo -root -tree 2>/dev/null | grep -i Headroom >&2 || true
      exit 1
    fi
    capture_root "$OUT_DIR/tray-open-mixer.png"
    xdotool windowminimize "$WIN" 2>/dev/null || xdotool windowunmap "$WIN" 2>/dev/null || true
    sleep 0.25

    # Open Patchbay from tray
    echo "demo: open patchbay from tray"
    menu_open
    xdotool mousemove --sync $((TRAY_CX + 50)) $((TRAY_CY + 38)) click 1
    sleep 0.5
    WIN="$(wait_for_window "Headroom" || true)"
    [[ -n "${WIN:-}" ]] || { echo "Expected Headroom window after Open Patchbay" >&2; exit 1; }
    capture_root "$OUT_DIR/tray-open-patchbay.png"
    xdotool windowminimize "$WIN" 2>/dev/null || xdotool windowunmap "$WIN" 2>/dev/null || true
    sleep 0.15

    kill -TERM "$HEADROOM_PID" 2>/dev/null || true
    kill -TERM "$TRAY_PID" 2>/dev/null || true
    kill -TERM "$WM_PID" 2>/dev/null || true
    wait "$HEADROOM_PID" 2>/dev/null || true
    wait "$TRAY_PID" 2>/dev/null || true
    wait "$WM_PID" 2>/dev/null || true
  '

echo "[4/4] Verify screenshots are unique"
declare -a tray_files=(
  "$OUT_DIR/tray-icon.png"
  "$OUT_DIR/tray-menu.png"
  "$OUT_DIR/tray-menu-profiles.png"
  "$OUT_DIR/tray-menu-muted.png"
  "$OUT_DIR/tray-menu-vol30.png"
  "$OUT_DIR/tray-menu-vol80.png"
  "$OUT_DIR/tray-open-mixer.png"
  "$OUT_DIR/tray-open-patchbay.png"
)

for f in "${tray_files[@]}"; do
  [[ -f "$f" ]] || { echo "Missing tray screenshot: $f" >&2; exit 1; }
done

dupes="$(sha256sum "${tray_files[@]}" | awk "{print \$1}" | sort | uniq -d | wc -l | tr -d " ")"
if [[ "$dupes" != "0" ]]; then
  echo "error: duplicate tray screenshots detected (some images are identical)" >&2
  sha256sum "${tray_files[@]}" >&2
  exit 1
fi

echo "Wrote:"
printf "  %s\n" "${tray_files[@]}"
