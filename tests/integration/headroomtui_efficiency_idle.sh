#!/usr/bin/env bash
set -euo pipefail

TUI_BIN="${1:-}"
if [[ -z "$TUI_BIN" ]]; then
  echo "Usage: headroomtui_efficiency_idle.sh /path/to/headroom-tui" >&2
  exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HARNESS="$ROOT/tests/harness/run_private_pipewire.sh"

"$HARNESS" -- bash -lc '
  set -euo pipefail
  TUI_BIN="$1"
  export TERM="${TERM:-xterm-256color}"
  export HEADROOM_TUI="$TUI_BIN"

  expect <<'"'"'EOF'"'"'
    log_user 0
    set timeout 25
    set tui $env(HEADROOM_TUI)
    spawn -noecho $tui --self-test-idle
    expect eof
    catch wait result
    set exit_status [lindex $result 3]
    exit $exit_status
EOF
' bash "$TUI_BIN"

