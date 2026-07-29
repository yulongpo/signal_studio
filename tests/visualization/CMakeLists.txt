add_executable(signal_studio_ms03_tests ms03_tests.cpp)
target_link_libraries(signal_studio_ms03_tests
  PRIVATE SignalStudio::Visualization SignalStudio::Workbench
          SignalStudio::Data SignalStudio::TaskRuntime Qt6::Core Qt6::Widgets)
signal_studio_enable_warnings(signal_studio_ms03_tests)
signal_studio_deploy_msvc_runtime(signal_studio_ms03_tests)
signal_studio_deploy_qt_runtime(signal_studio_ms03_tests)

set(_signal_studio_ms03_test_environment
  "QT_QPA_PLATFORM=offscreen"
  "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:signal_studio_ms03_tests>/platforms"
  "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:signal_studio_ms03_tests>")

foreach(_case IN ITEMS
    FR-VIS-001 FR-VIS-002 FR-VIS-003 FR-VIS-004 FR-VIS-005 FR-VIS-006
    FR-VIS-007 FR-VIS-008 FR-VIS-009 FR-VIS-010 FR-VIS-011 FR-VIS-012
    FR-VIS-013 FR-VIS-014 FR-VIS-015 FR-VIS-016 FR-VIS-017 FR-VIS-018
    FR-NAV-001 FR-NAV-002 FR-NAV-003 FR-NAV-004 FR-NAV-005 FR-NAV-006
    FR-NAV-007 FR-NAV-008 FR-NAV-009 FR-NAV-010 FR-NAV-011 FR-NAV-012
    FR-NAV-013 FR-NAV-014 FR-NAV-015
    FR-SEL-001 FR-SEL-002 FR-SEL-003 FR-SEL-004 FR-SEL-005 FR-SEL-006
    FR-SEL-007 FR-SEL-008
    NFR-USA-001 NFR-USA-002 NFR-USA-003 NFR-USA-004 NFR-USA-005
    NFR-USA-006 NFR-USA-007
    FR-VIS-101 FR-WB-101)
  add_test(NAME "ms03.${_case}" COMMAND signal_studio_ms03_tests --case "${_case}")
  set_tests_properties("ms03.${_case}" PROPERTIES
    TIMEOUT 30
    LABELS "qt;ui;unit;contract;requirement;ms-03"
    ENVIRONMENT "${_signal_studio_ms03_test_environment}")
endforeach()

foreach(_size IN ITEMS 1280x720 1600x900 1920x1080 3840x2160)
  string(REPLACE "x" ";" _dimensions "${_size}")
  list(GET _dimensions 0 _width)
  list(GET _dimensions 1 _height)
  add_test(NAME "ms03.ui.${_size}" COMMAND signal_studio_ms03_tests
    --case UI-LAYOUT --width "${_width}" --height "${_height}")
  set_tests_properties("ms03.ui.${_size}" PROPERTIES
    TIMEOUT 30 LABELS "qt;ui;layout;ms-03"
    ENVIRONMENT "${_signal_studio_ms03_test_environment}")
endforeach()

foreach(_dpi_case IN ITEMS
    "100-percent|1"
    "125-percent|1.25"
    "150-percent|1.5"
    "175-percent|1.75"
    "200-percent|2")
  string(REPLACE "|" ";" _dpi_parts "${_dpi_case}")
  list(GET _dpi_parts 0 _dpi_name)
  list(GET _dpi_parts 1 _dpi_scale)
  add_test(NAME "ms03.ui.${_dpi_name}-dpi" COMMAND signal_studio_ms03_tests
    --case UI-LAYOUT --width 1920 --height 1080)
  set_tests_properties("ms03.ui.${_dpi_name}-dpi" PROPERTIES
    TIMEOUT 30 LABELS "qt;ui;dpi;accessibility;ms-03"
    ENVIRONMENT
      "${_signal_studio_ms03_test_environment};QT_SCALE_FACTOR=${_dpi_scale};QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough")
endforeach()

add_test(NAME ms03.ui.screenshot
  COMMAND signal_visualization_workbench_demo
    --screenshot "${CMAKE_CURRENT_BINARY_DIR}/ms03-prototype-1280x720.png"
    --width 1280 --height 720)
set_tests_properties(ms03.ui.screenshot PROPERTIES
  TIMEOUT 30 LABELS "qt;ui;screenshot;ms-03"
  ENVIRONMENT "${_signal_studio_ms03_test_environment}")

add_test(NAME ms03.ui.default-windows-platform
  COMMAND "${CMAKE_COMMAND}" -E env
    --unset=QT_QPA_PLATFORM
    --unset=QT_PLUGIN_PATH
    --unset=QT_QPA_PLATFORM_PLUGIN_PATH
    --
    "$<TARGET_FILE:signal_visualization_workbench_demo>" --startup-smoke)
set_tests_properties(ms03.ui.default-windows-platform PROPERTIES
  TIMEOUT 30 LABELS "qt;ui;default-platform-plugin;ms-03")

foreach(_page IN ITEMS p04 p07)
  add_test(NAME "ms03.ui.screenshot.${_page}"
    COMMAND signal_visualization_workbench_demo
      --screenshot "${CMAKE_CURRENT_BINARY_DIR}/ms03-${_page}-1600x900.png"
      --width 1600 --height 900 --page "${_page}")
  set_tests_properties("ms03.ui.screenshot.${_page}" PROPERTIES
    TIMEOUT 30 LABELS "qt;ui;screenshot;prototype-page;ms-03"
    ENVIRONMENT "${_signal_studio_ms03_test_environment}")
endforeach()
