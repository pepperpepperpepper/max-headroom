#!/usr/bin/env bash
set -euo pipefail

DURATION_S=300
INTERVAL_S=1
BAT_PATH=""

usage() {
  cat >&2 <<'EOF'
Usage:
  scripts/host_power_rate_audit.sh [--duration SEC] [--interval SEC] [--battery /sys/class/power_supply/BAT*]

Samples battery power rate (Watts) over time (best-effort, no sudo).

Notes:
  - Intended for laptop "is it draining battery?" validation.
  - Positive Watts means battery discharge; negative means charging.
  - Prefer running on battery power (unplugged) for meaningful numbers.
EOF
}

err() { printf "error: %s\n" "$*" >&2; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --duration)
      DURATION_S="${2:-}"
      shift 2
      ;;
    --interval)
      INTERVAL_S="${2:-}"
      shift 2
      ;;
    --battery)
      BAT_PATH="${2:-}"
      shift 2
      ;;
    *)
      err "unknown arg: $1"
      usage
      exit 2
      ;;
  esac
done

if ! [[ "$DURATION_S" =~ ^[0-9]+$ ]] || [[ "$DURATION_S" -le 0 ]]; then
  err "invalid --duration: $DURATION_S"
  exit 2
fi
if ! [[ "$INTERVAL_S" =~ ^[0-9]+$ ]] || [[ "$INTERVAL_S" -le 0 ]]; then
  err "invalid --interval: $INTERVAL_S"
  exit 2
fi

find_battery() {
  local d=""
  for d in /sys/class/power_supply/BAT*; do
    [[ -d "$d" ]] || continue
    if [[ -r "$d/type" ]] && [[ "$(cat "$d/type" 2>/dev/null || true)" == "Battery" ]]; then
      echo "$d"
      return 0
    fi
  done
  return 1
}

if [[ -z "$BAT_PATH" ]]; then
  BAT_PATH="$(find_battery || true)"
fi

if [[ -z "$BAT_PATH" || ! -d "$BAT_PATH" ]]; then
  err "no battery found under /sys/class/power_supply (pass --battery PATH)"
  exit 2
fi

if [[ ! -r "$BAT_PATH/type" ]] || [[ "$(cat "$BAT_PATH/type" 2>/dev/null || true)" != "Battery" ]]; then
  err "not a Battery supply: $BAT_PATH"
  exit 2
fi

METHOD=""
if [[ -r "$BAT_PATH/power_now" ]]; then
  METHOD="power_now_uw"
elif [[ -r "$BAT_PATH/current_now" && -r "$BAT_PATH/voltage_now" ]]; then
  METHOD="current_voltage"
elif [[ -r "$BAT_PATH/energy_now" ]]; then
  METHOD="energy_delta"
else
  err "battery has no readable power source (power_now/current_now+voltage_now/energy_now): $BAT_PATH"
  exit 2
fi

read_status() { cat "$BAT_PATH/status" 2>/dev/null || echo "unknown"; }

read_power_uw() {
  case "$METHOD" in
    power_now_uw)
      cat "$BAT_PATH/power_now" 2>/dev/null || echo 0
      ;;
    current_voltage)
      local cur volt
      cur="$(cat "$BAT_PATH/current_now" 2>/dev/null || echo 0)"
      volt="$(cat "$BAT_PATH/voltage_now" 2>/dev/null || echo 0)"
      if ! [[ "$cur" =~ ^-?[0-9]+$ ]] || ! [[ "$volt" =~ ^-?[0-9]+$ ]]; then
        echo 0
        return 0
      fi
      # µA * µV = pW; convert to µW: divide by 1e6.
      echo "$((cur * volt / 1000000))"
      ;;
    *)
      echo 0
      ;;
  esac
}

uw_to_w() {
  awk -v uw="$1" 'BEGIN { printf "%.3f", uw / 1000000.0 }'
}

float_lt() { awk -v a="$1" -v b="$2" 'BEGIN { exit (a < b) ? 0 : 1 }'; }
float_gt() { awk -v a="$1" -v b="$2" 'BEGIN { exit (a > b) ? 0 : 1 }'; }
float_add() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.6f", a + b }'; }

printf "battery=%s method=%s duration=%ss interval=%ss\n" "$BAT_PATH" "$METHOD" "$DURATION_S" "$INTERVAL_S"
printf "note: watts > 0 means discharging; watts < 0 means charging\n\n"

printf "%8s %12s %12s\n" "t(s)" "status" "watts"

start_epoch="$(date +%s 2>/dev/null || echo 0)"
end_epoch="$((start_epoch + DURATION_S))"

count=0
sum_w="0.000000"
min_w=""
max_w=""

prev_epoch="$start_epoch"
prev_energy=""
if [[ "$METHOD" == "energy_delta" ]]; then
  prev_energy="$(cat "$BAT_PATH/energy_now" 2>/dev/null || echo '')"
fi

while :; do
  now_epoch="$(date +%s 2>/dev/null || echo 0)"
  if [[ "$now_epoch" -ge "$end_epoch" ]]; then
    break
  fi

sleep "$INTERVAL_S"

now_epoch="$(date +%s 2>/dev/null || echo 0)"
elapsed="$((now_epoch - start_epoch))"
status="$(read_status)"

watts=""
if [[ "$METHOD" == "energy_delta" ]]; then
  now_energy="$(cat "$BAT_PATH/energy_now" 2>/dev/null || echo '')"
  if [[ "$prev_energy" =~ ^-?[0-9]+$ ]] && [[ "$now_energy" =~ ^-?[0-9]+$ ]]; then
    wall_s="$((now_epoch - prev_epoch))"
    if [[ "$wall_s" -le 0 ]]; then
      wall_s="$INTERVAL_S"
    fi
    delta_e="$((now_energy - prev_energy))" # µWh
    # Convert to W: positive means discharge => energy decreasing => delta_e negative.
    watts="$(awk -v de="$delta_e" -v dt="$wall_s" 'BEGIN { printf "%.3f", (-de * 0.0036) / dt }')"
  else
    watts="0.000"
  fi
  prev_energy="$now_energy"
else
  uw="$(read_power_uw)"
  if ! [[ "$uw" =~ ^-?[0-9]+$ ]]; then
    uw=0
  fi
  watts="$(uw_to_w "$uw")"
fi

printf "%8s %12s %12s\n" "$elapsed" "$status" "$watts"

count="$((count + 1))"
sum_w="$(float_add "$sum_w" "$watts")"
if [[ -z "$min_w" ]] || float_lt "$watts" "$min_w"; then
  min_w="$watts"
fi
if [[ -z "$max_w" ]] || float_gt "$watts" "$max_w"; then
  max_w="$watts"
fi

prev_epoch="$now_epoch"
done

final_epoch="$(date +%s 2>/dev/null || echo 0)"
wall_s="$((final_epoch - start_epoch))"
if [[ "$wall_s" -le 0 ]]; then
  wall_s="$DURATION_S"
fi

avg_w="0.000"
if [[ "$count" -gt 0 ]]; then
  avg_w="$(awk -v s="$sum_w" -v n="$count" 'BEGIN { printf "%.3f", s / n }')"
fi

printf "\nsummary:\n"
printf "  wall_s=%s samples=%s\n" "$wall_s" "$count"
printf "  watts_avg=%s watts_min=%s watts_max=%s\n" "$avg_w" "${min_w:-0.000}" "${max_w:-0.000}"
