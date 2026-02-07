# Headroom Plan

## Current MVP

- Qt6 app with tabs: Mixer, Visualizer, Patchbay, Graph.
- Global Settings dialog (toolbar) with Output Devices ordering (affects Mixer + Patchbay).
- Parametric EQ (Output devices + Input devices + per-app streams), implemented as an in-graph PipeWire filter node.
- PipeWire graph discovery (nodes/ports/links), node Props (volume/mute), link create/destroy.
- Visualizers: waveform, spectrum, spectrogram.

## Roadmap (near-term)

- Patchbay: drag-to-connect, per-port filtering, saved layouts.
- Settings: reorder sinks (Output Devices) + future “layout edit mode” to drag sinks/nodes and persist positions.
- EQ: response curve display + presets/import (AutoEQ/Squiglink) + per-app EQ.
- Recording: “record everything” (system mix / per-device / per-app), like Audio Hijack-style capture to WAV/FLAC.
- CLI: headless TUI over SSH (alsamixer-style), with tabbed “spaces” (devices/streams/patchbay/EQ/recording) + hotkeys.
- Mixer: default device selection, per-app routing, nicer meters, presets.
- MIDI: show MIDI nodes/ports in Patchbay, connect/disconnect.
- Config: user preferences (theme, FFT sizes, tap target, refresh rates).
- Packaging: finish Flatpak manifest + add distro packaging metadata.

## QjackCtl parity gaps (to cover)

- Engine control: PipeWire service/profile/quantum controls (start/stop/restart, sample rate/quantum/latency presets).
- Status/diagnostics: xruns, DSP/CPU load, precise latency/buffer readouts, log/console.
- Sessions: save/restore graph + app orchestration (JACK session / NSM-style workflows).
- MIDI: first-class MIDI graph (ALSA seq / MIDI ports) + connect/disconnect + filtering.
- Patchbay power features: persistent patchbay, auto-connect rules, port aliases, presets/profiles, blacklists.
- Power-user UX: tray icon, multiple configuration profiles, startup/shutdown scripts/hooks.

## Terminal / SSH parity (non-negotiable)

- Treat `headroom-tui` + `headroomctl` as first-class: any control/action that exists in the GUI must also be possible over SSH (either via TUI hotkeys or a `headroomctl` command).
- If something is inherently graphical (visualizers / drag layout), provide a useful terminal alternative (meters/diagnostics and machine-readable graph state via `headroomctl`) and document the difference.

## Screenshots

- Generate screenshots (GUI + tray + terminal; uses a temporary PipeWire instance + demo tone streams):
  - `./scripts/make_screenshots.sh`
  - Output: `screenshots/*.png` (includes `tui-*.png` for Outputs/Inputs/Streams/Patchbay/EQ/Recording/Status/Engine + `cli-commands.png`)
  - Build selection: uses `HEADROOM_BUILD_DIR` if set; otherwise prefers `build_test/` (if present) and falls back to building `build/` (Release).
  - Note: `make_screenshots.sh` starts a *private* `dbus-daemon` + `pipewire` + `wireplumber` instance and prefers `snd-aloop` (when available) so the visualizers show real audio. If `snd-aloop` can’t be used, it falls back to a virtual null sink.
  - Audio source: prefers `testdata/audio/demo.opus` (gitignored) and falls back to generated sine tones if missing.
  - Terminal screenshots are captured via `Xvfb + xterm + xdotool + import` (best-effort; skipped if deps are missing).

- Upload screenshots:
  - `wtf-upload screenshots/*.png`
  - Requires AWS credentials (`AWS_ACCESS_KEY_ID`/`AWS_SECRET_ACCESS_KEY` in env). In this dev environment they live in `~/.api-keys`.

- One-link gallery page (recommended):
  - `./scripts/publish_screenshots.sh`
  - Regenerates `screenshots/*.png`, uploads them, generates a temporary `index.html` pointing at the uploaded images, then uploads that HTML (does not modify the repo’s `screenshots/index.html`).
  - Latest published gallery (2026-02-05): https://tmp.uh-oh.wtf/2026/02/05/38c345a3-index.html

## Tray menu demo (more comprehensive)

- The tray menu depends on a real system-tray manager (XEmbed). Headroom’s normal `QT_QPA_PLATFORM=offscreen` screenshot flow can’t exercise it.
- For screenshots in this container, we use `Xvfb + openbox + stalonetray + xdotool`:
  - Single screenshot: `./scripts/make_tray_screenshot.sh screenshots/tray-menu.png`
  - Full tray demo screenshots (automated), so it’s clear the tray UX works end-to-end:
    - `./scripts/make_tray_demo_screenshots.sh screenshots`
    - Tray icon visible in tray (baseline, before opening menu).
    - Menu open (shows current volume %, mute state, default output label).
    - Profiles submenu open (shows checkmark for active profile).
    - Toggle mute (capture *after* toggle so checkbox state differs).
    - Move volume slider (capture at e.g. ~30% and ~80% so it’s obvious it changed).
    - “Open Mixer” from tray (capture Mixer window open/foreground).
    - “Open Patchbay” from tray (capture Patchbay window open/foreground).
    - Optional: “Quit” (verify process exits cleanly; not necessarily a screenshot).
  - Ensure screenshots are actually unique (avoid the previous “every image is the same” issue): wait for UI updates between actions and always write distinct output filenames.

## Validation Notes (dev container)

- This container typically has no system PipeWire running; use a private instance + set `XDG_RUNTIME_DIR`.
- Verified CLI/TUI against a private PipeWire instance:
  - `headroomctl nodes` showed:
    - `32  Audio/Sink          NullSink`
    - `34  Stream/Output/Audio ToneA`
  - `headroomctl sinks` showed:
    - `32  NullSink  100%  unmuted`
  - `headroom-tui` smoke test: launched, tabbed between pages, and quit cleanly (exit OK).
  - `headroom-tui` EQ page: preset picker + enable/disable toggle worked (expect-driven smoke test).
  - `headroom-tui` Recording page: start/stop worked and produced a valid WAV header; in this container’s private PipeWire graph, nodes may remain suspended without a session manager, so recorded data bytes can be 0.
  - Verified recording format/template/timer wiring via `headroomctl record start "/tmp/headroom-test-{datetime}-{target}.{ext}" --format flac --duration 1 ...` (status JSON includes format/duration/frames/peak/rms, quantumFrames, and a graphSnapshot of devices/streams at start; bytes may be 0 depending on graph).
  - Tray icon features require a desktop shell/system tray; in this container/offscreen runs, the tray may be unavailable.
  - Tray demo screenshots: `./scripts/make_tray_demo_screenshots.sh` now enforces that volume changes are observable (waits for `headroomctl sinks --json` to report the new percent) and that all tray screenshots are unique (fixes the earlier “all images are the same” issue). `headroomctl set-volume`/`mute` also waits briefly after successful writes so one-shot commands reliably reach PipeWire before exit.
  - Patchbay port ordering: ports are rendered in a stable, channel-aware order (e.g. `FL` before `FR`) to avoid confusing EQ node layouts like `in_playback_1`/`out_playback_1` appearing swapped between screenshots/runs.

## Task List

### 0.0 (Testing) — now (blocker for release)

- [x] Add CTest wiring + `ctest` docs (local).
- [x] Add a unit-test framework (QtTest) + baseline unit tests.
- [x] Add a hermetic integration harness that launches a private `dbus-daemon` + `pipewire` + `wireplumber`, seeds a minimal graph, and exposes `XDG_RUNTIME_DIR` to tests.
- [x] Add `headroomctl` contract tests (JSON shape + stable fields) for: `nodes`, `sinks`, `sources`, `ports`, `links`.
- [x] Add `headroomctl` contract tests (JSON shape + stable fields) for: `engine status`, `diagnostics`.
- [x] Add `headroomctl` integration tests for: `set-volume`/`mute`, `default-sink`/`default-source`, sink ordering (`sinks order ...`), patchbay connect/disconnect, profiles save/apply/delete, autoconnect enable/apply/rules, sessions save/apply/delete, EQ enable/preset, recording start/stop/status.
- [x] Add TUI smoke/E2E tests under `Xvfb` (navigation + hotkeys: defaults, reorder, connect/disconnect, EQ toggle) with deterministic artifacts (covered by `ctest` integration test `headroom_tui_smoke`).
- [x] Add GUI + tray smoke tests (offscreen launch, open key dialogs, tray demo run) — screenshot pipeline stays a required E2E pass, but add explicit “no-crash” assertions (covered by `ctest` integration tests `headroom_gui_smoke_offscreen` + `headroom_tray_demo_smoke`).
- [x] CI: add a workflow that runs unit tests + private-PipeWire integration tests on every PR/push (separate from the Flatpak build job).
- [x] Real PipeWire host validation: add a `scripts/host_test.sh` checklist runner (systemd user units, real devices, latency presets, MIDI bridge, recording with real audio) for the machine you’ll provide.

### 0.1 (CLI / SSH) — top priority

- [x] `headroom-tui`: ncurses TUI shell with tabbed spaces + basic Outputs/Inputs volume/mute controls.
- [x] `headroom-tui`: reorder Outputs list (persist sink order to settings; `[ / ]`).
- [x] `headroom-tui`: Streams page (per-app playback/recording) + move streams between devices.
- [x] `headroom-tui`: Patchbay page that can connect/disconnect ports.
- [x] `headroom-tui`: EQ page (enable/disable per node + preset picker).
- [x] `headroom-tui`: Recording page (start/stop “record everything” once recorder exists).
- [x] `headroomctl`: list nodes/sinks/sources + set volume/mute (works with private PipeWire via `XDG_RUNTIME_DIR`).
- [x] `headroomctl`: connect/disconnect ports + show links + optional JSON output.
- [x] `headroomctl`: EQ toggles/presets + recording start/stop/status.

### 0.2 (daily-usable)

- [x] Mixer: per-app streams list (volume/mute/move-to-device).
- [x] Mixer: default device selection + “set as default”.
- [x] Mixer: proper meters (peak/RMS + clip indicator) and smoother updates.
- [x] Settings: “layout edit mode” to reorder/position sinks/nodes and persist.
- [x] Patchbay: drag-to-connect UX + disconnect gestures + selection highlight.
- [x] Patchbay: search/filter (by media class, app name, node name, port name).
- [x] Visualizer: per-node/per-port “tap” picker + quick swap from Mixer.
- [x] Visualizer: user-configurable FFT/smoothing/history and refresh rates.
- [x] EQ: response curve display + presets (save/load) + import (AutoEQ/Squiglink).

### 0.3 (power features)

- [x] Patchbay: persistent patchbay (save/restore links) + profiles.
- [x] Patchbay: auto-connect rules (match by node/port regex) + blacklist/whitelist.
- [x] Sessions: save/restore whole setup (links + defaults + EQ + layout) with named snapshots.
- [x] Recording: “record everything” with target picker (system mix / per-device / per-app).
- [x] Recording: format options (WAV/FLAC), levels/monitoring, timer, and file naming templates.
- [x] Recording: per-recording metadata (sample rate/quantum, device/app list).
- [x] Tray icon: quick mute/volume + open Mixer/Patchbay + profile switcher.
- [x] Tray menu: add a comprehensive automated demo (icon visible + menu + Profiles submenu + mute toggle + slider move + open Mixer/Patchbay) and include those screenshots in the gallery upload.

### 0.4 (QjackCtl parity, longer-term)

- [x] Engine control: PipeWire/WirePlumber start/stop/restart (systemd user units).
- [x] Engine control: quantum/sample-rate presets and latency-focused controls (via metadata/config).
- [x] Status/diagnostics: xruns + DSP/CPU load + latency/buffer readouts (PipeWire Profiler).
- [x] Status/diagnostics: log/console view (PipeWire + Headroom logs).
- [x] MIDI: show MIDI nodes/ports in Patchbay; connect/disconnect.
- [x] MIDI: optional ALSA seq bridge (ALSA sequencer <-> PipeWire MIDI).
- [x] Patchbay extras: port aliases, per-port permissions/locking, connection history/undo.
- [x] Startup/shutdown hooks: run scripts when profile loads/unloads.

### 0.5 (packaging / release)

- [x] Flatpak: finish manifest, permissions, portals, and CI build.
- [x] Distro packaging: add metadata/starter packaging (Deb/Arch spec) + AppStream + icons.
- [x] Release checklist: versioning, changelog, and screenshot refresh script integration.

## Next Up (ordered, actionable)

- [x] Tests: wire up CTest + unit test framework.
- [x] Tests: implement private PipeWire integration harness.
- [x] Tests: implement `headroomctl` command-matrix integration suite.
- [x] Tests: add TUI smoke/E2E tests under `Xvfb`.
- [x] Tests: add GUI/tray smoke tests (offscreen + tray demo).
- [x] Tests: add CI workflow to run the above on PRs.
- [x] Tests: run `scripts/host_test.sh` on a real PipeWire machine (systemd user session + real devices) and fix any gaps (recommended: `./scripts/host_test.sh --run-gui-self-tests --efficiency-audit --efficiency-duration 300`; pass `--read-only` if you don’t want it to change audio state; avoid `sudo`).

## Efficiency / Battery (next up)

- Full notes: `power_measurement_plan.md` + `power_measurement_results.md`.
- [x] HIGH: Add a real-X11 host automation script to run the visible→minimized→tray-only efficiency matrix (and assert meters/visualizer nodes stop + CPU/ctx stay ~0 while hidden), so we can regression-test battery behavior outside `QT_QPA_PLATFORM=offscreen`. (2026-02-05: `scripts/host_gui_efficiency_matrix.sh`; pepper@127.0.0.1:4444 full run out=`/tmp/headroom-gui-eff-matrix-20260205-052317`: visible meters=9 cpu_avg_pct=12.17; minimized/tray-only meters=0 cpu_avg_pct=0.00 ctx_max_per_s=1.)
- [x] HIGH: Real-host validation — investigate why `headroom.meter.*` nodes don’t appear on `pepper@127.0.0.1:4444` under real X11 (even with `metersMode=3`), so “visible worst-case” and GUI self-tests are meaningful outside `QT_QPA_PLATFORM=offscreen`. (2026-02-04: fixed X11 map-state detection + ensured the main window is actually `IsViewable`; meter nodes now appear reliably on host when visible.)
- [x] HIGH: Reduce “visible idle” CPU (2026-02-04 host matrix: ~1–2% while visible) by profiling and gating non-essential work (e.g., PipeWire profiler binding/sampling, UI timers/refresh) to the active tab or only when explicitly needed. (2026-02-04: root cause was `headroom.meter.*` node churn from topologyChanged-driven Mixer rebuild loops; fixed by suppressing topologyChanged emission for internal ephemeral nodes/ports/links. Host (AudioStim, window `IsViewable`): metersMode=3 dropped from cpu_avg_pct~29%/ctx~3785 to ~8.6%/ctx~141 (9 meters). Meters Off remains ~2%.)
- [x] HIGH: Update `power_measurement_plan.md` to prefer the new real-host matrix script + “attach to existing :0” workflow (no “sudo start Xorg” assumption), and document robust main-window selection + close-to-tray semantics. (2026-02-05.)
- [x] HIGH: Add a no-sudo “battery power rate” sampler (`scripts/host_power_rate_audit.sh`) to complement CPU/ctx (wakeups proxy) with real Watts from sysfs/upower. (2026-02-05.)
- [x] HIGH: Wire `scripts/host_test.sh` option `--gui-efficiency-matrix` to run the real-X11 visible→minimized→tray-only matrix script when a display is available. (2026-02-05.)
- [x] Efficiency: add “Meters: Off / Selected only / Visible rows / All” setting.
- [x] Efficiency: add “Meter style: Simple (Peak) / Detailed (Peak+RMS)” setting + adaptive meter tick interval (reduces visible worst-case CPU; 2026-02-06 pepper host matrix visible cpu_avg_pct ~10.45 → ~6.77 with meters forced on).
- [x] Efficiency: investigate shared meter tap design (avoid N meter streams for N rows).
- [x] Efficiency: use `Qt::CoarseTimer` for non-critical periodic UI refresh.
- [x] Efficiency: audit dialogs/pages for periodic refresh; stop timers while hidden.
- [x] Efficiency: stop dialog refresh timers while minimized (Engine + Recording).
- [x] Efficiency: suppress tray UI refresh while running tray-only (refresh on menu open / while window is visible).
- [x] Efficiency: disable PipeWire profiler sampling while GUI is hidden/minimized (avoid background wakeups from module-profiler).
- [x] Efficiency: debounce/batch TUI rapid volume key repeats (avoid blocking PipeWire writes).
- [x] Efficiency: run a real-machine wakeups audit with Headroom hidden/tray-only for 5 minutes; confirm CPU ~0% and low wakeups (preferred: `powertop`; no-`sudo` alternative: `./scripts/host_efficiency_audit.sh --name headroom --duration 300`). (2026-02-02: pepper@127.0.0.1:4444, tray-only 300s: cpu_avg_pct=0.00, ctx_avg_per_s=0; no `headroom.meter.*`/`headroom.visualizer` nodes. 2026-02-05 rerun (AudioStim active): meters before hide=8, after hide=0; tray-only 300s: cpu_avg_pct=0.00, cpu_max_pct=0.00, ctx_total=7, ctx_max_per_s=1; no `headroom.meter.*`/`headroom.visualizer` nodes.)
- [x] Efficiency: run a real-machine wakeups audit with Headroom **minimized** (not closed) for 5 minutes; confirm CPU ~0% and low wakeups, and no `headroom.meter.*`/`headroom.visualizer` nodes. (2026-02-02: pepper@127.0.0.1:4444, minimized 300s: cpu_avg_pct=0.00, ctx_avg_per_s=0; `headroom.meter.*` nodes present before minimize (8) and absent while minimized. 2026-02-05 rerun (AudioStim active): meters before minimize=9, after minimize=0; minimized 300s: cpu_avg_pct=0.00, cpu_max_pct=0.00, ctx_total=2, ctx_max_per_s=1; no `headroom.meter.*`/`headroom.visualizer` nodes.)
- [x] Efficiency: add automated `ctest` regression tests for minimized-window low-power behavior (meters/visualizer). (2026-02-03: `headroom_gui_efficiency_meters_minimized`, `headroom_gui_efficiency_visualizer_minimized`.)

### Efficiency acceptance checklist

- [x] GUI hidden: no `headroom.meter.*` streams present (covered by `ctest` integration test `headroom_gui_efficiency_meters_hide`).
- [x] GUI hidden: CPU stays low and stable (covered by `ctest` integration test `headroom_gui_efficiency_meters_hide`, which enforces a max CPU % while hidden).
- [x] GUI hidden: no `headroom.visualizer` stream present (covered by `ctest` integration test `headroom_gui_efficiency_visualizer_hide`).
- [x] GUI minimized: no `headroom.meter.*` streams present (covered by `ctest` integration test `headroom_gui_efficiency_meters_minimized`).
- [x] GUI minimized: no `headroom.visualizer` stream present (covered by `ctest` integration test `headroom_gui_efficiency_visualizer_minimized`).
- [x] GUI hidden/minimized: no profiler sampling (module-profiler) callbacks.
- [x] Tray-only: no periodic tray refresh wakeups (refresh on menu open).
- [x] TUI idle: no high-frequency redraw; input remains responsive (covered by `ctest` integration test `headroom_tui_efficiency_idle` + interaction coverage in `headroom_tui_smoke`).
- [x] Mixer visible: meters work; UI remains responsive; no excessive flicker (covered by `ctest` integration test `headroom_gui_efficiency_meters_visible`).
