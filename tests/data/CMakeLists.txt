add_executable(signal_studio_data_tests test_main.cpp)
target_link_libraries(signal_studio_data_tests PRIVATE SignalStudio::Data SignalStudio::TaskRuntime SignalStudio::Core)
signal_studio_enable_warnings(signal_studio_data_tests)
signal_studio_deploy_msvc_runtime(signal_studio_data_tests)

foreach(_case IN ITEMS
    FR-DAT-001 FR-DAT-002 FR-DAT-003 FR-DAT-004 FR-DAT-005 FR-DAT-006 FR-DAT-007 FR-DAT-008 FR-DAT-009
    FR-DAT-010 FR-DAT-011 FR-DAT-012 FR-DAT-013 FR-DAT-014 FR-DAT-015 FR-DAT-016 FR-DAT-017 FR-DAT-018 FR-DAT-019
    FR-IDX-001 FR-IDX-002 FR-IDX-003 FR-IDX-004 FR-IDX-005 FR-IDX-006 FR-IDX-007 FR-IDX-008 FR-IDX-009
    FR-IDX-010 FR-IDX-011 FR-IDX-012 FR-DATA-101)
  add_test(NAME "data.${_case}" COMMAND signal_studio_data_tests --case "${_case}")
  set_tests_properties("data.${_case}" PROPERTIES TIMEOUT 30 LABELS "unit;contract;requirement;ms-01;data")
endforeach()
