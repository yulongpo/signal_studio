add_executable(signal_studio_dataset_tests dataset_tests.cpp)
target_link_libraries(signal_studio_dataset_tests PRIVATE SignalStudio::Dataset)
signal_studio_enable_warnings(signal_studio_dataset_tests)
signal_studio_deploy_msvc_runtime(signal_studio_dataset_tests)

foreach(_case IN ITEMS append-query-commit roundtrip)
  add_test(NAME "dataset.${_case}" COMMAND signal_studio_dataset_tests --case "${_case}")
  set_tests_properties("dataset.${_case}" PROPERTIES TIMEOUT 20 LABELS "unit;contract;ms-06;dataset")
endforeach()
