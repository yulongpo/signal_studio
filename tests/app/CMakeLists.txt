add_executable(signal_studio_ms04_tests ms04_tests.cpp)
target_link_libraries(signal_studio_ms04_tests
  PRIVATE SignalStudioApplicationCore SignalStudio::Core SignalStudio::Data
          SignalStudio::DSP SignalStudio::Workbench)
target_compile_definitions(signal_studio_ms04_tests PRIVATE
  SIGNAL_STUDIO_MS04_EXTERNAL_DATA_DIR="${SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR}")
signal_studio_enable_warnings(signal_studio_ms04_tests)
signal_studio_deploy_msvc_runtime(signal_studio_ms04_tests)

foreach(_case IN ITEMS
    FR-INS-001 FR-INS-002 FR-INS-003 FR-INS-004 FR-INS-005 FR-INS-006 FR-INS-007
    FR-EXP-001 FR-EXP-002 FR-EXP-003 FR-EXP-004 FR-EXP-005 FR-EXP-006 FR-EXP-007
    FR-EXP-008 FR-EXP-009
    NFR-REL-001 NFR-REL-002 NFR-REL-003 NFR-REL-004
    APP-PROJECT-IMPORT-ANALYSIS APP-CANCEL-RETRY APP-ERROR-RECOVERY
    APP-HEADLESS-SELF-TEST EXTERNAL-ALL-RECORDINGS)
  add_test(NAME "ms04.${_case}" COMMAND signal_studio_ms04_tests --case "${_case}")
  set_tests_properties("ms04.${_case}" PROPERTIES
    TIMEOUT 120 LABELS "unit;integration;requirement;ms-04")
endforeach()

add_test(NAME ms04.app.headless-self-test-before-qt
  COMMAND signal_studio --self-test --scratch "${CMAKE_CURRENT_BINARY_DIR}/headless-self-test")
set_tests_properties(ms04.app.headless-self-test-before-qt PROPERTIES
  TIMEOUT 60 LABELS "headless;startup;ms-04")

set(_signal_studio_ms04_ui_environment
  "QT_QPA_PLATFORM=offscreen"
  "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:signal_studio>/platforms"
  "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:signal_studio>")

foreach(_size IN ITEMS 1280x720 1920x1080 3840x2160)
  string(REPLACE "x" ";" _dimensions "${_size}")
  list(GET _dimensions 0 _width)
  list(GET _dimensions 1 _height)
  add_test(NAME "ms04.ui.${_size}"
    COMMAND signal_studio --startup-smoke --page p01 --width "${_width}" --height "${_height}"
      --state-dir "${CMAKE_CURRENT_BINARY_DIR}/state-${_size}")
  set_tests_properties("ms04.ui.${_size}" PROPERTIES
    TIMEOUT 30 LABELS "qt;ui;layout;ms-04"
    ENVIRONMENT "${_signal_studio_ms04_ui_environment}")
endforeach()

foreach(_dpi_case IN ITEMS "150-percent|1.5" "200-percent|2")
  string(REPLACE "|" ";" _dpi_parts "${_dpi_case}")
  list(GET _dpi_parts 0 _dpi_name)
  list(GET _dpi_parts 1 _dpi_scale)
  add_test(NAME "ms04.ui.${_dpi_name}-dpi"
    COMMAND signal_studio --startup-smoke --page p01 --width 1920 --height 1080
      --state-dir "${CMAKE_CURRENT_BINARY_DIR}/state-${_dpi_name}")
  set_tests_properties("ms04.ui.${_dpi_name}-dpi" PROPERTIES
    TIMEOUT 30 LABELS "qt;ui;dpi;ms-04"
    ENVIRONMENT
      "${_signal_studio_ms04_ui_environment};QT_SCALE_FACTOR=${_dpi_scale};QT_SCALE_FACTOR_ROUNDING_POLICY=PassThrough")
endforeach()

add_test(NAME ms04.ui.default-windows-platform
  COMMAND "${CMAKE_COMMAND}" -E env
    --unset=QT_QPA_PLATFORM --unset=QT_PLUGIN_PATH --unset=QT_QPA_PLATFORM_PLUGIN_PATH --
    "$<TARGET_FILE:signal_studio>" --startup-smoke --page p01
      --state-dir "${CMAKE_CURRENT_BINARY_DIR}/state-windows")
set_tests_properties(ms04.ui.default-windows-platform PROPERTIES
  TIMEOUT 30 LABELS "qt;ui;default-platform-plugin;ms-04")

foreach(_page IN ITEMS p01)
  add_test(NAME "ms04.ui.screenshot.${_page}"
    COMMAND signal_studio
      --screenshot "${CMAKE_CURRENT_BINARY_DIR}/ms04-${_page}-1600x900.png"
      --page "${_page}" --width 1600 --height 900
      --state-dir "${CMAKE_CURRENT_BINARY_DIR}/state-${_page}")
  set_tests_properties("ms04.ui.screenshot.${_page}" PROPERTIES
    TIMEOUT 30 LABELS "qt;ui;screenshot;ms-04"
    ENVIRONMENT "${_signal_studio_ms04_ui_environment}")
endforeach()

if(SIGNAL_STUDIO_MS02_PERF_RECORDING_AVAILABLE)
  foreach(_page IN ITEMS w01 p02 p03 p05 w05)
    add_test(NAME "ms04.ui.screenshot.${_page}"
      COMMAND signal_studio
        --input "${SIGNAL_STUDIO_MS02_PERF_RECORDING}"
        --screenshot "${CMAKE_CURRENT_BINARY_DIR}/ms04-${_page}-1600x900.png"
        --page "${_page}" --width 1600 --height 900
        --state-dir "${CMAKE_CURRENT_BINARY_DIR}/state-${_page}")
    set_tests_properties("ms04.ui.screenshot.${_page}" PROPERTIES
      TIMEOUT 120 LABELS "qt;ui;screenshot;external-data;ms-04"
      ENVIRONMENT "${_signal_studio_ms04_ui_environment}")
  endforeach()
endif()
