cmake_minimum_required(VERSION 3.22)

project(headroom VERSION 0.1.1 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(CCACHE_PROGRAM ccache)
  if (CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  endif()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(HEADROOM_BUILD_TUI "Build headroom-tui (ncurses TUI)" ON)
option(HEADROOM_BUILD_CLI "Build headroomctl (CLI)" ON)
option(HEADROOM_BUILD_GUI "Build headroom (Qt GUI)" ON)
option(HEADROOM_BUILD_TESTS "Build tests" ON)

find_package(Qt6 REQUIRED COMPONENTS Core)
if (HEADROOM_BUILD_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Gui Widgets)
endif()
if (HEADROOM_BUILD_TESTS)
  find_package(Qt6 REQUIRED COMPONENTS Test)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(SPA REQUIRED IMPORTED_TARGET libspa-0.2)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
if (HEADROOM_BUILD_GUI)
  pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
endif()

if (HEADROOM_BUILD_GUI OR HEADROOM_BUILD_TUI OR HEADROOM_BUILD_CLI)
  add_library(headroom_backend_common STATIC
    src/backend/PipeWireThread.cpp
    src/backend/PipeWireThread.h
    src/backend/PipeWireGraph.cpp
    src/backend/PipeWireGraph.h
    src/backend/PipeWireGraphRegistry.cpp
    src/backend/PipeWireGraphOpsLinks.cpp
    src/backend/PipeWireGraphOpsMetadata.cpp
    src/backend/PipeWireGraphOpsMetaProfiler.cpp
    src/backend/PipeWireGraphOpsNodeControls.cpp
    src/backend/PipeWireGraphOpsProfilerSnapshot.cpp
    src/backend/PipeWireGraphInternal.h
    src/backend/PatchbayProfiles.cpp
    src/backend/PatchbayProfiles.h
    src/backend/PatchbayProfileHooks.cpp
    src/backend/PatchbayProfileHooks.h
    src/backend/PatchbayAutoConnectRules.cpp
    src/backend/PatchbayAutoConnectRules.h
    src/backend/PatchbayPortConfig.cpp
    src/backend/PatchbayPortConfig.h
    src/backend/AudioRecorder.cpp
    src/backend/AudioRecorder.h
    src/backend/EngineControl.cpp
    src/backend/EngineControl.h
  )

  target_include_directories(headroom_backend_common PUBLIC src)

  target_link_libraries(headroom_backend_common
    PUBLIC
      Qt6::Core
      PkgConfig::PIPEWIRE
      PkgConfig::SPA
      PkgConfig::SNDFILE
  )

  target_compile_options(headroom_backend_common PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(headroom_backend_common PUBLIC HEADROOM_VERSION="${PROJECT_VERSION}")

  if (HEADROOM_BUILD_GUI OR HEADROOM_BUILD_TUI)
    add_library(headroom_backend_eq STATIC
      src/backend/EqConfig.h
      src/backend/EqManager.cpp
      src/backend/EqManagerPersist.cpp
      src/backend/EqManagerRealtime.cpp
      src/backend/EqManager.h
      src/backend/ParametricEqFilter.cpp
      src/backend/ParametricEqFilterDesign.cpp
      src/backend/ParametricEqFilterResponse.cpp
      src/backend/ParametricEqFilter.h
    )

    target_link_libraries(headroom_backend_eq PUBLIC headroom_backend_common)
    target_compile_options(headroom_backend_eq PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  if (HEADROOM_BUILD_GUI OR HEADROOM_BUILD_CLI)
    add_library(headroom_backend_sessions STATIC
      src/backend/SessionSnapshots.cpp
      src/backend/SessionSnapshotsApply.cpp
      src/backend/SessionSnapshotsSerialize.cpp
      src/backend/SessionSnapshots.h
    )

    target_link_libraries(headroom_backend_sessions PUBLIC headroom_backend_common)
    target_compile_options(headroom_backend_sessions PRIVATE -Wall -Wextra -Wpedantic)

    add_library(headroom_backend_alsa_seq STATIC
      src/backend/AlsaSeqBridge.cpp
      src/backend/AlsaSeqBridge.h
    )

    target_link_libraries(headroom_backend_alsa_seq PUBLIC headroom_backend_common)
    target_compile_options(headroom_backend_alsa_seq PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  if (HEADROOM_BUILD_GUI)
    add_library(headroom_backend_gui STATIC
      src/backend/AudioTap.cpp
      src/backend/AudioTap.h
      src/backend/AudioLevelTap.cpp
      src/backend/AudioLevelTap.h
      src/backend/LogStore.cpp
      src/backend/LogStore.h
      src/backend/PatchbayAutoConnectController.cpp
      src/backend/PatchbayAutoConnectController.h
      src/backend/PatchbayPersistentProfileController.cpp
      src/backend/PatchbayPersistentProfileController.h
      src/dsp/Fft.cpp
      src/dsp/Fft.h
    )

    target_link_libraries(headroom_backend_gui PUBLIC headroom_backend_common)
    target_compile_options(headroom_backend_gui PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if (HEADROOM_BUILD_GUI)
  qt_add_executable(headroom
  	  src/app.qrc
  	  src/main.cpp
  	  src/MainWindow.cpp
  	  src/MainWindowTray.cpp
  	  src/MainWindowActions.cpp
  	  src/MainWindow.h
    src/settings/SettingsKeys.h
    src/settings/VisualizerSettings.h
    src/ui/GraphPage.cpp
    src/ui/GraphPage.h
    src/ui/MixerPage.cpp
    src/ui/MixerPage.h
    src/ui/MixerRows.cpp
    src/ui/MixerRouting.cpp
    src/ui/PatchbayPage.cpp
    src/ui/PatchbayPage.h
    src/ui/PatchbaySceneBuilder.cpp
    src/ui/PatchbaySceneController.cpp
    src/ui/PatchbaySceneControllerContextMenu.cpp
    src/ui/PatchbaySceneControllerUndo.cpp
    src/ui/PatchbaySceneInternal.h
    src/ui/AppTheme.cpp
    src/ui/AppTheme.h
    src/ui/PatchbayProfileHooksDialog.cpp
    src/ui/PatchbayProfileHooksDialog.h
    src/ui/EqDialog.cpp
    src/ui/EqDialog.h
    src/ui/EqResponseWidget.cpp
    src/ui/EqResponseWidget.h
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.h
    src/ui/EngineDialog.cpp
    src/ui/EngineDialogClock.cpp
    src/ui/EngineDialogSystemd.cpp
    src/ui/EngineDialog.h
    src/ui/AutoConnectRulesDialog.cpp
    src/ui/AutoConnectRulesDialog.h
  	  src/ui/SessionsDialog.cpp
  	  src/ui/SessionsDialog.h
  	  src/ui/RecorderDialog.cpp
  	  src/ui/RecorderDialogRecording.cpp
  	  src/ui/RecorderDialogTargets.cpp
  	  src/ui/RecorderDialog.h
  	  src/ui/LogsDialog.cpp
  	  src/ui/LogsDialog.h
  	  src/ui/VisualizerPage.cpp
    src/ui/VisualizerPage.h
    src/ui/VisualizerWidget.cpp
    src/ui/VisualizerWidget.h
    src/ui/LevelMeterWidget.cpp
    src/ui/LevelMeterWidget.h
    src/ui/VolumeSlider.cpp
    src/ui/VolumeSlider.h
  )

  target_link_libraries(headroom
    PRIVATE
      headroom_backend_common
      headroom_backend_eq
      headroom_backend_sessions
      headroom_backend_alsa_seq
      headroom_backend_gui
      Qt6::Core
      Qt6::Gui
      Qt6::Widgets
      PkgConfig::XCB
  )

  target_compile_options(headroom PRIVATE -Wall -Wextra -Wpedantic)

  install(TARGETS headroom RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if (HEADROOM_BUILD_TUI)
  set(CURSES_NEED_NCURSES TRUE)
  set(CURSES_NEED_WIDE TRUE)
  find_package(Curses REQUIRED)

  qt_add_executable(headroom_tui
    src/tui/main.cpp
    src/tui/TuiApp.cpp
    src/tui/TuiAppActions.cpp
    src/tui/TuiAppActionsDevices.cpp
    src/tui/TuiAppActionsPatchbay.cpp
    src/tui/TuiAppInternal.h
    src/tui/TuiAppRender.cpp
    src/tui/TuiPrompts.cpp
    src/tui/TuiRenderAudio.cpp
	    src/tui/TuiRenderEq.cpp
	    src/tui/TuiRenderRecording.cpp
	    src/tui/TuiRenderSystem.cpp
	    src/tui/TuiUtil.cpp
	    src/tui/TuiUtilFormat.cpp
	    src/tui/TuiUtilPrompts.cpp
	    src/tui/TuiInternal.h
  )

  set_target_properties(headroom_tui PROPERTIES OUTPUT_NAME headroom-tui)

  target_include_directories(headroom_tui PRIVATE ${CURSES_INCLUDE_DIRS})

  target_link_libraries(headroom_tui
    PRIVATE
      headroom_backend_common
      headroom_backend_eq
      Qt6::Core
      ${CURSES_LIBRARIES}
  )

  target_compile_options(headroom_tui PRIVATE -Wall -Wextra -Wpedantic)

  install(TARGETS headroom_tui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if (HEADROOM_BUILD_CLI)
  qt_add_executable(headroomctl
    src/cli/main.cpp
    src/cli/CliCommands.cpp
    src/cli/CliCommandsEngine.cpp
    src/cli/CliCommandsEngineClock.cpp
    src/cli/CliCommandsEngineMidiBridge.cpp
    src/cli/CliCommandsEngineSystemd.cpp
    src/cli/CliCommandsEq.cpp
    src/cli/CliCommandsGraph.cpp
    src/cli/CliCommandsGraphConnections.cpp
    src/cli/CliCommandsGraphControls.cpp
    src/cli/CliCommandsGraphListing.cpp
    src/cli/CliCommandsPatchbay.cpp
    src/cli/CliCommandsPatchbayAutoconnect.cpp
    src/cli/CliCommandsPatchbayHooks.cpp
    src/cli/CliCommandsPatchbayPortConfig.cpp
    src/cli/CliCommandsRecord.cpp
    src/cli/CliCommandsSession.cpp
    src/cli/CliUtil.cpp
    src/cli/CliInternal.h
  )

  target_link_libraries(headroomctl
    PRIVATE
      headroom_backend_common
      headroom_backend_sessions
      headroom_backend_alsa_seq
      Qt6::Core
  )

  target_compile_options(headroomctl PRIVATE -Wall -Wextra -Wpedantic)

  install(TARGETS headroomctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if (HEADROOM_BUILD_GUI)
  install(FILES resources/com.maxheadroom.Headroom.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES resources/com.maxheadroom.Headroom.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
  install(FILES resources/icons/com.maxheadroom.Headroom.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

if (HEADROOM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
