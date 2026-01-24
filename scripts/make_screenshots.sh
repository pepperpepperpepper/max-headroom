#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

OUT_DIR="${1:-$ROOT/screenshots}"
mkdir -p "$OUT_DIR"

RUNTIME_DIR="/tmp/headroom-runtime-$RANDOM-$RANDOM"
mkdir -p "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"

PIPEWIRE_LOG_OUT="/tmp/headroom-pipewire.out"
PIPEWIRE_LOG_ERR="/tmp/headroom-pipewire.err"
WIREPLUMBER_LOG_OUT="/tmp/headroom-wireplumber.out"
WIREPLUMBER_LOG_ERR="/tmp/headroom-wireplumber.err"

cleanup() {
  set +e
  if [[ -n "${HEADROOM_PID:-}" ]]; then kill -TERM "$HEADROOM_PID" 2>/dev/null || true; fi
  if [[ -n "${TONE1_PID:-}" ]]; then kill -TERM "$TONE1_PID" 2>/dev/null || true; fi
  if [[ -n "${TONE2_PID:-}" ]]; then kill -TERM "$TONE2_PID" 2>/dev/null || true; fi
  if [[ -n "${WIREPLUMBER_PID:-}" ]]; then kill -TERM "$WIREPLUMBER_PID" 2>/dev/null || true; fi
  if [[ -n "${PIPEWIRE_PID:-}" ]]; then kill -TERM "$PIPEWIRE_PID" 2>/dev/null || true; fi
  if [[ -n "${DBUS_PID:-}" ]]; then kill -TERM "$DBUS_PID" 2>/dev/null || true; fi
  rm -rf "$RUNTIME_DIR" 2>/dev/null || true
}
trap cleanup EXIT

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing dependency: $1" >&2
    exit 1
  fi
}

need pipewire
need pw-cli
need pw-cat
need pw-dump
need wireplumber
need dbus-daemon
need jq
need ffmpeg

echo "[1/5] Build"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/build" -j >/dev/null

echo "[2/5] Start private PipeWire + WirePlumber (snd-aloop preferred)"

# If we can, load snd-aloop and relax /dev/snd permissions so an unprivileged PipeWire can open it.
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

for _ in $(seq 1 200); do
  if XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-dump -r pipewire-0 | jq -e 'any(.[]; .type=="PipeWire:Interface:Node" and (.info.props["media.class"]=="Audio/Sink"))' >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
done

echo "[3/5] Seed demo graph (loopback device + null sink + test tones)"

# Extra virtual sink so screenshots show sink ordering.
XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-cli -r pipewire-0-manager create-node spa-node-factory \
  factory.name=support.null-audio-sink node.name=Headroom-NullSink2 node.description=NullSink2 \
  media.class=Audio/Sink object.linger=true >/dev/null

# Keep screenshots deterministic and avoid mutating the user's real config.
export XDG_CONFIG_HOME="$RUNTIME_DIR/config"
export XDG_CACHE_HOME="$RUNTIME_DIR/cache"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"

# Prefer the snd-aloop ALSA sink if it exists; otherwise fall back to the virtual sink.
PREFERRED_SINK="$(
  XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-dump -r pipewire-0 | jq -r '
    [.[] | select(.type=="PipeWire:Interface:Node")
      | select(.info.props["media.class"]=="Audio/Sink")
      | .info.props["node.name"]] as $sinks
    | ($sinks | map(select(test("snd_aloop"))) | .[0])
      // ($sinks | .[0])
      // empty
  '
)"
if [[ -z "$PREFERRED_SINK" ]]; then
  PREFERRED_SINK="Headroom-NullSink2"
fi

DEMO_OPUS="$ROOT/testdata/audio/demo.opus"
if [[ -f "$DEMO_OPUS" ]]; then
  echo "Using demo audio: $DEMO_OPUS"
else
  echo "warn: missing $DEMO_OPUS; falling back to generated sine tones" >&2
fi

# Two playback streams (creates Stream/Output/Audio nodes).
if [[ -f "$DEMO_OPUS" ]]; then
  ffmpeg -hide_banner -loglevel error -stream_loop -1 -i "$DEMO_OPUS" -f f32le -ac 2 -ar 48000 - 2>/dev/null \
    | XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-cat --remote pipewire-0 --playback --raw --format f32 --rate 48000 --channels 2 \
        --target "$PREFERRED_SINK" --properties "node.name=Headroom-ToneA node.description=ToneA" - >/dev/null 2>&1 &
else
  ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=220:sample_rate=48000:duration=120" -f f32le -ac 2 -ar 48000 - 2>/dev/null \
    | XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-cat --remote pipewire-0 --playback --raw --format f32 --rate 48000 --channels 2 \
        --target "$PREFERRED_SINK" --properties "node.name=Headroom-ToneA node.description=ToneA" - >/dev/null 2>&1 &
fi
TONE1_PID=$!

if [[ -f "$DEMO_OPUS" ]]; then
  ffmpeg -hide_banner -loglevel error -stream_loop -1 -i "$DEMO_OPUS" -af "volume=0.85" -f f32le -ac 2 -ar 48000 - 2>/dev/null \
    | XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-cat --remote pipewire-0 --playback --raw --format f32 --rate 48000 --channels 2 \
        --target Headroom-NullSink2 --properties "node.name=Headroom-ToneB node.description=ToneB" - >/dev/null 2>&1 &
else
  ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=120" -f f32le -ac 2 -ar 48000 - 2>/dev/null \
    | XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-cat --remote pipewire-0 --playback --raw --format f32 --rate 48000 --channels 2 \
        --target Headroom-NullSink2 --properties "node.name=Headroom-ToneB node.description=ToneB" - >/dev/null 2>&1 &
fi
TONE2_PID=$!

# Enable EQ for the virtual sink so the Patchbay screenshot includes the EQ node.
XDG_RUNTIME_DIR="$RUNTIME_DIR" "$ROOT/build/headroomctl" eq enable Headroom-NullSink2 on >/dev/null 2>&1 || true

echo "[4/5] Capture screenshots (offscreen)"
export XDG_RUNTIME_DIR="$RUNTIME_DIR"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

"$ROOT/build/headroom" --tab mixer --screenshot "$OUT_DIR/mixer.png" --screenshot-delay-ms 700 >/dev/null 2>&1
"$ROOT/build/headroom" --tab visualizer --tap-target "$PREFERRED_SINK" --tap-capture-sink --screenshot "$OUT_DIR/visualizer.png" --screenshot-delay-ms 1800 >/dev/null 2>&1
"$ROOT/build/headroom" --tab patchbay --screenshot "$OUT_DIR/patchbay.png" --screenshot-wait-node "headroom\\.eq\\." --screenshot-wait-timeout-ms 15000 --screenshot-delay-ms 900 >/dev/null 2>&1
"$ROOT/build/headroom" --tab graph --screenshot "$OUT_DIR/graph.png" --screenshot-delay-ms 900 >/dev/null 2>&1
"$ROOT/build/headroom" --screenshot-window settings --screenshot "$OUT_DIR/settings.png" --screenshot-delay-ms 900 >/dev/null 2>&1
"$ROOT/build/headroom" --screenshot-window eq --screenshot "$OUT_DIR/eq.png" --screenshot-delay-ms 900 >/dev/null 2>&1
"$ROOT/build/headroom" --screenshot-window engine --screenshot "$OUT_DIR/engine.png" --screenshot-delay-ms 900 >/dev/null 2>&1
"$ROOT/scripts/make_tray_demo_screenshots.sh" "$OUT_DIR" >/dev/null 2>&1 \
  || "$ROOT/scripts/make_tray_screenshot.sh" "$OUT_DIR/tray-menu.png" >/dev/null 2>&1 \
  || true

echo "[5/5] Done"
echo "Wrote:"
ls -1 "$OUT_DIR"/*.png
