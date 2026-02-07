# Power / Efficiency Measurement Plan (Real X11 + Real PipeWire host)

_(Renamed from `power_meaurement_plan.md`.)_

Target host: `pepper@127.0.0.1:4444` (Arch laptop with real PipeWire + an active screen)

Goal: run **legitimate laptop power measurements** (CPU + wakeups proxy + PipeWire nodes + battery watts) while exercising **real X11 windowing** (minimize / close-to-tray / tray menu) instead of `QT_QPA_PLATFORM=offscreen`.

## Recommended scripts (from this repo)

- `scripts/host_gui_efficiency_matrix.sh`: automated visible → minimized → tray-only matrix (forces meters on, asserts taps stop while hidden, logs CPU/ctx).
- `scripts/host_efficiency_audit.sh`: no-sudo per-process CPU% + ctx/s (wakeups proxy).
- `scripts/host_power_rate_audit.sh`: no-sudo battery power sampler (Watts) via sysfs (best-effort; useful for “is it draining battery?” validation).

## Preflight (on the host)

SSH in:

```bash
ssh -p 4444 pepper@127.0.0.1
```

Confirm PipeWire is present (user session):

```bash
echo "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR"
ls -la "$XDG_RUNTIME_DIR"/pipewire-0
```

Confirm you can talk to the already-running X session:

```bash
export DISPLAY=:0
xdpyinfo >/dev/null
```

If `xdpyinfo` fails, you may need to run as the console user / set `XAUTHORITY` / allow local access (or start a temporary Xorg; see below).

## Copy a build + scripts to the host (one-time)

If the host does not have a checkout of this repo, copy just what you need from your dev machine:

```bash
scp -P 4444 build_test/headroom pepper@127.0.0.1:/tmp/headroom
scp -P 4444 -r scripts pepper@127.0.0.1:/tmp/maxheadroom-hosttest
```

## Fast path: run the full visible → minimized → tray-only matrix (recommended)

From a repo checkout on the host:

```bash
export DISPLAY=:0
./scripts/host_gui_efficiency_matrix.sh --headroom ./build_test/headroom --hidden-duration 300
```

Or if you copied to `/tmp`:

```bash
export DISPLAY=:0
/tmp/maxheadroom-hosttest/scripts/host_gui_efficiency_matrix.sh --headroom /tmp/headroom --hidden-duration 300
```

This produces an output directory like `/tmp/headroom-gui-eff-matrix-YYYYmmdd-HHMMSS` containing:

- `pw-visible.txt`, `pw-minimized.txt`, `pw-trayonly.txt` (PipeWire node assertions)
- `eff-*.txt` (CPU% + ctx/s samples + summary)
- `windows.txt` (for diagnosing “wrong window id” issues under real WMs)

## Optional: measure *actual* battery drain (Watts)

Do this on battery power (unplugged) for meaningful numbers.

Baseline (no Headroom):

```bash
scripts/host_power_rate_audit.sh --duration 300 --interval 1 | tee /tmp/headroom-power-baseline.txt
```

Tray-only (Headroom running, window closed-to-tray):

```bash
scripts/host_power_rate_audit.sh --duration 300 --interval 1 | tee /tmp/headroom-power-trayonly.txt
```

Minimized (Headroom running, window minimized):

```bash
scripts/host_power_rate_audit.sh --duration 300 --interval 1 | tee /tmp/headroom-power-minimized.txt
```

Note: compare runs taken close together; screen brightness, Wi‑Fi, and background activity can dominate short samples.

## Optional: powertop snapshots (requires sudo)

```bash
OUT="/tmp/headroom-power-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
sudo -n powertop --time=60 --html="$OUT/powertop-baseline.html" >/dev/null 2>&1 || true
```

## If you need to start a temporary Xorg (headless host)

Only do this if you *don’t* already have an active `DISPLAY=:0` you can use.

```bash
if [[ -S /tmp/.X11-unix/X0 ]]; then
  echo "Xorg already running on :0"
else
  sudo -n rm -f /tmp/.X11-unix/X0 || true

  # Start Xorg as root on a real VT; -ac avoids xauth hassles over SSH.
  sudo -n openvt -s -f -c 7 -- /usr/lib/Xorg :0 vt7 -nolisten tcp -ac \
    -logfile /tmp/headroom-realx-Xorg.0.log >/dev/null 2>&1 &

  for _ in $(seq 1 200); do
    if [[ -S /tmp/.X11-unix/X0 ]] && DISPLAY=:0 xdpyinfo >/dev/null 2>&1; then
      break
    fi
    sleep 0.1
  done

  DISPLAY=:0 xdpyinfo >/dev/null || { tail -n 200 /tmp/headroom-realx-Xorg.0.log; exit 1; }
fi
```

Start a WM + tray (pepper):

```bash
export DISPLAY=:0
nohup openbox >/tmp/headroom-realx-openbox.log 2>&1 &
nohup stalonetray --geometry 1x1-0+0 --icon-size 22 --background "#202020" --kludges force_icons_size \
  >/tmp/headroom-realx-tray.log 2>&1 &
sleep 0.3
```

Cleanup:

```bash
pkill -TERM openbox stalonetray || true
sudo -n pkill -TERM -f '/usr/lib/Xorg :0' || true
sudo -n rm -f /tmp/.X11-unix/X0 || true
```
