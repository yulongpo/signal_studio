add_executable(signal_studio_task_runtime_tests task_runtime_tests.cpp)
target_link_libraries(signal_studio_task_runtime_tests PRIVATE SignalStudio::TaskRuntime)
signal_studio_enable_warnings(signal_studio_task_runtime_tests)
signal_studio_deploy_msvc_runtime(signal_studio_task_runtime_tests)

foreach(_case IN ITEMS
    FR-TSK-001 FR-TSK-002 FR-TSK-003 FR-TSK-004 FR-TSK-005
    FR-TSK-006 FR-TSK-007 FR-TSK-008 FR-TSK-009 FR-TSK-010 FR-TASK-101)
  add_test(NAME "task-runtime.${_case}" COMMAND signal_studio_task_runtime_tests --case "${_case}")
  set_tests_properties("task-runtime.${_case}" PROPERTIES
    TIMEOUT 20 LABELS "unit;contract;requirement;ms-01;task-runtime")
endforeach()
