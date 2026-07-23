add_executable(signal_studio_core_services_tests core_services_tests.cpp)
target_link_libraries(signal_studio_core_services_tests PRIVATE SignalStudio::Core)
signal_studio_enable_warnings(signal_studio_core_services_tests)
signal_studio_deploy_msvc_runtime(signal_studio_core_services_tests)

foreach(_case IN ITEMS
    result-and-services
    paths-and-atomic-files
    source-fingerprint
    workspace-lifecycle
    workspace-schema
    recent-projects
    current-context)
  add_test(NAME "core.services.${_case}" COMMAND signal_studio_core_services_tests --case "${_case}")
  set_tests_properties("core.services.${_case}" PROPERTIES LABELS "unit;contract;ms-01;core")
endforeach()

function(signal_studio_add_core_requirement_test requirement case_name)
  add_test(NAME "core.${requirement}" COMMAND signal_studio_core_services_tests --case "${case_name}")
  set_tests_properties("core.${requirement}" PROPERTIES LABELS "unit;contract;requirement;ms-01;core")
endfunction()

signal_studio_add_core_requirement_test(FR-PRJ-001 FR-PRJ-001)
signal_studio_add_core_requirement_test(FR-PRJ-002 FR-PRJ-002)
signal_studio_add_core_requirement_test(FR-PRJ-003 FR-PRJ-003)
signal_studio_add_core_requirement_test(FR-PRJ-004 FR-PRJ-004)
signal_studio_add_core_requirement_test(FR-PRJ-005 FR-PRJ-005)
signal_studio_add_core_requirement_test(FR-PRJ-006 FR-PRJ-006)
signal_studio_add_core_requirement_test(FR-PRJ-007 FR-PRJ-007)
signal_studio_add_core_requirement_test(FR-PRJ-008 FR-PRJ-008)
signal_studio_add_core_requirement_test(FR-PRJ-009 FR-PRJ-009)
signal_studio_add_core_requirement_test(FR-PRJ-010 FR-PRJ-010)
signal_studio_add_core_requirement_test(FR-CORE-101 FR-CORE-101)
