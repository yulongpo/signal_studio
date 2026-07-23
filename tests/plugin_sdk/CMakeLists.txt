add_executable(signal_studio_plugin_tests plugin_tests.cpp)
target_link_libraries(signal_studio_plugin_tests PRIVATE SignalStudio::PluginSDK)
signal_studio_enable_warnings(signal_studio_plugin_tests)
signal_studio_deploy_msvc_runtime(signal_studio_plugin_tests)

foreach(_case IN ITEMS algorithm-rms algorithm-bad-operation registry-validate)
  add_test(NAME "plugin.${_case}" COMMAND signal_studio_plugin_tests --case "${_case}")
  set_tests_properties("plugin.${_case}" PROPERTIES TIMEOUT 20 LABELS "unit;contract;ms-06;plugin-sdk")
endforeach()

# ABI runtime loading test uses the minimal_plugin shared library built by the SDK example. It
# registers only when that target exists (headless + UI builds both build it).
if(TARGET signal_studio_minimal_plugin)
  add_test(NAME "plugin.host-load-minimal" COMMAND signal_studio_plugin_tests --case host-load-minimal)
  set_tests_properties("plugin.host-load-minimal" PROPERTIES TIMEOUT 20 LABELS "unit;contract;ms-06;plugin-sdk"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
