add_executable(signal_studio_ms02_integration_tests ms02_integration_tests.cpp)
target_include_directories(signal_studio_ms02_integration_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/benchmarks")
target_compile_definitions(signal_studio_ms02_integration_tests PRIVATE
  SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR="${SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR}")
target_link_libraries(signal_studio_ms02_integration_tests
  PRIVATE SignalStudio::Data SignalStudio::DSP SignalStudio::Compute)
signal_studio_enable_warnings(signal_studio_ms02_integration_tests)
signal_studio_deploy_msvc_runtime(signal_studio_ms02_integration_tests)
if(SIGNAL_STUDIO_EXTERNAL_TEST_DATA_AVAILABLE)
  add_test(NAME integration.ms02.data-dsp-cache
    COMMAND signal_studio_ms02_integration_tests)
  set_tests_properties(integration.ms02.data-dsp-cache PROPERTIES
    TIMEOUT 60 LABELS "integration;ms-02;data;dsp;compute;external-data")
else()
  add_test(NAME integration.ms02.data-dsp-cache
    COMMAND "${CMAKE_COMMAND}" -E echo
      "SKIP: approved external recordings are unavailable at ${SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR}")
  set_tests_properties(integration.ms02.data-dsp-cache PROPERTIES
    SKIP_REGULAR_EXPRESSION "^SKIP:"
    LABELS "integration;ms-02;external-data;environment-skip")
endif()
