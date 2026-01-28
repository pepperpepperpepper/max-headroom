# Refactor Plan — De‑monolithize the 5 biggest files

Goal: split the current top 5 “monolith” source files into ~3 smaller translation units each, keeping behavior identical. This is a mechanical refactor: extract/move/rename only.

## Progress tracker

- [x] 1) `src/cli/main.cpp` → `src/cli/main.cpp` + `CliUtil.cpp` + `CliCommands.cpp`
- [x] 2) `src/tui/main.cpp` → `src/tui/main.cpp` + `TuiApp.cpp` + `TuiRender.cpp`
- [x] 3) `src/ui/PatchbayPage.cpp` → `src/ui/PatchbayPage.cpp` + `PatchbaySceneBuilder.cpp` + `PatchbaySceneController.cpp`
- [x] 4) `src/backend/PipeWireGraph.cpp` → `src/backend/PipeWireGraph.cpp` + `PipeWireGraphRegistry.cpp` + `PipeWireGraphOpsMetaProfiler.cpp` (+ `PipeWireGraphOpsNodeControls.cpp` + `PipeWireGraphOpsLinks.cpp`)
- [x] 5) `src/ui/MixerPage.cpp` → `src/ui/MixerPage.cpp` + `MixerRows.cpp` + `MixerRouting.cpp`
- [x] 6) Post-refactor validation: ensure the full screenshot/demo pipeline still works (`./scripts/make_screenshots.sh`)
- [x] 7) Post-refactor: recompute “biggest monoliths” and queue Phase 2 candidates

## Recommended implementation order

1) Start with `src/ui/MixerPage.cpp` (smallest, GUI-only; good warmup).
2) Then `src/cli/main.cpp` and `src/tui/main.cpp` (largest wins; isolated targets).
3) Then `src/ui/PatchbayPage.cpp` (GUI-only but more stateful).
4) Finish with `src/backend/PipeWireGraph.cpp` (shared by all targets; riskiest).

If we need to derisk the backend earlier (e.g., to make future changes easier), swap steps (4) and (3)/(1).

## Principles

- No feature changes. Keep CLI/TUI/GUI behavior and exit codes identical.
- Keep public APIs stable where possible (especially `PipeWireGraph` and UI page classes).
- Keep PipeWire thread/lock semantics unchanged.
- Each step should be buildable (avoid a “big bang” move).

## “Buildable at every step” template

For each monolith:

1) Add new `.cpp` files with stubs (and minimal forward declarations), update `CMakeLists.txt`, verify build.
2) Move pure helpers (no class-member access) first; build.
3) Move class method definitions next; build.
4) Only then move stateful logic (callbacks, event handlers, UI actions); build.
5) Run a tiny smoke pass (help output / launch and quit / screenshot script).

## Biggest monoliths (as of 2026‑01‑26)

1. `src/cli/main.cpp` — 3796 LOC (`headroomctl`)
2. `src/tui/main.cpp` — 3039 LOC (`headroom-tui`)
3. `src/ui/PatchbayPage.cpp` — 1694 LOC
4. `src/backend/PipeWireGraph.cpp` — 1589 LOC
5. `src/ui/MixerPage.cpp` — 858 LOC

---

## Biggest monoliths (post-refactor, as of 2026‑01‑26)

Largest `.cpp` translation units after completing steps (1)-(6):

1. `src/tui/TuiAppActions.cpp` — 788 LOC
2. `src/backend/SessionSnapshots.cpp` — 619 LOC
3. `src/backend/PipeWireGraphOpsMetaProfiler.cpp` — 592 LOC
4. `src/backend/ParametricEqFilter.cpp` — 579 LOC
5. `src/tui/TuiUtil.cpp` — 578 LOC

### Phase 2 candidates (optional)

- [x] A) `src/cli/CliCommands.cpp` → split by domain (graph/patchbay/sessions/engine/recording/eq) so no single file stays > ~1k LOC.
- [x] B) `src/tui/TuiRender.cpp` → split by page/common UI into `TuiRenderAudio.cpp`, `TuiRenderEq.cpp`, `TuiRenderRecording.cpp`, `TuiRenderSystem.cpp` (plus `TuiPrompts.cpp` + `TuiUtil.cpp`; `runTui()` moved to `TuiApp.cpp`).
- [x] C) `src/backend/PipeWireGraphOps.cpp` → split into `PipeWireGraphOpsNodeControls.cpp`, `PipeWireGraphOpsLinks.cpp`, `PipeWireGraphOpsMetaProfiler.cpp` (node controls, link ops, profiler/metadata).
- [x] D) `src/ui/PatchbaySceneController.cpp` → split into `PatchbaySceneController.cpp`, `PatchbaySceneControllerContextMenu.cpp`, `PatchbaySceneControllerUndo.cpp` (interaction state machine, context menus, undo commands).
- [x] E) `src/ui/EngineDialog.cpp` → split into `EngineDialog.cpp`, `EngineDialogClock.cpp`, `EngineDialogSystemd.cpp` (UI layout + diagnostics/midi, latency preset UI/state, systemd/service ops).

### Phase 3 candidates (optional)

- [x] F) `src/tui/TuiApp.cpp` → `src/tui/TuiApp.cpp` + `TuiAppActions.cpp` + `TuiAppRender.cpp` (main loop/state, key handling/actions, frame render).
- [x] G) `src/cli/CliCommandsPatchbay.cpp` → split by command area (profiles, hooks, autoconnect, port config).
- [x] H) `src/cli/CliCommandsGraph.cpp` → `src/cli/CliCommandsGraph.cpp` + `CliCommandsGraphListing.cpp` + `CliCommandsGraphControls.cpp` + `CliCommandsGraphConnections.cpp` (listing, default sink/source, connect/disconnect, node controls).
- [x] I) `src/cli/CliCommandsEngine.cpp` → `src/cli/CliCommandsEngine.cpp` + `CliCommandsEngineClock.cpp` + `CliCommandsEngineMidiBridge.cpp` + `CliCommandsEngineSystemd.cpp` (clock controls, MIDI bridge status/enable, status + start/stop/restart).
- [x] J) `src/ui/RecorderDialog.cpp` → `src/ui/RecorderDialog.cpp` + `src/ui/RecorderDialogTargets.cpp` + `src/ui/RecorderDialogRecording.cpp` (UI layout, target list, recording actions/metadata).

### Phase 4 candidates (optional)

Current “biggest monoliths” after Phase 4L (from `wc -l`, as of 2026‑01‑27):

1. `src/backend/PipeWireGraphOpsMetaProfiler.cpp` — 592 LOC
2. `src/backend/ParametricEqFilter.cpp` — 579 LOC
3. `src/tui/TuiUtil.cpp` — 578 LOC
4. `src/MainWindow.cpp` — 577 LOC
5. `src/backend/EqManager.cpp` — 571 LOC

- [x] K) `src/tui/TuiAppActions.cpp` → `src/tui/TuiAppActions.cpp` + `src/tui/TuiAppActionsDevices.cpp` + `src/tui/TuiAppActionsPatchbay.cpp` (dispatcher/global keys, device actions, patchbay actions).
- [x] L) `src/backend/SessionSnapshots.cpp` → `src/backend/SessionSnapshots.cpp` + `src/backend/SessionSnapshotsSerialize.cpp` + `src/backend/SessionSnapshotsApply.cpp` (types/core, JSON/INI IO, apply/restore logic).
- [x] M) `src/backend/PipeWireGraphOpsMetaProfiler.cpp` → `src/backend/PipeWireGraphOpsMetaProfiler.cpp` + `src/backend/PipeWireGraphOpsProfilerSnapshot.cpp` + `src/backend/PipeWireGraphOpsMetadata.cpp` (snapshot capture, metadata parsing, format helpers).
- [x] N) `src/backend/ParametricEqFilter.cpp` → `src/backend/ParametricEqFilter.cpp` + `src/backend/ParametricEqFilterDesign.cpp` + `src/backend/ParametricEqFilterResponse.cpp` (design math, response evaluation, glue).
- [x] O) `src/tui/TuiUtil.cpp` → `src/tui/TuiUtil.cpp` + `src/tui/TuiUtilFormat.cpp` + `src/tui/TuiUtilPrompts.cpp` (formatting, shared prompt helpers, misc).
- [x] P) `src/MainWindow.cpp` → `src/MainWindow.cpp` + `src/MainWindowTray.cpp` + `src/MainWindowActions.cpp` (window wiring/layout, tray/menu, actions/shortcuts).
- [x] Q) `src/backend/EqManager.cpp` → `src/backend/EqManager.cpp` + `src/backend/EqManagerPersist.cpp` + `src/backend/EqManagerRealtime.cpp` (UI-facing state, settings IO, PipeWire/EQ updates).

## 1) `headroomctl` monolith — `src/cli/main.cpp` → ~3 files

### Target split

- `src/cli/main.cpp`
  - Entry point + global flag parsing (`--json`, `--help`, `--version`) + dispatch table.
  - Lazy creation of `PipeWireThread`/`PipeWireGraph` only when a command needs PipeWire.
- `src/cli/CliUtil.cpp`
  - Pure helpers and shared utilities (parsing, file IO, JSON helpers, process helpers).
- `src/cli/CliCommands.cpp`
  - Command handlers (grouped by domain: graph listing, patchbay, sessions, EQ, recording, engine/diagnostics).

### What moves where (concrete)

- Move “helpers above `main()`” into `CliUtil.cpp`:
  - `printUsage`, `waitForGraph`, `parseNodeId`, `parseVolumeValue`
  - `runtimeDirPath`, `recordingStatusPath`
  - `writeJsonFileAtomic`, `readJsonObjectFile`
  - `pidAlive`, `processRunningExact`, `stopPid`
  - EQ preset serialization helpers (`eqPresetKeyForNodeName`, builtin preset lookup, normalize preset name, etc.)
  - JSON encoders (`nodeToJson`, `nodeControlsToJson`, `portToJson`, `linkToJson`)
- Extract the large `if (cmd == "...") { ... }` blocks in `main()` into per-command functions in `CliCommands.cpp`:
  - `record` (status/stop/start)
  - `engine` (status/start/stop/restart/midi-bridge/clock)
  - `diagnostics`
  - `nodes|sinks|sources|default-sink|default-source|ports|links`
  - `connect|disconnect`
  - `patchbay` (profiles/hooks/autoconnect/port config)
  - `session`
  - `eq`
  - `set-volume|mute`

### Glue approach (keep file count low)

- Avoid adding many new headers: use forward declarations for helper functions and command handler entry points (declared once in `main.cpp`, defined in the other `.cpp` files).
- If a shared struct becomes necessary, add a single small internal header `src/cli/CliInternal.h` (optional).

### Acceptance checks

- `./build/headroomctl --help` output unchanged.
- `./scripts/make_screenshots.sh` still produces `screenshots/cli-commands.png` reliably.

---

## 2) `headroom-tui` monolith — `src/tui/main.cpp` → ~3 files

### Target split

- `src/tui/main.cpp`
  - Argument parsing (`--help`, `--version`) and a single `runTui()` call.
- `src/tui/TuiApp.cpp`
  - State + main loop + key handling + calls into rendering.
- `src/tui/TuiRender.cpp`
  - Rendering helpers + small “picker” dialogs (EQ preset selection, connect/disconnect pickers, file/template prompt UI).

### What moves where (concrete)

- Rendering-only functions → `TuiRender.cpp`:
  - `drawBar`, `drawHeader`, `drawStatusBar`, `drawHelpOverlay`
  - `drawListPage`, `drawStreamsPage`, `drawPatchbayPage`, `drawEqPage`, `drawRecordingPage`, `drawStatusPage`, `drawEnginePage`
- App loop / actions → `TuiApp.cpp`:
  - page switching, selection indices, help overlay toggle
  - volume/mute/default actions, reorder outputs, connect/disconnect, EQ toggles/presets, recording start/stop, engine start/stop/restart

### Notes

- Keep curses init/teardown in one place (`TuiApp.cpp`) so render/action code assumes curses is ready.

---

## 3) Patchbay UI monolith — `src/ui/PatchbayPage.cpp` → ~3 files

### Target split

- `src/ui/PatchbayPage.cpp`
  - Widget layout (header/profile/filter) + wiring to `PipeWireGraph` + profile CRUD.
- `src/ui/PatchbaySceneBuilder.cpp`
  - Scene rebuild/layout: build all QGraphicsItems from the current PipeWire graph + settings (filter, sink order, saved positions).
- `src/ui/PatchbaySceneController*.cpp`
  - Interaction logic: event filter, selection/hover, drag-to-connect, context menus, connect/disconnect commands/undo.

### What moves where (concrete)

- Builder (`PatchbaySceneBuilder.cpp`):
  - `PatchbayPage::rebuild()` (most of it)
  - Node sorting/filtering logic and scene item construction
  - Initial link path generation and `updateLinkPaths()` (or at least the geometry portion)
- Controller (`PatchbaySceneController.cpp`):
  - `PatchbayPage::eventFilter()`
  - `beginConnectionDrag/updateConnectionDrag/endConnectionDrag/cancelConnectionDrag`
  - hover/selection helpers: `clearSelection`, `clearLinkSelection`, `setHoverPortDot`, `setSelectedLinkId`, `setHoverLinkId`, `updateLinkStyles`, `updatePortDotStyle`
- Controller context menus (`PatchbaySceneControllerContextMenu.cpp`):
  - Context menus for port alias/lock and “Disconnect” on links
- Controller undo (`PatchbaySceneControllerUndo.cpp`):
  - `tryConnectPorts` / `tryDisconnectLink` and the undo commands (`PatchbayConnectCommand`, `PatchbayDisconnectCommand`)

### Required API reshaping (expected)

To keep this split clean, move the “scene state” fields into a small helper struct that both builder and controller can operate on without needing deep access to `PatchbayPage` internals:

- New: `struct PatchbaySceneState` (owned by `PatchbayPage`)
  - `layoutEditMode`
  - `nodeRootByNodeId`, `portLocalPosByNodeId`, `portDotByPortId`, `linkVisualById`
  - selection/hover/drag state (`selectedOut*`, `hoverPortDot`, `selectedLinkId`, `hoverLinkId`, `connectionDragItem`, …)

`PatchbayPage` stays responsible for:

- profile controls (`reloadProfiles/apply/save/delete/edit hooks`)
- settings persistence (`saveLayoutPositions/loadSavedNodePos`)

---

## 4) PipeWire backend monolith — `src/backend/PipeWireGraph.cpp` → ~3 files

### Target split

- `src/backend/PipeWireGraph.cpp`
  - ctor/dtor, caches getters, `audioSinks/audioSources/...`, `scheduleGraphChanged()`.
- `src/backend/PipeWireGraphRegistry.cpp`
  - Registry observation + parsing:
    - `onRegistryGlobal/onRegistryGlobalRemove`
    - `onNodeInfo/onNodeParam`
    - `bindNode/unbindNode`, plus any “cache update” helpers
- `src/backend/PipeWireGraphOpsMetaProfiler.cpp`
  - Metadata + profiler:
    - default devices + clock settings (`setDefaultAudioSink/Source`, `setClock*`, presets/apply, `onMetadataProperty`)
    - profiler snapshot (`onProfilerProfile`, `profilerSnapshot()`)
- `src/backend/PipeWireGraphOpsNodeControls.cpp`
  - Node controls: `setNodeVolume/setNodeMute`
- `src/backend/PipeWireGraphOpsLinks.cpp`
  - Link ops: `createLink/destroyLink`

### Notes

- Keep `pw_thread_loop_lock()` coverage identical to today; the split should not change locking order or what runs on the PipeWire thread loop.
- Keep `m_createdLinkProxies` semantics unchanged (link lifetime / linger behavior).

---

## 5) Mixer UI monolith — `src/ui/MixerPage.cpp` → ~3 files

### Target split

- `src/ui/MixerPage.cpp`
  - Widget wiring + rebuild of sections + meter ticking.
- `src/ui/MixerRows.cpp`
  - Row widget builders:
    - `makeNodeRow` (devices)
    - `makeStreamRow` (streams)
- `src/ui/MixerRouting.cpp`
  - Graph traversal + rewiring helpers:
    - `routeForStream`
    - `portsForNode`
    - `movePlaybackStreamToSink`
    - `moveCaptureStreamToSource`

### Notes

- Keep QtWidgets/UI code in `MixerPage.cpp`/`MixerRows.cpp`; keep routing helpers depending only on QtCore + backend types.

---

## CMake updates (implementation checklist)

When implementing, add new `.cpp` files to the relevant targets in `CMakeLists.txt`:

- `headroom` (GUI): add new `src/ui/*.cpp` and new `src/backend/*.cpp` files.
- `headroom_tui`: add new `src/tui/*.cpp` and new `src/backend/*.cpp` files.
- `headroomctl`: add new `src/cli/*.cpp` and new `src/backend/*.cpp` files.

## Refactor acceptance criteria

- `cmake --build build` succeeds.
- `./scripts/make_screenshots.sh` succeeds (GUI + tray + TUI + CLI).
- No visible behavior regressions in:
  - `headroomctl` command outputs/exit codes
  - TUI key bindings and pages
  - Patchbay drag/connect/disconnect/profile flows
  - Mixer stream routing + meters
