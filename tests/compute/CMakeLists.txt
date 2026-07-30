add_executable(signal_studio_compute_tests compute_tests.cpp)
target_include_directories(signal_studio_compute_tests PRIVATE "${PROJECT_SOURCE_DIR}/benchmarks")
target_compile_definitions(signal_studio_compute_tests PRIVATE
  SIGNAL_STUDIO_MS02_PERF_RECORDING="${SIGNAL_STUDIO_MS02_PERF_RECORDING}"
  SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256="${SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256}")
target_link_libraries(signal_studio_compute_tests
  PRIVATE SignalStudio::Compute SignalStudio::Data SignalStudio::DSP)
if(WIN32)
  target_link_libraries(signal_studio_compute_tests PRIVATE Psapi)
endif()
signal_studio_enable_warnings(signal_studio_compute_tests)
signal_studio_deploy_msvc_runtime(signal_studio_compute_tests)

foreach(_case IN ITEMS NFR-PERF-001 NFR-PERF-002 NFR-PERF-007 NFR-PERF-011 FR-COMPUTE-101)
  add_test(NAME "compute.${_case}" COMMAND signal_studio_compute_tests --case "${_case}")
  set_tests_properties("compute.${_case}" PROPERTIES
    TIMEOUT 30 LABELS "unit;contract;performance;requirement;ms-02;compute")
endforeach()

foreach(_case IN ITEMS NFR-PERF-003 NFR-PERF-004 NFR-PERF-005 NFR-PERF-006 NFR-PERF-008 NFR-PERF-009 NFR-PERF-010)
  if(SIGNAL_STUDIO_MS02_PERF_RECORDING_AVAILABLE)
    add_test(NAME "compute.${_case}" COMMAND signal_studio_compute_tests --case "${_case}")
    set_tests_properties("compute.${_case}" PROPERTIES
      TIMEOUT 60 LABELS "integration;external-data;performance;requirement;ms-02;compute")
  else()
    add_test(NAME "compute.${_case}" COMMAND "${CMAKE_COMMAND}" -E echo
      "SKIP: MS-02真实X310录制缺失；无法执行真实录制输入+100 GB（100,000,000,000字节）逻辑重复映射专项")
    set_tests_properties("compute.${_case}" PROPERTIES
      SKIP_REGULAR_EXPRESSION "^SKIP:"
      LABELS "integration;external-data;environment-skip;performance;requirement;ms-02;compute")
  endif()
endforeach()

# 完整索引测试会与同盘顺序读取基线比较吞吐；并发磁盘基准会污染该比值。
set_tests_properties(compute.NFR-PERF-009 PROPERTIES RUN_SERIAL TRUE TIMEOUT 900)

if(SIGNAL_STUDIO_BUILD_UI)
  add_executable(signal_studio_ms02_qt_performance_tests qt_performance_tests.cpp)
  target_compile_definitions(signal_studio_ms02_qt_performance_tests PRIVATE
    SIGNAL_STUDIO_MS02_PERF_RECORDING="${SIGNAL_STUDIO_MS02_PERF_RECORDING}"
    SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256="${SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256}")
  target_link_libraries(signal_studio_ms02_qt_performance_tests
    PRIVATE SignalStudio::DSP SignalStudio::Data SignalStudio::Compute Qt6::Core Qt6::Widgets)
  signal_studio_enable_warnings(signal_studio_ms02_qt_performance_tests)
  signal_studio_deploy_msvc_runtime(signal_studio_ms02_qt_performance_tests)
  if(NOT TARGET Qt6::QOffscreenIntegrationPlugin OR
     NOT TARGET Qt6::QWindowsIntegrationPlugin)
    message(FATAL_ERROR "MS-02 Qt 性能专项需要 Qt offscreen/windows 平台插件导入目标")
  endif()
  add_custom_command(TARGET signal_studio_ms02_qt_performance_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "$<TARGET_FILE_DIR:signal_studio_ms02_qt_performance_tests>/platforms"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
      "$<TARGET_FILE_DIR:signal_studio_ms02_qt_performance_tests>/platforms/"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
      "$<TARGET_FILE_DIR:signal_studio_ms02_qt_performance_tests>/platforms/"
    COMMENT "部署 MS-02 Qt 性能专项所需的平台插件")
  set(_signal_studio_ms02_qt_test_environment
    "QT_QPA_PLATFORM=offscreen"
    "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:signal_studio_ms02_qt_performance_tests>/platforms"
    "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:signal_studio_ms02_qt_performance_tests>")

  foreach(_case IN ITEMS NFR-PERF-001 NFR-PERF-002)
    add_test(NAME "performance.qt.${_case}"
      COMMAND signal_studio_ms02_qt_performance_tests --case "${_case}")
    set_tests_properties("performance.qt.${_case}" PROPERTIES
      TIMEOUT 30 LABELS "qt;ui;performance;requirement;ms-02;actual-paint"
      ENVIRONMENT "${_signal_studio_ms02_qt_test_environment}")
  endforeach()
  add_test(NAME performance.qt.default-platform-plugin
    COMMAND "${CMAKE_COMMAND}" -E env
      --unset=QT_QPA_PLATFORM
      --unset=QT_PLUGIN_PATH
      --unset=QT_QPA_PLATFORM_PLUGIN_PATH
      --
      "$<TARGET_FILE:signal_studio_ms02_qt_performance_tests>" --startup-smoke)
  set_tests_properties(performance.qt.default-platform-plugin PROPERTIES
    TIMEOUT 30 LABELS "qt;ui;regression;ms-02;default-platform-plugin")
  if(SIGNAL_STUDIO_MS02_PERF_RECORDING_AVAILABLE)
    add_test(NAME performance.qt.NFR-PERF-010
      COMMAND signal_studio_ms02_qt_performance_tests --case NFR-PERF-010)
    set_tests_properties(performance.qt.NFR-PERF-010 PROPERTIES
      TIMEOUT 60 RUN_SERIAL TRUE
      LABELS "qt;ui;external-data;performance;requirement;ms-02;actual-paint"
      ENVIRONMENT "${_signal_studio_ms02_qt_test_environment}")
  else()
    add_test(NAME performance.qt.NFR-PERF-010 COMMAND "${CMAKE_COMMAND}" -E echo
      "SKIP: MS-02真实X310录制缺失；无法执行Qt三图实际绘制专项")
    set_tests_properties(performance.qt.NFR-PERF-010 PROPERTIES
      SKIP_REGULAR_EXPRESSION "^SKIP:"
      LABELS "qt;ui;external-data;environment-skip;performance;requirement;ms-02")
  endif()
endif()
