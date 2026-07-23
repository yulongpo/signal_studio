add_executable(signal_studio_model_tests model_tests.cpp)
target_link_libraries(signal_studio_model_tests PRIVATE SignalStudio::ModelRuntime)
signal_studio_enable_warnings(signal_studio_model_tests)
signal_studio_deploy_msvc_runtime(signal_studio_model_tests)

foreach(_case IN ITEMS registry-install-resolve null-session-unavailable)
  add_test(NAME "model.${_case}" COMMAND signal_studio_model_tests --case "${_case}")
  set_tests_properties("model.${_case}" PROPERTIES TIMEOUT 20 LABELS "unit;contract;ms-06;model-runtime")
endforeach()
