set(_root "${SIGNAL_STUDIO_PARENT_BUILD_DIR}/headless-no-qt-test")
set(_build "${_root}/build")
set(_prefix "${_root}/prefix")
set(_consumer_build "${_root}/consumer")
file(REMOVE_RECURSE "${_root}")

execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=SIGNAL_STUDIO_QT_ROOT --unset=Qt6_DIR
  "PATH=${SIGNAL_STUDIO_TOOLCHAIN_PATH}" "INCLUDE=${SIGNAL_STUDIO_TOOLCHAIN_INCLUDE}"
  "LIB=${SIGNAL_STUDIO_TOOLCHAIN_LIB}" "LIBPATH=${SIGNAL_STUDIO_TOOLCHAIN_LIBPATH}"
  "VCToolsRedistDir=${SIGNAL_STUDIO_VCTOOLS_REDIST_DIR}"
  "UniversalCRTSdkDir=${SIGNAL_STUDIO_UNIVERSAL_CRT_SDK_DIR}" "UCRTVersion=${SIGNAL_STUDIO_UCRT_VERSION}"
  "${CMAKE_COMMAND}" -S "${SIGNAL_STUDIO_SOURCE_DIR}" -B "${_build}" -G "${SIGNAL_STUDIO_GENERATOR}"
  "-DCMAKE_MAKE_PROGRAM=${SIGNAL_STUDIO_MAKE_PROGRAM}" "-DCMAKE_BUILD_TYPE=${SIGNAL_STUDIO_BUILD_TYPE}"
  "-DCMAKE_C_COMPILER=${SIGNAL_STUDIO_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${SIGNAL_STUDIO_CXX_COMPILER}"
  "-DCMAKE_LINKER=${SIGNAL_STUDIO_LINKER}" "-DCMAKE_RC_COMPILER=${SIGNAL_STUDIO_RC_COMPILER}"
  "-DCMAKE_MT=${SIGNAL_STUDIO_MT}"
  -DSIGNAL_STUDIO_BUILD_UI=OFF -DSIGNAL_STUDIO_BUILD_TESTS=OFF -DSIGNAL_STUDIO_BUILD_SDK_EXAMPLE=OFF -DBUILD_TESTING=OFF
  RESULT_VARIABLE _configure OUTPUT_VARIABLE _configure_out ERROR_VARIABLE _configure_err)
if(NOT _configure EQUAL 0)
  message(FATAL_ERROR "No-Qt configure failed:\n${_configure_out}\n${_configure_err}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
  "PATH=${SIGNAL_STUDIO_TOOLCHAIN_PATH}" "INCLUDE=${SIGNAL_STUDIO_TOOLCHAIN_INCLUDE}"
  "LIB=${SIGNAL_STUDIO_TOOLCHAIN_LIB}" "LIBPATH=${SIGNAL_STUDIO_TOOLCHAIN_LIBPATH}"
  "VCToolsRedistDir=${SIGNAL_STUDIO_VCTOOLS_REDIST_DIR}"
  "UniversalCRTSdkDir=${SIGNAL_STUDIO_UNIVERSAL_CRT_SDK_DIR}" "UCRTVersion=${SIGNAL_STUDIO_UCRT_VERSION}"
  "${CMAKE_COMMAND}" --build "${_build}" RESULT_VARIABLE _build_result OUTPUT_VARIABLE _build_out ERROR_VARIABLE _build_err)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR "No-Qt build failed:\n${_build_out}\n${_build_err}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${_build}" --prefix "${_prefix}" RESULT_VARIABLE _install OUTPUT_VARIABLE _install_out ERROR_VARIABLE _install_err)
if(NOT _install EQUAL 0)
  message(FATAL_ERROR "No-Qt install failed:\n${_install_out}\n${_install_err}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=SIGNAL_STUDIO_QT_ROOT --unset=Qt6_DIR
  "PATH=${SIGNAL_STUDIO_TOOLCHAIN_PATH}" "INCLUDE=${SIGNAL_STUDIO_TOOLCHAIN_INCLUDE}"
  "LIB=${SIGNAL_STUDIO_TOOLCHAIN_LIB}" "LIBPATH=${SIGNAL_STUDIO_TOOLCHAIN_LIBPATH}"
  "${CMAKE_COMMAND}" -S "${SIGNAL_STUDIO_CONSUMER_SOURCE}" -B "${_consumer_build}" -G "${SIGNAL_STUDIO_GENERATOR}"
  "-DCMAKE_MAKE_PROGRAM=${SIGNAL_STUDIO_MAKE_PROGRAM}" "-DCMAKE_BUILD_TYPE=${SIGNAL_STUDIO_BUILD_TYPE}"
  "-DCMAKE_C_COMPILER=${SIGNAL_STUDIO_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${SIGNAL_STUDIO_CXX_COMPILER}"
  "-DCMAKE_LINKER=${SIGNAL_STUDIO_LINKER}" "-DCMAKE_RC_COMPILER=${SIGNAL_STUDIO_RC_COMPILER}"
  "-DCMAKE_MT=${SIGNAL_STUDIO_MT}"
  "-DCMAKE_PREFIX_PATH=${_prefix}"
  RESULT_VARIABLE _consumer_configure OUTPUT_VARIABLE _consumer_configure_out ERROR_VARIABLE _consumer_configure_err)
if(NOT _consumer_configure EQUAL 0)
  message(FATAL_ERROR "No-Qt consumer configure failed:\n${_consumer_configure_out}\n${_consumer_configure_err}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
  "PATH=${SIGNAL_STUDIO_TOOLCHAIN_PATH}" "INCLUDE=${SIGNAL_STUDIO_TOOLCHAIN_INCLUDE}"
  "LIB=${SIGNAL_STUDIO_TOOLCHAIN_LIB}" "LIBPATH=${SIGNAL_STUDIO_TOOLCHAIN_LIBPATH}"
  "${CMAKE_COMMAND}" --build "${_consumer_build}" RESULT_VARIABLE _consumer_build_result OUTPUT_VARIABLE _consumer_build_out ERROR_VARIABLE _consumer_build_err)
if(NOT _consumer_build_result EQUAL 0)
  message(FATAL_ERROR "No-Qt consumer build failed:\n${_consumer_build_out}\n${_consumer_build_err}")
endif()
if(CMAKE_HOST_WIN32)
  file(GLOB _test_runtime_dlls "${SIGNAL_STUDIO_MSVC_TEST_RUNTIME_DIR}/*.dll")
  if(SIGNAL_STUDIO_BUILD_TYPE STREQUAL "Debug")
    list(APPEND _test_runtime_dlls "${SIGNAL_STUDIO_DEBUG_UCRT}")
    foreach(_debug_runtime IN LISTS _test_runtime_dlls)
      get_filename_component(_debug_runtime_name "${_debug_runtime}" NAME)
      if(EXISTS "${_prefix}/bin/${_debug_runtime_name}")
        message(FATAL_ERROR "No-Qt Debug install leaked a non-redistributable runtime: ${_debug_runtime_name}")
      endif()
    endforeach()
    if(NOT _test_runtime_dlls)
      message(FATAL_ERROR "No-Qt Debug consumer test environment is missing the matching runtime")
    endif()
    file(COPY ${_test_runtime_dlls} DESTINATION "${_consumer_build}")
    file(GLOB _installed_runtime_dlls "${_prefix}/bin/*.dll")
    file(COPY ${_installed_runtime_dlls} DESTINATION "${_consumer_build}")
  else()
    if(NOT _test_runtime_dlls)
      message(FATAL_ERROR "No-Qt Release consumer test environment is missing the matching redistributable runtime")
    endif()
    foreach(_release_runtime IN LISTS _test_runtime_dlls)
      get_filename_component(_release_runtime_name "${_release_runtime}" NAME)
      if(NOT EXISTS "${_prefix}/bin/${_release_runtime_name}")
        message(FATAL_ERROR "No-Qt Release install is missing the matching runtime: ${_release_runtime_name}")
      endif()
    endforeach()
    file(GLOB _installed_runtime_dlls "${_prefix}/bin/*.dll")
    file(COPY ${_installed_runtime_dlls} DESTINATION "${_consumer_build}")
  endif()
endif()
if(CMAKE_HOST_WIN32)
  set(_exe_suffix ".exe")
else()
  set(_exe_suffix "")
endif()
execute_process(COMMAND "${_consumer_build}/signal_studio_consumer_smoke${_exe_suffix}" RESULT_VARIABLE _run OUTPUT_VARIABLE _run_out ERROR_VARIABLE _run_err)
if(NOT _run EQUAL 0)
  message(FATAL_ERROR "No-Qt consumer run failed:\n${_run_out}\n${_run_err}")
endif()
message(STATUS "No-Qt headless configure/build/install/consumer passed: ${_run_out}")
