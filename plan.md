# Headroom Plan

## Current MVP

- Qt6 app with tabs: Mixer, Visualizer, Patchbay, Graph.
- Global Settings dialog (toolbar) with Output Devices ordering (affects Mixer + Patchbay).
- Per-device Parametric EQ (Output + Input devices), implemented as an in-graph PipeWire filter node.
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

## Screenshots

- Generate screenshots (uses a temporary PipeWire instance + demo tone streams):
  - `./scripts/make_screenshots.sh`
  - Output: `screenshots/*.png`
  - Note: `make_screenshots.sh` starts a *private* `dbus-daemon` + `pipewire` + `wireplumber` instance and prefers `snd-aloop` (when available) so the visualizers show real audio. If `snd-aloop` can’t be used, it falls back to a virtual null sink.
  - Audio source: prefers `testdata/audio/demo.opus` (gitignored) and falls back to generated sine tones if missing.

- Upload screenshots:
  - `wtf-upload screenshots/*.png`
  - Requires AWS credentials (`AWS_ACCESS_KEY_ID`/`AWS_SECRET_ACCESS_KEY` in env). In this dev environment they live in `~/.api-keys`.

- One-link gallery page (recommended):
  - `./scripts/publish_screenshots.sh`
  - Regenerates `screenshots/*.png`, uploads them, generates a temporary `index.html` pointing at the uploaded images, then uploads that HTML (does not modify the repo’s `screenshots/index.html`).

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

- [x] TUI: add a key legend/help overlay (alsamixer-style) + status line (selected node, volume, mute).
- [x] TUI: add “default device” actions (set default sink/source) to match GUI behavior.
- [x] CLI: add explicit `default-sink` / `default-source` commands (wrap `PipeWireGraph::setDefaultAudioSink/Source`).
- [x] Patchbay: make `headroom.eq.*` ports readable (avoid `in_playback_1`/`out_playback_1` label overflow).
- [x] Screenshots: publish an uploaded gallery via `./scripts/publish_screenshots.sh` (latest: `https://tmp.uh-oh.wtf/2026/01/24/ca7c1a39-index.html`).
- [x] Release: commit + push to GitHub (`git@github.com:pepperpepperpepper/max-headroom.git`).
- [x] TUI: add a lightweight Engine page (systemd user-unit status + start/stop/restart).
- [x] CLI: add sink ordering commands (for remote layout control).
