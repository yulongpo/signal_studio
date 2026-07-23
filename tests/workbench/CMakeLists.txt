add_executable(signal_studio_workbench_tests workbench_tests.cpp)
target_link_libraries(signal_studio_workbench_tests PRIVATE SignalStudio::Workbench)
signal_studio_enable_warnings(signal_studio_workbench_tests)
signal_studio_deploy_msvc_runtime(signal_studio_workbench_tests)

foreach(_case IN ITEMS
    service-registry
    command-registry
    panel-factory
    diagnostics)
  add_test(NAME "workbench.${_case}" COMMAND signal_studio_workbench_tests --case "${_case}")
  set_tests_properties("workbench.${_case}" PROPERTIES TIMEOUT 20 LABELS "unit;contract;ms-03;workbench")
endforeach()
