cmake_minimum_required(VERSION 3.22)

project(headroom VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(HEADROOM_BUILD_TUI "Build headroom-tui (ncurses TUI)" ON)
option(HEADROOM_BUILD_CLI "Build headroomctl (CLI)" ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(SPA REQUIRED IMPORTED_TARGET libspa-0.2)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

qt_add_executable(headroom
  src/app.qrc
  src/main.cpp
  src/MainWindow.cpp
  src/MainWindow.h
  src/backend/PipeWireThread.cpp
  src/backend/PipeWireThread.h
  src/backend/PipeWireGraph.cpp
  src/backend/PipeWireGraph.h
  src/backend/PatchbayProfiles.cpp
  src/backend/PatchbayProfiles.h
  src/backend/PatchbayProfileHooks.cpp
  src/backend/PatchbayProfileHooks.h
  src/backend/PatchbayAutoConnectRules.cpp
  src/backend/PatchbayAutoConnectRules.h
  src/backend/PatchbayPortConfig.cpp
  src/backend/PatchbayPortConfig.h
  src/backend/PatchbayAutoConnectController.cpp
  src/backend/PatchbayAutoConnectController.h
  src/backend/SessionSnapshots.cpp
  src/backend/SessionSnapshots.h
  src/backend/AudioTap.cpp
  src/backend/AudioTap.h
  src/backend/AudioLevelTap.cpp
  src/backend/AudioLevelTap.h
  src/backend/AudioRecorder.cpp
  src/backend/AudioRecorder.h
  src/backend/AlsaSeqBridge.cpp
  src/backend/AlsaSeqBridge.h
  src/backend/EngineControl.cpp
  src/backend/EngineControl.h
  src/backend/LogStore.cpp
  src/backend/LogStore.h
  src/backend/EqConfig.h
  src/backend/EqManager.cpp
  src/backend/EqManager.h
  src/backend/ParametricEqFilter.cpp
  src/backend/ParametricEqFilter.h
  src/dsp/Fft.cpp
  src/dsp/Fft.h
  src/settings/SettingsKeys.h
  src/settings/VisualizerSettings.h
  src/ui/GraphPage.cpp
  src/ui/GraphPage.h
  src/ui/MixerPage.cpp
  src/ui/MixerPage.h
  src/ui/PatchbayPage.cpp
  src/ui/PatchbayPage.h
  src/ui/PatchbayProfileHooksDialog.cpp
  src/ui/PatchbayProfileHooksDialog.h
  src/ui/EqDialog.cpp
  src/ui/EqDialog.h
  src/ui/EqResponseWidget.cpp
  src/ui/EqResponseWidget.h
  src/ui/SettingsDialog.cpp
  src/ui/SettingsDialog.h
  src/ui/EngineDialog.cpp
  src/ui/EngineDialog.h
  src/ui/AutoConnectRulesDialog.cpp
  src/ui/AutoConnectRulesDialog.h
  src/ui/SessionsDialog.cpp
  src/ui/SessionsDialog.h
  src/ui/RecorderDialog.cpp
  src/ui/RecorderDialog.h
  src/ui/LogsDialog.cpp
  src/ui/LogsDialog.h
  src/ui/VisualizerPage.cpp
  src/ui/VisualizerPage.h
  src/ui/VisualizerWidget.cpp
  src/ui/VisualizerWidget.h
  src/ui/LevelMeterWidget.cpp
  src/ui/LevelMeterWidget.h
)

target_include_directories(headroom PRIVATE src)

target_link_libraries(headroom
  PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    PkgConfig::PIPEWIRE
    PkgConfig::SPA
    PkgConfig::SNDFILE
)

target_compile_options(headroom PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(headroom PRIVATE HEADROOM_VERSION="${PROJECT_VERSION}")

install(TARGETS headroom RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if (HEADROOM_BUILD_TUI)
  set(CURSES_NEED_NCURSES TRUE)
  set(CURSES_NEED_WIDE TRUE)
  find_package(Curses REQUIRED)

  qt_add_executable(headroom_tui
    src/tui/main.cpp
    src/backend/PipeWireThread.cpp
    src/backend/PipeWireThread.h
    src/backend/PipeWireGraph.cpp
    src/backend/PipeWireGraph.h
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
    src/backend/EqConfig.h
    src/backend/EqManager.cpp
    src/backend/EqManager.h
    src/backend/ParametricEqFilter.cpp
    src/backend/ParametricEqFilter.h
  )

  set_target_properties(headroom_tui PROPERTIES OUTPUT_NAME headroom-tui)

  target_include_directories(headroom_tui PRIVATE src ${CURSES_INCLUDE_DIRS})

  target_link_libraries(headroom_tui
    PRIVATE
      Qt6::Core
      PkgConfig::PIPEWIRE
      PkgConfig::SPA
      PkgConfig::SNDFILE
      ${CURSES_LIBRARIES}
  )

  target_compile_options(headroom_tui PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(headroom_tui PRIVATE HEADROOM_VERSION="${PROJECT_VERSION}")

  install(TARGETS headroom_tui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if (HEADROOM_BUILD_CLI)
  qt_add_executable(headroomctl
    src/cli/main.cpp
    src/backend/PipeWireThread.cpp
    src/backend/PipeWireThread.h
    src/backend/PipeWireGraph.cpp
    src/backend/PipeWireGraph.h
    src/backend/PatchbayProfiles.cpp
    src/backend/PatchbayProfiles.h
    src/backend/PatchbayProfileHooks.cpp
    src/backend/PatchbayProfileHooks.h
    src/backend/PatchbayAutoConnectRules.cpp
    src/backend/PatchbayAutoConnectRules.h
    src/backend/PatchbayPortConfig.cpp
    src/backend/PatchbayPortConfig.h
    src/backend/SessionSnapshots.cpp
    src/backend/SessionSnapshots.h
    src/backend/AudioRecorder.cpp
    src/backend/AudioRecorder.h
    src/backend/AlsaSeqBridge.cpp
    src/backend/AlsaSeqBridge.h
    src/backend/EngineControl.cpp
    src/backend/EngineControl.h
  )

  target_include_directories(headroomctl PRIVATE src)

  target_link_libraries(headroomctl
    PRIVATE
      Qt6::Core
      PkgConfig::PIPEWIRE
      PkgConfig::SPA
      PkgConfig::SNDFILE
  )

  target_compile_options(headroomctl PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(headroomctl PRIVATE HEADROOM_VERSION="${PROJECT_VERSION}")

  install(TARGETS headroomctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(FILES resources/com.maxheadroom.Headroom.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/com.maxheadroom.Headroom.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES resources/icons/com.maxheadroom.Headroom.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
