#!/usr/bin/env bash
set -euo pipefail

PID=""
NAME=""
DURATION_S=300
INTERVAL_S=1

usage() {
  cat >&2 <<'EOF'
Usage:
  scripts/host_efficiency_audit.sh --pid PID [--duration SEC] [--interval SEC]
  scripts/host_efficiency_audit.sh --name PROCESS_NAME [--duration SEC] [--interval SEC]

Samples a process' CPU usage and context-switch rate (a proxy for wakeups) using /proc.

Recommended workflow (tray-only idle audit):
  1) Start Headroom and hide/minimize it (tray-only).
  2) Run: scripts/host_efficiency_audit.sh --name headroom --duration 300

Output:
  - Per-interval CPU% (single-core %) and ctx/s.
  - Summary with avg/max CPU% and total/avg/max ctx/s.

Notes:
  - Does not require sudo.
  - CPU% may exceed 100% on multi-core systems.
EOF
}

err() { printf "error: %s\n" "$*" >&2; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --pid)
      PID="${2:-}"
      shift 2
      ;;
    --name)
      NAME="${2:-}"
      shift 2
      ;;
    --duration)
      DURATION_S="${2:-}"
      shift 2
      ;;
    --interval)
      INTERVAL_S="${2:-}"
      shift 2
      ;;
    *)
      err "unknown arg: $1"
      usage
      exit 2
      ;;
  esac
done

if [[ -z "$PID" && -z "$NAME" ]]; then
  err "missing required: --pid or --name"
  usage
  exit 2
fi

if [[ -n "$PID" && -n "$NAME" ]]; then
  err "--pid and --name are mutually exclusive"
  usage
  exit 2
fi

if [[ -n "$NAME" ]]; then
  if command -v pgrep >/dev/null 2>&1; then
    PID="$(pgrep -n -x "$NAME" 2>/dev/null || true)"
  elif command -v pidof >/dev/null 2>&1; then
    PID="$(pidof -s "$NAME" 2>/dev/null || true)"
  else
    err "need pgrep or pidof to resolve --name"
    exit 2
  fi
fi

if [[ -z "$PID" ]]; then
  err "could not resolve PID"
  exit 2
fi

if ! [[ "$PID" =~ ^[0-9]+$ ]]; then
  err "invalid PID: $PID"
  exit 2
fi

if [[ ! -r "/proc/$PID/stat" ]]; then
  err "process not found (or not readable): pid=$PID"
  exit 2
fi

CLK_TCK="$(getconf CLK_TCK 2>/dev/null || echo 100)"
if ! [[ "$CLK_TCK" =~ ^[0-9]+$ ]] || [[ "$CLK_TCK" -le 0 ]]; then
  CLK_TCK=100
fi

read_cpu_jiffies() {
  local stat rest
  stat="$(<"/proc/$PID/stat")"
  rest="${stat##*) }"
  set -- $rest
  local utime="${12:-0}"
  local stime="${13:-0}"
  if [[ -z "$utime" || -z "$stime" ]]; then
    printf "0\n"
    return 0
  fi
  printf "%s\n" "$((utime + stime))"
}

read_ctx_switches() {
  local vol non
  vol="$(awk '/^voluntary_ctxt_switches:/ {print $2; exit}' "/proc/$PID/status" 2>/dev/null || echo 0)"
  non="$(awk '/^nonvoluntary_ctxt_switches:/ {print $2; exit}' "/proc/$PID/status" 2>/dev/null || echo 0)"
  printf "%s %s\n" "${vol:-0}" "${non:-0}"
}

if ! [[ "$DURATION_S" =~ ^[0-9]+$ ]] || [[ "$DURATION_S" -le 0 ]]; then
  err "invalid --duration: $DURATION_S"
  exit 2
fi
if ! [[ "$INTERVAL_S" =~ ^[0-9]+$ ]] || [[ "$INTERVAL_S" -le 0 ]]; then
  err "invalid --interval: $INTERVAL_S"
  exit 2
fi

printf "pid=%s duration=%ss interval=%ss clk_tck=%s\n" "$PID" "$DURATION_S" "$INTERVAL_S" "$CLK_TCK"

start_epoch="$(date +%s 2>/dev/null || echo 0)"
start_jiffies="$(read_cpu_jiffies)"
read -r start_vol start_non <<<"$(read_ctx_switches)"

prev_epoch="$start_epoch"
prev_jiffies="$start_jiffies"
prev_ctx="$((start_vol + start_non))"

max_cpu_pct="0.00"
max_ctx_per_s=0
total_delta_jiffies=0
total_delta_ctx=0

printf "%8s %8s %8s\n" "t(s)" "cpu(%)" "ctx/s"

end_epoch="$((start_epoch + DURATION_S))"
while :; do
  now_epoch="$(date +%s 2>/dev/null || echo 0)"
  if [[ "$now_epoch" -ge "$end_epoch" ]]; then
    break
  fi

  sleep "$INTERVAL_S"

  if [[ ! -r "/proc/$PID/stat" ]]; then
    err "process exited during audit: pid=$PID"
    break
  fi

  now_epoch="$(date +%s 2>/dev/null || echo 0)"
  now_jiffies="$(read_cpu_jiffies)"
  read -r now_vol now_non <<<"$(read_ctx_switches)"

  local_wall="$((now_epoch - prev_epoch))"
  if [[ "$local_wall" -le 0 ]]; then
    local_wall="$INTERVAL_S"
  fi

  delta_j="$((now_jiffies - prev_jiffies))"
  if [[ "$delta_j" -lt 0 ]]; then
    delta_j=0
  fi

  now_ctx="$((now_vol + now_non))"
  delta_ctx="$((now_ctx - prev_ctx))"
  if [[ "$delta_ctx" -lt 0 ]]; then
    delta_ctx=0
  fi

  total_delta_jiffies="$((total_delta_jiffies + delta_j))"
  total_delta_ctx="$((total_delta_ctx + delta_ctx))"

  cpu_pct="$(awk -v dj="$delta_j" -v wall="$local_wall" -v tck="$CLK_TCK" 'BEGIN { printf "%.2f", (100.0 * dj) / (wall * tck) }')"
  ctx_per_s="$((delta_ctx / local_wall))"

  if awk -v a="$cpu_pct" -v b="$max_cpu_pct" 'BEGIN { exit (a > b) ? 0 : 1 }'; then
    max_cpu_pct="$cpu_pct"
  fi
  if [[ "$ctx_per_s" -gt "$max_ctx_per_s" ]]; then
    max_ctx_per_s="$ctx_per_s"
  fi

  elapsed="$((now_epoch - start_epoch))"
  printf "%8s %8s %8s\n" "$elapsed" "$cpu_pct" "$ctx_per_s"

  prev_epoch="$now_epoch"
  prev_jiffies="$now_jiffies"
  prev_ctx="$now_ctx"
done

final_epoch="$(date +%s 2>/dev/null || echo 0)"
wall_s="$((final_epoch - start_epoch))"
if [[ "$wall_s" -le 0 ]]; then
  wall_s="$DURATION_S"
fi

avg_cpu_pct="$(awk -v dj="$total_delta_jiffies" -v wall="$wall_s" -v tck="$CLK_TCK" 'BEGIN { printf "%.2f", (100.0 * dj) / (wall * tck) }')"
avg_ctx_per_s="$((total_delta_ctx / wall_s))"

printf "\nsummary:\n"
printf "  wall_s=%s\n" "$wall_s"
printf "  cpu_avg_pct=%s cpu_max_pct=%s\n" "$avg_cpu_pct" "$max_cpu_pct"
printf "  ctx_total=%s ctx_avg_per_s=%s ctx_max_per_s=%s\n" "$total_delta_ctx" "$avg_ctx_per_s" "$max_ctx_per_s"

