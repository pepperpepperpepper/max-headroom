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

cleanup() {
  set +e
  if [[ -n "${HEADROOM_PID:-}" ]]; then kill -TERM "$HEADROOM_PID" 2>/dev/null || true; fi
  if [[ -n "${TONE1_PID:-}" ]]; then kill -TERM "$TONE1_PID" 2>/dev/null || true; fi
  if [[ -n "${TONE2_PID:-}" ]]; then kill -TERM "$TONE2_PID" 2>/dev/null || true; fi
  if [[ -n "${PIPEWIRE_PID:-}" ]]; then kill -TERM "$PIPEWIRE_PID" 2>/dev/null || true; fi
  rm -rf "$RUNTIME_DIR" 2>/dev/null || true
}
trap cleanup EXIT

echo "[1/5] Build"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/build" -j >/dev/null

echo "[2/5] Start PipeWire"
XDG_RUNTIME_DIR="$RUNTIME_DIR" nohup pipewire -c /usr/share/pipewire/pipewire.conf >"$PIPEWIRE_LOG_OUT" 2>"$PIPEWIRE_LOG_ERR" &
PIPEWIRE_PID=$!

for _ in $(seq 1 200); do
  [[ -S "$RUNTIME_DIR/pipewire-0" ]] && break
  sleep 0.05
done

echo "[3/5] Seed demo graph (null sink + test tones)"
XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-cli -r pipewire-0-manager create-node spa-node-factory \
  factory.name=support.null-audio-sink node.name=Headroom-NullSink node.description=NullSink \
  media.class=Audio/Sink object.linger=true >/dev/null

XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-cli -r pipewire-0-manager create-node spa-node-factory \
  factory.name=support.null-audio-sink node.name=Headroom-NullSink-2 node.description=NullSink2 \
  media.class=Audio/Sink object.linger=true >/dev/null

# Two sine-wave playback streams (creates Stream/Output/Audio nodes)
ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=220:sample_rate=48000:duration=120" -f f32le -ac 2 -ar 48000 - \
  | XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-cat --remote pipewire-0 --playback --raw --format f32 --rate 48000 --channels 2 \
      --target Headroom-NullSink --properties "node.name=Headroom-ToneA node.description=ToneA" - >/dev/null 2>&1 &
TONE1_PID=$!

ffmpeg -hide_banner -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=120" -f f32le -ac 2 -ar 48000 - \
  | XDG_RUNTIME_DIR="$RUNTIME_DIR" pw-cat --remote pipewire-0 --playback --raw --format f32 --rate 48000 --channels 2 \
      --target Headroom-NullSink-2 --properties "node.name=Headroom-ToneB node.description=ToneB" - >/dev/null 2>&1 &
TONE2_PID=$!

echo "[4/5] Capture screenshots (offscreen)"
export XDG_RUNTIME_DIR="$RUNTIME_DIR"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

"$ROOT/build/headroom" --tab mixer --screenshot "$OUT_DIR/mixer.png" --screenshot-delay-ms 700 >/dev/null 2>&1
HEADROOM_DEMO_VISUALIZER=1 "$ROOT/build/headroom" --tab visualizer --screenshot "$OUT_DIR/visualizer.png" --screenshot-delay-ms 1400 >/dev/null 2>&1
"$ROOT/build/headroom" --tab patchbay --screenshot "$OUT_DIR/patchbay.png" --screenshot-delay-ms 900 >/dev/null 2>&1
"$ROOT/build/headroom" --tab graph --screenshot "$OUT_DIR/graph.png" --screenshot-delay-ms 900 >/dev/null 2>&1
"$ROOT/build/headroom" --screenshot-window settings --screenshot "$OUT_DIR/settings.png" --screenshot-delay-ms 900 >/dev/null 2>&1
"$ROOT/build/headroom" --screenshot-window eq --screenshot "$OUT_DIR/eq.png" --screenshot-delay-ms 900 >/dev/null 2>&1
"$ROOT/build/headroom" --screenshot-window engine --screenshot "$OUT_DIR/engine.png" --screenshot-delay-ms 900 >/dev/null 2>&1
"$ROOT/scripts/make_tray_screenshot.sh" "$OUT_DIR/tray-menu.png" >/dev/null 2>&1 || true

echo "[5/5] Done"
echo "Wrote:"
ls -1 "$OUT_DIR"/*.png
