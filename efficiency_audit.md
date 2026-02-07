# Efficiency / Battery Audit

This document focuses on reducing CPU usage, wakeups, and background PipeWire work so Headroom can run comfortably on laptops and in the tray.

## What “battery friendly” means here

- **When the GUI is hidden/minimized/tray-only:** no continuous UI redraw; no meter/visualizer audio taps running; no profiler sampling; minimal PipeWire traffic.
- **When the user is actively interacting:** accept higher refresh rates, but avoid unnecessary per-frame allocations and repeated graph queries.
- **When the TUI is idle:** avoid full-screen redraws and expensive graph computations unless something actually changed.

## Major wakeup/CPU sources (current codebase)

### 1) Mixer per-row meters (`AudioLevelTap`)

Each row can create its own PipeWire monitor stream (`headroom.meter.*`) and process audio every quantum to compute peak/RMS. This can dominate CPU and PipeWire activity when many rows exist.

**Mitigation implemented:**
- Meter taps are now **disabled when the meter widget isn’t visible** (and re-enabled on show).
- Mixer rebuild/control refresh work is **deferred while the Mixer tab is hidden**.
- Mixer has a **“Meters: Off / Selected only / Visible rows / All”** setting to avoid enabling lots of taps by default.

**Investigation (shared meter tap):**
- Today “N meters” means **N `pw_stream`s** (and thus N internal `headroom.meter.*` nodes), but they all share **one PipeWire core/client** via `PipeWireThread`.
- A true shared tap would likely require a **multi-port `pw_filter` meter node** and explicit link management (including channel mapping), because `PW_KEY_TARGET_OBJECT` is per-stream (not per-port).
- Time-multiplexing a single stream by frequently retargeting would require disconnect/reconnect cycles and is likely too expensive/glitch-prone.
- Recommendation: keep current approach + “Meters” mode gating; revisit if profiling shows real overhead (candidate: dedicated multi-input meter filter node).

### 2) GUI timers / hidden-tab work

Qt timers that run while a page is hidden keep waking the CPU and often trigger repaints.

**Mitigation implemented:**
- Mixer meter timer runs only while visible.
- Patchbay rebuild work is deferred while Patchbay is hidden.
- Visualizer already enables/disables its audio tap via show/hide.
- Non-critical UI refresh timers prefer **`Qt::CoarseTimer`**.
- Dialogs with periodic refresh stop timers while hidden (e.g. recording status, engine diagnostics).
- Tray UI refresh is suppressed while running tray-only (refreshes on menu open / while window is visible), to avoid background wakeups from graph churn.

**Further options (optional):**
- If profiling still shows wakeups, re-audit any remaining dialogs/pages for hidden timers (and stop them).

### 3) TUI render loop

The TUI historically redrew every loop iteration and repeatedly recomputed device/stream lists and stream routing.

**Mitigation implemented:**
- Cache device/stream lists per frame in `TuiState`.
- Avoid per-row expensive routing rebuilds (Streams page).
- Render only when **dirty** or on a modest periodic refresh (Recording/Status), instead of every loop.
- Coalesce rapid Left/Right (volume) repeats so PipeWire volume writes don’t block interaction.

### 4) Patchbay/Graphics artifacts

QGraphicsView update modes can cause extra repaints and visual artifacts when many items move/refresh.

**Mitigation implemented:**
- Use `QGraphicsView::SmartViewportUpdate` for Patchbay view.

### 5) Profiler sampling (`module-profiler`)

Binding the PipeWire Profiler interface can produce frequent callbacks (especially while audio is active). Parsing snapshots for CPU/xruns/latency is useful for diagnostics, but it shouldn’t keep the app “hot” in the background.

**Mitigation implemented:**
- When the GUI window is hidden/minimized, Headroom disables its profiler subscription and re-enables it when the window is shown again.

## How to measure (recommended workflow)

1. **Baseline CPU/wakeups while idle**
   - Start Headroom, leave it in the tray for 2–5 minutes.
   - Confirm CPU usage is near zero and wakeups are low (tools: `powertop`, `htop`, `perf top`).
   - No-`sudo` quick check: `scripts/host_efficiency_audit.sh --name headroom --duration 300` (ctx/s is a wakeups proxy).
2. **PipeWire overhead**
   - Check whether Headroom is creating meter/visualizer streams when it shouldn’t (tool: `pw-top`).
3. **Interactive performance**
   - Mixer: drag sliders, toggle mute, switch default device; ensure UI remains responsive.
   - TUI: hold Left/Right (or +/-) and verify no sluggishness or redraw glitches.

## Acceptance checklist

- [x] GUI hidden: no `headroom.meter.*` streams present (covered by `ctest` integration test `headroom_gui_efficiency_meters_hide`).
- [x] GUI hidden: CPU stays low and stable (covered by `ctest` integration test `headroom_gui_efficiency_meters_hide`, which enforces a max CPU % while hidden).
- [x] GUI hidden: no `headroom.visualizer` stream present (covered by `ctest` integration test `headroom_gui_efficiency_visualizer_hide`).
- [x] TUI idle: no high-frequency redraw; input remains responsive (covered by `ctest` integration test `headroom_tui_efficiency_idle` + interaction coverage in `headroom_tui_smoke`).
- [x] Mixer visible: meters work; UI remains responsive; no excessive flicker (covered by `ctest` integration test `headroom_gui_efficiency_meters_visible`).
