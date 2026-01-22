# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

- Packaging: distro specs + release automation polish.

## [0.1.0] - 2026-01-22

### Added

- Qt6 GUI (`headroom`) with Mixer / Visualizer / Patchbay / Graph tabs.
- PipeWire graph control: discover nodes/ports/links, create/destroy links, volume/mute controls.
- Visualizers: waveform + spectrum + spectrogram with selectable tap target.
- Patchbay profiles, auto-connect rules (regex + whitelist/blacklist), and profile load/unload hooks.
- Sessions (snapshots) for saving/restoring routing + settings.
- Parametric EQ per output/input device (in-graph filter), with response curve + presets/import.
- Recording (“record everything”) with WAV/FLAC output, timer, templates, and metadata.
- CLI (`headroomctl`) and ncurses TUI (`headroom-tui`) for SSH/headless workflows.
