add_executable(signal_studio_platform_tests test_main.cpp)
target_compile_definitions(signal_studio_platform_tests PRIVATE SIGNAL_STUDIO_TEST_HAS_UI=$<BOOL:${SIGNAL_STUDIO_BUILD_UI}>)
target_link_libraries(signal_studio_platform_tests PRIVATE SignalStudio::Core SignalStudio::Data SignalStudio::DSP
  SignalStudio::Compute SignalStudio::TaskRuntime SignalStudio::PluginSDK SignalStudio::ModelRuntime SignalStudio::Dataset)
if(SIGNAL_STUDIO_BUILD_UI)
  target_link_libraries(signal_studio_platform_tests PRIVATE SignalStudio::Visualization SignalStudio::Workbench)
endif()
signal_studio_enable_warnings(signal_studio_platform_tests)
signal_studio_deploy_msvc_runtime(signal_studio_platform_tests)
if(WIN32 AND SIGNAL_STUDIO_BUILD_UI)
  add_custom_command(TARGET signal_studio_platform_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      $<TARGET_RUNTIME_DLLS:signal_studio_platform_tests>
      $<TARGET_FILE_DIR:signal_studio_platform_tests>
    COMMAND_EXPAND_LISTS
    COMMENT "Copying platform-test runtime DLLs for direct launch")
endif()

set(_module_cases core compute data task-runtime dsp model-runtime dataset plugin-sdk)
if(SIGNAL_STUDIO_BUILD_UI)
  list(APPEND _module_cases visualization workbench)
endif()
foreach(_module IN LISTS _module_cases)
  add_test(NAME "module.${_module}.contract" COMMAND signal_studio_platform_tests --case "module.${_module}")
  set_tests_properties("module.${_module}.contract" PROPERTIES LABELS "unit;contract;compatibility;${_module}")
endforeach()
foreach(_case IN ITEMS core.version core.errors.contract core.errors.invariants core.errors.serialization core.errors.propagation
    core.capabilities core.module-descriptor-validation graph.compatibility)
  string(REPLACE "." "_" _safe_case "${_case}")
  add_test(NAME "platform.${_case}" COMMAND signal_studio_platform_tests --case "${_case}")
  set_tests_properties("platform.${_case}" PROPERTIES LABELS "unit;contract;compatibility")
endforeach()

add_executable(signal_plugin_abi_c_consumer abi_c_consumer.c)
set_target_properties(signal_plugin_abi_c_consumer PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(signal_plugin_abi_c_consumer PRIVATE SignalStudio::PluginSDK)
signal_studio_enable_warnings(signal_plugin_abi_c_consumer)
signal_studio_deploy_msvc_runtime(signal_plugin_abi_c_consumer)
add_test(NAME compatibility.plugin_abi.c_compile_link COMMAND signal_plugin_abi_c_consumer)

add_executable(signal_plugin_abi_cpp_consumer abi_cpp_consumer.cpp)
target_link_libraries(signal_plugin_abi_cpp_consumer PRIVATE SignalStudio::PluginSDK)
signal_studio_enable_warnings(signal_plugin_abi_cpp_consumer)
signal_studio_deploy_msvc_runtime(signal_plugin_abi_cpp_consumer)
add_test(NAME compatibility.plugin_abi.cpp_compile_link COMMAND signal_plugin_abi_cpp_consumer)

add_executable(signal_plugin_abi_exception_boundary plugin_abi_exception_boundary.cpp)
target_link_libraries(signal_plugin_abi_exception_boundary PRIVATE SignalStudio::PluginSDK)
signal_studio_enable_warnings(signal_plugin_abi_exception_boundary)
signal_studio_deploy_msvc_runtime(signal_plugin_abi_exception_boundary)
add_test(NAME compatibility.plugin_abi.cpp_exception_boundary COMMAND signal_plugin_abi_exception_boundary)
set_tests_properties(compatibility.plugin_abi.cpp_exception_boundary PROPERTIES LABELS "compatibility;plugin-sdk;abi")

if(TARGET signal_studio_minimal_plugin)
  add_executable(signal_plugin_abi_runtime plugin_abi_runtime.cpp)
  target_link_libraries(signal_plugin_abi_runtime PRIVATE SignalStudio::PluginSDK ${CMAKE_DL_LIBS})
  signal_studio_enable_warnings(signal_plugin_abi_runtime)
  signal_studio_deploy_msvc_runtime(signal_plugin_abi_runtime)
  add_test(NAME compatibility.plugin_abi.export_symbol_and_version_rejection
    COMMAND signal_plugin_abi_runtime "$<TARGET_FILE:signal_studio_minimal_plugin>")
endif()

add_executable(signal_studio_benchmark_smoke benchmark_smoke.cpp)
target_link_libraries(signal_studio_benchmark_smoke PRIVATE SignalStudio::Core)
signal_studio_enable_warnings(signal_studio_benchmark_smoke)
signal_studio_deploy_msvc_runtime(signal_studio_benchmark_smoke)
add_test(NAME performance.platform.non_release_smoke COMMAND signal_studio_benchmark_smoke)
set_tests_properties(performance.platform.non_release_smoke PROPERTIES TIMEOUT 10 LABELS "performance;non-release-threshold")

add_executable(signal_studio_module_performance_smoke module_performance_smoke.cpp)
target_compile_definitions(signal_studio_module_performance_smoke
  PRIVATE SIGNAL_STUDIO_TEST_HAS_UI=$<BOOL:${SIGNAL_STUDIO_BUILD_UI}>)
target_link_libraries(signal_studio_module_performance_smoke PRIVATE SignalStudio::Core SignalStudio::Data
  SignalStudio::DSP SignalStudio::Compute SignalStudio::TaskRuntime SignalStudio::PluginSDK
  SignalStudio::ModelRuntime SignalStudio::Dataset)
if(SIGNAL_STUDIO_BUILD_UI)
  target_link_libraries(signal_studio_module_performance_smoke PRIVATE SignalStudio::Visualization SignalStudio::Workbench)
endif()
signal_studio_enable_warnings(signal_studio_module_performance_smoke)
signal_studio_deploy_msvc_runtime(signal_studio_module_performance_smoke)
foreach(_module IN LISTS _module_cases)
  add_test(NAME "performance.module.${_module}.non_release_smoke"
    COMMAND signal_studio_module_performance_smoke --module "${_module}")
  set_tests_properties("performance.module.${_module}.non_release_smoke" PROPERTIES
    TIMEOUT 10 LABELS "performance;non-release-threshold;${_module}")
endforeach()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
foreach(_manifest IN ITEMS baseline external minimal public-headers dependency-lock portable-config)
  string(REPLACE "-" "_" _manifest_name "${_manifest}")
  add_test(NAME "verification.${_manifest_name}" COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_manifests.py" "${_manifest}")
  set_tests_properties("verification.${_manifest_name}" PROPERTIES
    ENVIRONMENT "PYTHONUTF8=1;PYTHONIOENCODING=utf-8")
endforeach()
add_test(NAME verification.vscode_workflow_coherence COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/scripts/validate-vscode-workflow.py")
set_tests_properties(verification.vscode_workflow_coherence PROPERTIES
  ENVIRONMENT "PYTHONUTF8=1;PYTHONIOENCODING=utf-8" LABELS "verification;toolchain;vscode")
set_tests_properties(verification.external PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)

if(WIN32)
  set(_environment_test_arguments -NoProfile -ExecutionPolicy Bypass -File
    "${CMAKE_CURRENT_SOURCE_DIR}/verify_common_environment.ps1" -RepositoryRoot "${CMAKE_SOURCE_DIR}")
  if(SIGNAL_STUDIO_BUILD_UI)
    list(APPEND _environment_test_arguments -RequireQt)
  endif()
  add_test(NAME verification.powershell_environment_idempotency COMMAND powershell.exe ${_environment_test_arguments})
  set_tests_properties(verification.powershell_environment_idempotency PROPERTIES
    TIMEOUT 60 LABELS "verification;toolchain;same-session")
  if(SIGNAL_STUDIO_BUILD_UI)
    set_property(TEST verification.powershell_environment_idempotency APPEND PROPERTY
      ENVIRONMENT "SIGNAL_STUDIO_QT_ROOT=${SIGNAL_STUDIO_QT_ROOT}")
  endif()
  add_test(NAME verification.windows_path_semantics COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
    "${CMAKE_CURRENT_SOURCE_DIR}/verify_windows_path_semantics.ps1" -RepositoryRoot "${CMAKE_SOURCE_DIR}")
  set_tests_properties(verification.windows_path_semantics PROPERTIES
    TIMEOUT 30 LABELS "verification;toolchain;paths")
  add_test(NAME verification.dependency_validation_modes COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
    "${CMAKE_CURRENT_SOURCE_DIR}/verify_dependency_validation_modes.ps1" -RepositoryRoot "${CMAKE_SOURCE_DIR}")
  set_tests_properties(verification.dependency_validation_modes PROPERTIES
    TIMEOUT 60 LABELS "verification;toolchain;dependencies")
endif()

if(SIGNAL_STUDIO_BUILD_UI)
  set(_consumer_source "${CMAKE_CURRENT_SOURCE_DIR}/consumer")
else()
  set(_consumer_source "${CMAKE_CURRENT_SOURCE_DIR}/consumer_headless")
endif()
add_test(NAME package.installed_component_consumer COMMAND "${CMAKE_COMMAND}"
  "-DSIGNAL_STUDIO_BUILD_DIR=${CMAKE_BINARY_DIR}" "-DSIGNAL_STUDIO_CONSUMER_SOURCE=${_consumer_source}"
  "-DSIGNAL_STUDIO_GENERATOR=${CMAKE_GENERATOR}" "-DSIGNAL_STUDIO_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
  "-DSIGNAL_STUDIO_QT_ROOT=${SIGNAL_STUDIO_QT_ROOT}" "-DSIGNAL_STUDIO_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/run_installed_consumer.cmake")
set_tests_properties(package.installed_component_consumer PROPERTIES TIMEOUT 180 LABELS "package;all-modules")

if(SIGNAL_STUDIO_BUILD_UI)
  add_test(NAME package.headless_no_qt_build_install_consumer COMMAND "${CMAKE_COMMAND}"
    "-DSIGNAL_STUDIO_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DSIGNAL_STUDIO_PARENT_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DSIGNAL_STUDIO_GENERATOR=${CMAKE_GENERATOR}" "-DSIGNAL_STUDIO_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    "-DSIGNAL_STUDIO_BUILD_TYPE=${CMAKE_BUILD_TYPE}" "-DSIGNAL_STUDIO_CONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/consumer_headless"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/run_headless_build_consumer.cmake")
  set_tests_properties(package.headless_no_qt_build_install_consumer PROPERTIES TIMEOUT 240 LABELS "package;headless;no-qt")
endif()
