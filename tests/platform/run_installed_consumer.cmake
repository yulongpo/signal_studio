if(NOT IS_DIRECTORY "${SIGNAL_STUDIO_BUILD_DIR}")
  message(FATAL_ERROR "Signal Studio build directory does not exist")
endif()

set(_root "${SIGNAL_STUDIO_BUILD_DIR}/installed-consumer-test")
set(_prefix "${_root}/prefix")
set(_build "${_root}/build")
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_root}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${SIGNAL_STUDIO_BUILD_DIR}" --prefix "${_prefix}"
  RESULT_VARIABLE _install_result
  OUTPUT_VARIABLE _install_output
  ERROR_VARIABLE _install_error
)
if(NOT _install_result EQUAL 0)
  message(FATAL_ERROR "Install failed:\n${_install_output}\n${_install_error}")
endif()

execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "PATH=${SIGNAL_STUDIO_TOOLCHAIN_PATH}"
    "INCLUDE=${SIGNAL_STUDIO_TOOLCHAIN_INCLUDE}"
    "LIB=${SIGNAL_STUDIO_TOOLCHAIN_LIB}"
    "LIBPATH=${SIGNAL_STUDIO_TOOLCHAIN_LIBPATH}"
    "${CMAKE_COMMAND}"
    -S "${SIGNAL_STUDIO_CONSUMER_SOURCE}"
    -B "${_build}"
    -G "${SIGNAL_STUDIO_GENERATOR}"
    "-DCMAKE_BUILD_TYPE=${SIGNAL_STUDIO_BUILD_TYPE}"
    "-DCMAKE_MAKE_PROGRAM=${SIGNAL_STUDIO_MAKE_PROGRAM}"
    "-DCMAKE_C_COMPILER=${SIGNAL_STUDIO_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${SIGNAL_STUDIO_CXX_COMPILER}"
    "-DCMAKE_LINKER=${SIGNAL_STUDIO_LINKER}"
    "-DCMAKE_RC_COMPILER=${SIGNAL_STUDIO_RC_COMPILER}"
    "-DCMAKE_MT=${SIGNAL_STUDIO_MT}"
    "-DCMAKE_PREFIX_PATH=${_prefix};${SIGNAL_STUDIO_QT_ROOT}"
  RESULT_VARIABLE _configure_result
  OUTPUT_VARIABLE _configure_output
  ERROR_VARIABLE _configure_error
)
if(NOT _configure_result EQUAL 0)
  message(FATAL_ERROR "Consumer configure failed:\n${_configure_output}\n${_configure_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "PATH=${SIGNAL_STUDIO_TOOLCHAIN_PATH}"
    "INCLUDE=${SIGNAL_STUDIO_TOOLCHAIN_INCLUDE}"
    "LIB=${SIGNAL_STUDIO_TOOLCHAIN_LIB}"
    "LIBPATH=${SIGNAL_STUDIO_TOOLCHAIN_LIBPATH}"
    "${CMAKE_COMMAND}" --build "${_build}"
  RESULT_VARIABLE _build_result
  OUTPUT_VARIABLE _build_output
  ERROR_VARIABLE _build_error
)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR "Consumer build failed:\n${_build_output}\n${_build_error}")
endif()

if(CMAKE_HOST_WIN32)
  file(GLOB _test_runtime_dlls "${SIGNAL_STUDIO_MSVC_TEST_RUNTIME_DIR}/*.dll")
  if(SIGNAL_STUDIO_BUILD_TYPE STREQUAL "Debug")
    list(APPEND _test_runtime_dlls "${SIGNAL_STUDIO_DEBUG_UCRT}")
    foreach(_debug_runtime IN LISTS _test_runtime_dlls)
      get_filename_component(_debug_runtime_name "${_debug_runtime}" NAME)
      if(EXISTS "${_prefix}/bin/${_debug_runtime_name}")
        message(FATAL_ERROR "Debug non-redistributable runtime leaked into the formal install: ${_debug_runtime_name}")
      endif()
    endforeach()
    if(NOT _test_runtime_dlls)
      message(FATAL_ERROR "Debug consumer test environment is missing the matching non-redistributable runtime")
    endif()
    file(COPY ${_test_runtime_dlls} DESTINATION "${_build}")
    file(GLOB _installed_runtime_dlls "${_prefix}/bin/*.dll")
    file(COPY ${_installed_runtime_dlls} DESTINATION "${_build}")
  else()
    if(NOT _test_runtime_dlls)
      message(FATAL_ERROR "Release consumer test environment is missing the matching redistributable runtime")
    endif()
    foreach(_release_runtime IN LISTS _test_runtime_dlls)
      get_filename_component(_release_runtime_name "${_release_runtime}" NAME)
      if(NOT EXISTS "${_prefix}/bin/${_release_runtime_name}")
        message(FATAL_ERROR "Release install is missing the matching redistributable runtime: ${_release_runtime_name}")
      endif()
    endforeach()
    file(GLOB _installed_runtime_dlls "${_prefix}/bin/*.dll")
    file(COPY ${_installed_runtime_dlls} DESTINATION "${_build}")
  endif()
endif()

if(CMAKE_HOST_WIN32)
  set(_exe_suffix ".exe")
else()
  set(_exe_suffix "")
endif()
set(_consumer_runtime_path "$ENV{PATH}")
if(CMAKE_HOST_WIN32 AND IS_DIRECTORY "${SIGNAL_STUDIO_QT_ROOT}/bin")
  set(_consumer_runtime_path "${SIGNAL_STUDIO_QT_ROOT}/bin;${_consumer_runtime_path}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "PATH=${_consumer_runtime_path}"
    "${_build}/signal_studio_consumer_smoke${_exe_suffix}"
  RESULT_VARIABLE _run_result
  OUTPUT_VARIABLE _run_output
  ERROR_VARIABLE _run_error
)
if(NOT _run_result EQUAL 0)
  message(FATAL_ERROR "Consumer run failed with ${_run_result}:\n${_run_output}\n${_run_error}")
endif()

set(_installed_demo "${_prefix}/bin/signal_visualization_workbench_demo${_exe_suffix}")
if(SIGNAL_STUDIO_EXPECT_UI AND NOT EXISTS "${_installed_demo}")
  message(FATAL_ERROR "UI install is missing the Visualization/Workbench demo: ${_installed_demo}")
endif()
set(_installed_application "${_prefix}/bin/SignalStudio${_exe_suffix}")
if(SIGNAL_STUDIO_EXPECT_UI AND NOT EXISTS "${_installed_application}")
  message(FATAL_ERROR "UI install is missing the Signal Studio application: ${_installed_application}")
endif()
if(SIGNAL_STUDIO_EXPECT_UI)
  set(_demo_runtime_path "${_prefix}/bin;${SIGNAL_STUDIO_TOOLCHAIN_PATH}")
  if(SIGNAL_STUDIO_BUILD_TYPE STREQUAL "Debug")
    get_filename_component(_debug_ucrt_dir "${SIGNAL_STUDIO_DEBUG_UCRT}" DIRECTORY)
    set(_demo_runtime_path
      "${_prefix}/bin;${SIGNAL_STUDIO_MSVC_TEST_RUNTIME_DIR};${_debug_ucrt_dir};${SIGNAL_STUDIO_TOOLCHAIN_PATH}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
      --unset=QT_QPA_PLATFORM
      --unset=QT_PLUGIN_PATH
      --unset=QT_QPA_PLATFORM_PLUGIN_PATH
      "PATH=${_demo_runtime_path}"
      "${_installed_demo}" --startup-smoke
    RESULT_VARIABLE _demo_result
    OUTPUT_VARIABLE _demo_output
    ERROR_VARIABLE _demo_error
  )
  if(NOT _demo_result EQUAL 0)
    message(FATAL_ERROR
      "Installed Qt demo startup failed with ${_demo_result}:\n${_demo_output}\n${_demo_error}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
      --unset=QT_QPA_PLATFORM
      --unset=QT_PLUGIN_PATH
      --unset=QT_QPA_PLATFORM_PLUGIN_PATH
      "PATH=${_demo_runtime_path}"
      "${_installed_application}" --self-test --scratch "${_root}/s"
    RESULT_VARIABLE _application_self_test_result
    OUTPUT_VARIABLE _application_self_test_output
    ERROR_VARIABLE _application_self_test_error
  )
  if(NOT _application_self_test_result EQUAL 0)
    message(FATAL_ERROR
      "Installed Signal Studio self-test failed with ${_application_self_test_result}:\n"
      "${_application_self_test_output}\n${_application_self_test_error}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
      --unset=QT_QPA_PLATFORM
      --unset=QT_PLUGIN_PATH
      --unset=QT_QPA_PLATFORM_PLUGIN_PATH
      "PATH=${_demo_runtime_path}"
      "${_installed_application}" --startup-smoke
        --state-dir "${_root}/state"
    RESULT_VARIABLE _application_startup_result
    OUTPUT_VARIABLE _application_startup_output
    ERROR_VARIABLE _application_startup_error
  )
  if(NOT _application_startup_result EQUAL 0)
    message(FATAL_ERROR
      "Installed Signal Studio startup failed with ${_application_startup_result}:\n"
      "${_application_startup_output}\n${_application_startup_error}")
  endif()
endif()
message(STATUS "Installed package consumer passed: ${_run_output}")
