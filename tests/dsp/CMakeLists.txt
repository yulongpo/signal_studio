add_executable(signal_studio_dsp_tests dsp_tests.cpp)
target_link_libraries(signal_studio_dsp_tests PRIVATE SignalStudio::DSP SignalStudio::Data)
target_compile_definitions(signal_studio_dsp_tests PRIVATE
  SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR="${SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR}")
if(SIGNAL_STUDIO_EXTERNAL_TEST_DATA_AVAILABLE)
  target_compile_definitions(signal_studio_dsp_tests PRIVATE
    SIGNAL_STUDIO_HAVE_EXTERNAL_TEST_DATA=1)
endif()
signal_studio_enable_warnings(signal_studio_dsp_tests)
signal_studio_deploy_msvc_runtime(signal_studio_dsp_tests)

foreach(_case IN ITEMS
    FR-DSP-001 FR-DSP-002 FR-DSP-003 FR-DSP-004 FR-DSP-005 FR-DSP-006
    FR-DSP-007 FR-DSP-008 FR-DSP-009 FR-DSP-010 FR-DSP-011 FR-DSP-012
    NFR-NUM-001 NFR-NUM-002 NFR-NUM-003 NFR-NUM-004 NFR-NUM-005 FR-DSP-101)
  add_test(NAME "dsp.${_case}" COMMAND signal_studio_dsp_tests --case "${_case}")
  set_tests_properties("dsp.${_case}" PROPERTIES
    TIMEOUT 60 LABELS "unit;contract;numerical;requirement;ms-02;dsp")
endforeach()
