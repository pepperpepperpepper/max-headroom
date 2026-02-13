add_executable(headroom_unit_tests
  test_fft.cpp
  ../src/dsp/Fft.cpp
)

target_include_directories(headroom_unit_tests PRIVATE ../src)

target_link_libraries(headroom_unit_tests
  PRIVATE
    Qt6::Core
    Qt6::Test
)

target_compile_options(headroom_unit_tests PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME headroom_unit_tests COMMAND headroom_unit_tests)

add_executable(headroom_volume_scale_tests
  test_volume_scale.cpp
)

target_include_directories(headroom_volume_scale_tests PRIVATE ../src)

target_link_libraries(headroom_volume_scale_tests
  PRIVATE
    Qt6::Core
    Qt6::Test
)

target_compile_options(headroom_volume_scale_tests PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME headroom_volume_scale_tests COMMAND headroom_volume_scale_tests)

if (TARGET headroomctl)
  add_test(
    NAME headroomctl_contract_nodes_json
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_contract_nodes.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_contract_sinks_json
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_contract_sinks.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_contract_sources_json
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_contract_sources.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_contract_default_devices_json
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_contract_default_devices.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_contract_ports_json
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_contract_ports.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_contract_links_json
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_contract_links.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_contract_engine_status_json
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_contract_engine_status.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_contract_diagnostics_json
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_contract_diagnostics.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_integration_controls
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_integration_controls.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_integration_connections
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_integration_connections.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_integration_patchbay_profiles
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_integration_patchbay_profiles.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_integration_autoconnect
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_integration_autoconnect.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_integration_sessions
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_integration_sessions.sh $<TARGET_FILE:headroomctl>
  )

  add_test(
    NAME headroomctl_integration_recording
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomctl_integration_recording.sh $<TARGET_FILE:headroomctl>
  )

	  if (TARGET headroom_tui)
	    add_test(
	      NAME headroom_tui_smoke
	      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomtui_smoke.sh $<TARGET_FILE:headroom_tui> $<TARGET_FILE:headroomctl>
	    )

	    add_test(
	      NAME headroom_tui_efficiency_idle
	      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroomtui_efficiency_idle.sh $<TARGET_FILE:headroom_tui>
	    )
	  endif()
	endif()

if (TARGET headroom)
  add_test(
    NAME headroom_gui_smoke_offscreen
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroom_gui_smoke_offscreen.sh $<TARGET_FILE:headroom>
  )

  add_test(
    NAME headroom_gui_efficiency_meters_hide
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroom_gui_efficiency_meters_hide.sh $<TARGET_FILE:headroom>
  )

  add_test(
    NAME headroom_gui_efficiency_meters_minimized
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroom_gui_efficiency_meters_minimized.sh $<TARGET_FILE:headroom>
  )

  add_test(
    NAME headroom_gui_efficiency_meters_visible
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroom_gui_efficiency_meters_visible.sh $<TARGET_FILE:headroom>
  )

  add_test(
    NAME headroom_gui_efficiency_visualizer_hide
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroom_gui_efficiency_visualizer_hide.sh $<TARGET_FILE:headroom>
  )

  add_test(
    NAME headroom_gui_efficiency_visualizer_minimized
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroom_gui_efficiency_visualizer_minimized.sh $<TARGET_FILE:headroom>
  )
endif()

if (TARGET headroom AND TARGET headroomctl)
  add_test(
    NAME headroom_tray_demo_smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/headroom_tray_demo_smoke.sh $<TARGET_FILE:headroom> $<TARGET_FILE:headroomctl>
  )
endif()
