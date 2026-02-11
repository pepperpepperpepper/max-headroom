#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage:
  run_private_pipewire.sh -- <command...>

Runs <command...> inside a private PipeWire + WirePlumber + DBus session with a temporary XDG_RUNTIME_DIR.
EOF
}

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing dependency: $1" >&2
    exit 1
  fi
}

if [[ $# -lt 2 ]] || [[ "$1" != "--" ]]; then
  usage
  exit 2
fi
shift

need dbus-daemon
need pipewire
need pw-cli
need pw-dump
need wireplumber

RUNTIME_DIR="$(mktemp -d "/tmp/headroom-test-runtime-XXXXXX")"
chmod 700 "$RUNTIME_DIR"

export XDG_RUNTIME_DIR="$RUNTIME_DIR"
export XDG_CONFIG_HOME="$RUNTIME_DIR/config"
export XDG_CACHE_HOME="$RUNTIME_DIR/cache"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"

PIPEWIRE_LOG_OUT="$RUNTIME_DIR/pipewire.out"
PIPEWIRE_LOG_ERR="$RUNTIME_DIR/pipewire.err"
WIREPLUMBER_LOG_OUT="$RUNTIME_DIR/wireplumber.out"
WIREPLUMBER_LOG_ERR="$RUNTIME_DIR/wireplumber.err"

cleanup() {
  set +e
  [[ -n "${WIREPLUMBER_PID:-}" ]] && kill -TERM "$WIREPLUMBER_PID" 2>/dev/null || true
  [[ -n "${PIPEWIRE_PID:-}" ]] && kill -TERM "$PIPEWIRE_PID" 2>/dev/null || true
  [[ -n "${DBUS_PID:-}" ]] && kill -TERM "$DBUS_PID" 2>/dev/null || true
  rm -rf "$RUNTIME_DIR" 2>/dev/null || true
}
trap cleanup EXIT

# Private DBus session bus for WirePlumber.
DBUS_OUT="$(dbus-daemon --session --fork --nopidfile --print-address=1 --print-pid=1)"
export DBUS_SESSION_BUS_ADDRESS="$(echo "$DBUS_OUT" | head -n 1)"
DBUS_PID="$(echo "$DBUS_OUT" | tail -n 1)"

nohup pipewire -c /usr/share/pipewire/pipewire.conf >"$PIPEWIRE_LOG_OUT" 2>"$PIPEWIRE_LOG_ERR" &
PIPEWIRE_PID=$!

for _ in $(seq 1 200); do
  [[ -S "$RUNTIME_DIR/pipewire-0" ]] && [[ -S "$RUNTIME_DIR/pipewire-0-manager" ]] && break
  sleep 0.05
done

# Ensure the Profiler interface exists so `headroomctl diagnostics` works in a hermetic graph.
PROFILER_OK=0
for _ in $(seq 1 60); do
  pw-cli -r pipewire-0-manager load-module libpipewire-module-profiler >/dev/null 2>&1 || true
  if pw-cli -r pipewire-0-manager ls Profiler 2>/dev/null | grep -q 'PipeWire:Interface:Profiler'; then
    PROFILER_OK=1
    break
  fi
  sleep 0.05
done
if [[ "$PROFILER_OK" -ne 1 ]]; then
  echo "Failed to load PipeWire profiler module (libpipewire-module-profiler)" >&2
  exit 1
fi

nohup env PIPEWIRE_REMOTE=pipewire-0-manager wireplumber >"$WIREPLUMBER_LOG_OUT" 2>"$WIREPLUMBER_LOG_ERR" &
WIREPLUMBER_PID=$!

# Seed a minimal graph so CLI listing/tests have deterministic non-empty output.
pw-cli -r pipewire-0-manager create-node adapter \
  factory.name=support.null-audio-sink node.name=Headroom-TestSink node.description=TestSink \
  media.class=Audio/Sink object.linger=true audio.channels=2 'audio.position=[ FL FR ]' >/dev/null
pw-cli -r pipewire-0-manager create-node adapter \
  factory.name=support.null-audio-sink node.name=Headroom-TestSource node.description=TestSource \
  media.class=Audio/Source object.linger=true audio.channels=2 'audio.position=[ FL FR ]' >/dev/null

for _ in $(seq 1 200); do
  if pw-cli -r pipewire-0 ls Port 2>/dev/null | grep -q 'object.path = "Headroom-TestSink:playback_0"'; then
    break
  fi
  sleep 0.05
done

pw-cli -r pipewire-0-manager create-node adapter \
  factory.name=support.null-audio-sink node.name=Headroom-AltSink node.description=AltSink \
  media.class=Audio/Sink object.linger=true audio.channels=2 'audio.position=[ FL FR ]' >/dev/null

for _ in $(seq 1 200); do
  if pw-cli -r pipewire-0 ls Port 2>/dev/null | grep -q 'object.path = "Headroom-AltSink:playback_0"'; then
    break
  fi
  sleep 0.05
done

status=0
"$@" || status=$?
if [[ $status -ne 0 ]]; then
  echo "Command failed (exit $status): $*" >&2
  echo "---- pipewire.err (tail) ----" >&2
  tail -n 200 "$PIPEWIRE_LOG_ERR" >&2 || true
  echo "---- wireplumber.err (tail) ----" >&2
  tail -n 200 "$WIREPLUMBER_LOG_ERR" >&2 || true
fi

exit "$status"
