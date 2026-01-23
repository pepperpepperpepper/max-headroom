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

- Upload screenshots:
  - `wtf-upload screenshots/*.png`
  - Requires AWS credentials (`AWS_ACCESS_KEY_ID`/`AWS_SECRET_ACCESS_KEY` in env). In this dev environment they live in `~/.api-keys`.

- One-link gallery page (recommended):
  - `./scripts/publish_screenshots.sh`
  - Regenerates `screenshots/*.png`, uploads them, generates a temporary `index.html` pointing at the uploaded images, then uploads that HTML (does not modify the repo’s `screenshots/index.html`).

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

## Task List

### 0.1 (CLI / SSH) — top priority

- [x] `headroom-tui`: ncurses TUI shell with tabbed spaces + basic Outputs/Inputs volume/mute controls.
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
