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
    "${CMAKE_COMMAND}"
    -S "${SIGNAL_STUDIO_CONSUMER_SOURCE}"
    -B "${_build}"
    -G "${SIGNAL_STUDIO_GENERATOR}"
    "-DCMAKE_BUILD_TYPE=${SIGNAL_STUDIO_BUILD_TYPE}"
    "-DCMAKE_MAKE_PROGRAM=${SIGNAL_STUDIO_MAKE_PROGRAM}"
    "-DCMAKE_PREFIX_PATH=${_prefix};${SIGNAL_STUDIO_QT_ROOT}"
  RESULT_VARIABLE _configure_result
  OUTPUT_VARIABLE _configure_output
  ERROR_VARIABLE _configure_error
)
if(NOT _configure_result EQUAL 0)
  message(FATAL_ERROR "Consumer configure failed:\n${_configure_output}\n${_configure_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_build}"
  RESULT_VARIABLE _build_result
  OUTPUT_VARIABLE _build_output
  ERROR_VARIABLE _build_error
)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR "Consumer build failed:\n${_build_output}\n${_build_error}")
endif()

if(CMAKE_HOST_WIN32)
  set(_exe_suffix ".exe")
else()
  set(_exe_suffix "")
endif()
execute_process(
  COMMAND "${_build}/signal_studio_consumer_smoke${_exe_suffix}"
  RESULT_VARIABLE _run_result
  OUTPUT_VARIABLE _run_output
  ERROR_VARIABLE _run_error
)
if(NOT _run_result EQUAL 0)
  message(FATAL_ERROR "Consumer run failed:\n${_run_output}\n${_run_error}")
endif()
message(STATUS "Installed package consumer passed: ${_run_output}")
