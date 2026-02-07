# Power / Efficiency Measurement Results (Real X11 host)

Host: `pepper@127.0.0.1:4444` (hostname: `blonderon`)

## 2026-02-03 — Tray-only idle (real Xorg :0 on vt7)

Output directory (host): `/tmp/headroom-power-20260203-184457-trayonly`

- Main window map state: `IsUnMapped` (tray-only)
- PipeWire nodes: no `headroom.meter.*` or `headroom.visualizer` nodes observed
- `scripts/host_efficiency_audit.sh` (PID 594503, 300s): `cpu_avg_pct=0.00`, `cpu_max_pct=0.00`, `ctx_total=4`
- `powertop` CSV/HTML captured in the output directory above

Notes:
- On this host, the `powertop` CSV “Overview of Software Power Consumers” section was blank in the generated reports, so the per-process CPU/ctx audit is currently the most actionable “is it draining” signal.

## 2026-02-04 — Audio stimulus + GUI state matrix (real Xorg :0 on vt7)

Output directory (host): `/tmp/headroom-power-20260204-091716-matrix`

Setup:
- Audio stimulus: `ffmpeg` loop of `testdata/audio/demo.opus` piped to `pw-cat` (node.name `Headroom-AudioStim`, volume 0.02).
- Headroom binary: `/tmp/headroom-new` (built from this repo in the dev container).
- Isolated config forced meters: `[mixer] metersMode=3`.

Results (`scripts/host_efficiency_audit.sh`):
- Visible (60s): `cpu_avg_pct=1.40`, `cpu_max_pct=2.00`, `ctx_total=2867` (`ctx_avg_per_s=47`).
- Minimized (60s): `cpu_avg_pct=0.00`, `cpu_max_pct=0.00`, `ctx_total=6`.
- Tray-only (300s): `cpu_avg_pct=0.00`, `cpu_max_pct=1.00`, `ctx_total=11`.

PipeWire nodes:
- No `headroom.meter.*` nodes were observed during this run (even with `metersMode=3`). The minimized/tray-only numbers are still the key “not in use” battery signal for background drain.

## 2026-02-05 — Rerun (AudioStim active): tray-only + minimized (real Xorg :0, active screen)

Setup:
- Audio stimulus: `pw-cat` playback from `/dev/zero` (node.name `Headroom-AudioStim`, volume 0.02).
- Headroom config: forced meters on (`[mixer] metersMode=3`).

Results (host efficiency audit, 300s):
- Tray-only (close-to-tray): `meters before hide=8`, `after hide=0`; `cpu_avg_pct=0.00`, `cpu_max_pct=0.00`, `ctx_total=7`, `ctx_max_per_s=1`.
- Minimized (not closed): `meters before minimize=9`, `after minimize=0`; `cpu_avg_pct=0.00`, `cpu_max_pct=0.00`, `ctx_total=2`, `ctx_max_per_s=1`.

PipeWire nodes:
- While tray-only/minimized: no `headroom.meter.*` or `headroom.visualizer` nodes observed (confirmed via `pw-cli ls Node` filtering).

## 2026-02-05 — Full visible→minimized→tray-only matrix via script (real Xorg :0, active screen)

Output directory (host): `/tmp/headroom-gui-eff-matrix-20260205-052317`

Setup:
- Script: `scripts/host_gui_efficiency_matrix.sh` (copied to host under `/tmp/maxheadroom-hosttest/scripts`).
- Audio stimulus: `pw-cat` playback from `/dev/zero` (node.name `Headroom-AudioStim`, volume 0.02).
- Headroom config: forced meters on (`[mixer] metersMode=3`).

Results (`scripts/host_efficiency_audit.sh`):
- Visible (60s): `cpu_avg_pct=12.17`, `cpu_max_pct=15.00`, `ctx_total=5614` (`ctx_avg_per_s=93`).
- Minimized (300s): `cpu_avg_pct=0.00`, `cpu_max_pct=1.00`, `ctx_total=6` (`ctx_max_per_s=1`).
- Tray-only (300s): `cpu_avg_pct=0.00`, `cpu_max_pct=0.00`, `ctx_total=7` (`ctx_max_per_s=1`).

PipeWire nodes:
- Visible: `headroom.meter.*` nodes observed (`meters=9`).
- Minimized/tray-only: no `headroom.meter.*` or `headroom.visualizer` nodes observed (`meters=0`, `visualizer=0`).

## 2026-02-05 — Full matrix rerun (real X :0, active screen)

Output directory (host): `/tmp/headroom-gui-eff-matrix-20260205-130611`

Results (`scripts/host_efficiency_audit.sh`):
- Visible (60s): `cpu_avg_pct=10.45`, `cpu_max_pct=12.00`, `ctx_total=5722` (`ctx_avg_per_s=95`).
- Minimized (300s): `cpu_avg_pct=0.00`, `cpu_max_pct=0.00`, `ctx_total=6` (`ctx_max_per_s=1`).
- Tray-only (300s): `cpu_avg_pct=0.00`, `cpu_max_pct=0.00`, `ctx_total=7` (`ctx_max_per_s=1`).

PipeWire nodes:
- Visible: `headroom.meter.*` nodes observed (`meters=9`).
- Minimized/tray-only: no `headroom.meter.*` or `headroom.visualizer` nodes observed (`meters=0`, `visualizer=0`).

## 2026-02-06 — Full matrix after “Simple (Peak)” meter style + adaptive tick (real X :0, active screen)

Output directory (host): `/tmp/headroom-gui-eff-matrix-20260206-025805`

Setup:
- Meter style: defaulted to “Simple (Peak)” (no config key set; new default).
- Headroom config: forced meters on (`[mixer] metersMode=3`).

Results (`scripts/host_efficiency_audit.sh`):
- Visible (60s): `cpu_avg_pct=6.77`, `cpu_max_pct=10.00`, `ctx_total=3664` (`ctx_avg_per_s=61`).
- Minimized (120s): `cpu_avg_pct=0.00`, `cpu_max_pct=0.00`, `ctx_total=7` (`ctx_max_per_s=1`).
- Tray-only (120s): `cpu_avg_pct=0.00`, `cpu_max_pct=0.00`, `ctx_total=7` (`ctx_max_per_s=1`).

PipeWire nodes:
- Visible: `headroom.meter.*` nodes observed (`meters=9`).
- Minimized/tray-only: no `headroom.meter.*` or `headroom.visualizer` nodes observed (`meters=0`, `visualizer=0`).
