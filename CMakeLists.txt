cmake_minimum_required(VERSION 3.28)

project(SignalStudio VERSION 1.0.0 DESCRIPTION "Signal Processing Platform" LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DEBUG_POSTFIX "d")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(SIGNAL_STUDIO_BUILD_UI "Build Qt-backed Visualization and Workbench components" ON)
option(SIGNAL_STUDIO_BUILD_TESTS "Build Signal Studio verification tests" ${BUILD_TESTING})
option(SIGNAL_STUDIO_BUILD_SDK_EXAMPLE "Build the C ABI SDK example" ${BUILD_TESTING})
set(SIGNAL_STUDIO_QT_ROOT "$ENV{SIGNAL_STUDIO_QT_ROOT}" CACHE PATH "Qt installation prefix for UI builds")
set(SIGNAL_STUDIO_MINIMUM_QT_VERSION "6.10.3")
set(SIGNAL_STUDIO_CUDA_MODE "AUTO" CACHE STRING "CUDA request: AUTO, ON, or OFF")
set_property(CACHE SIGNAL_STUDIO_CUDA_MODE PROPERTY STRINGS AUTO ON OFF)
set(SIGNAL_STUDIO_ONEMKL_MODE "AUTO" CACHE STRING "oneMKL request: AUTO, ON, or OFF")
set_property(CACHE SIGNAL_STUDIO_ONEMKL_MODE PROPERTY STRINGS AUTO ON OFF)
set(SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../test_data"
  CACHE PATH "Optional approved external recording directory")

set(SIGNAL_STUDIO_EXTERNAL_TEST_DATA_AVAILABLE OFF)
if(EXISTS "${SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR}/20241110-174401-662_bw_12800000_sampleTime_0.4_rollOff_0.3.wav"
   AND EXISTS "${SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR}/x310_capture_cf1425MHz_sr50MSps_20260521_144220.sc16"
   AND EXISTS "${SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR}/x310_capture_cf1245MHz_sr50MSps_20260521_144927.sc16")
  set(SIGNAL_STUDIO_EXTERNAL_TEST_DATA_AVAILABLE ON)
endif()
set(SIGNAL_STUDIO_MS02_PERF_RECORDING
  "${SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR}/x310_capture_cf1425MHz_sr50MSps_20260521_144220.sc16")
set(SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256
  "74d0ace877568a1c26505c773f249a02e411546254e67c63c4613bb508f2d605")
set(SIGNAL_STUDIO_MS02_PERF_RECORDING_BYTES 4004031888)
set(SIGNAL_STUDIO_MS02_PERF_RECORDING_AVAILABLE OFF)
if(EXISTS "${SIGNAL_STUDIO_MS02_PERF_RECORDING}")
  file(SIZE "${SIGNAL_STUDIO_MS02_PERF_RECORDING}" _signal_studio_ms02_recording_bytes)
  if(NOT _signal_studio_ms02_recording_bytes EQUAL SIGNAL_STUDIO_MS02_PERF_RECORDING_BYTES)
    message(FATAL_ERROR
      "MS-02 X310 录制大小不匹配：期望 ${SIGNAL_STUDIO_MS02_PERF_RECORDING_BYTES}，实际 ${_signal_studio_ms02_recording_bytes}")
  endif()
  file(SHA256 "${SIGNAL_STUDIO_MS02_PERF_RECORDING}" _signal_studio_ms02_recording_sha256)
  if(NOT _signal_studio_ms02_recording_sha256 STREQUAL SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256)
    message(FATAL_ERROR
      "MS-02 X310 录制 SHA-256 不匹配：期望 ${SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256}，实际 ${_signal_studio_ms02_recording_sha256}")
  endif()
  set(SIGNAL_STUDIO_MS02_PERF_RECORDING_AVAILABLE ON)
endif()

set(SIGNAL_STUDIO_MSVC_TEST_RUNTIME_FILES)
set(SIGNAL_STUDIO_MSVC_INSTALL_RUNTIME_FILES)
set(SIGNAL_STUDIO_MSVC_TEST_RUNTIME_DIR)
set(SIGNAL_STUDIO_DEBUG_UCRT)
if(MSVC AND WIN32)
  if(NOT DEFINED ENV{VCToolsRedistDir})
    message(FATAL_ERROR "缺少 VCToolsRedistDir，无法部署与当前 MSVC 工具集匹配的 VC 运行库")
  endif()
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}/debug_nonredist/x64/Microsoft.VC143.DebugCRT"
      SIGNAL_STUDIO_MSVC_TEST_RUNTIME_DIR)
  else()
    file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}/x64/Microsoft.VC143.CRT" SIGNAL_STUDIO_MSVC_TEST_RUNTIME_DIR)
  endif()
  file(GLOB SIGNAL_STUDIO_MSVC_TEST_RUNTIME_FILES CONFIGURE_DEPENDS
    "${SIGNAL_STUDIO_MSVC_TEST_RUNTIME_DIR}/*.dll")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(NOT DEFINED ENV{UniversalCRTSdkDir} OR NOT DEFINED ENV{UCRTVersion})
      message(FATAL_ERROR "缺少 UniversalCRTSdkDir/UCRTVersion，无法部署 Debug UCRT")
    endif()
    file(TO_CMAKE_PATH "$ENV{UniversalCRTSdkDir}/bin/$ENV{UCRTVersion}/x64/ucrt/ucrtbased.dll"
      SIGNAL_STUDIO_DEBUG_UCRT)
    if(NOT EXISTS "${SIGNAL_STUDIO_DEBUG_UCRT}")
      message(FATAL_ERROR "未找到与 Windows SDK 匹配的 Debug UCRT：${SIGNAL_STUDIO_DEBUG_UCRT}")
    endif()
    list(APPEND SIGNAL_STUDIO_MSVC_TEST_RUNTIME_FILES "${SIGNAL_STUDIO_DEBUG_UCRT}")
  else()
    set(SIGNAL_STUDIO_MSVC_INSTALL_RUNTIME_FILES ${SIGNAL_STUDIO_MSVC_TEST_RUNTIME_FILES})
  endif()
  if(NOT SIGNAL_STUDIO_MSVC_TEST_RUNTIME_FILES)
    message(FATAL_ERROR "未找到与构建类型匹配的 x64 VC143 运行库集：${SIGNAL_STUDIO_MSVC_TEST_RUNTIME_DIR}")
  endif()
endif()

function(signal_studio_deploy_msvc_runtime target)
  if(SIGNAL_STUDIO_MSVC_TEST_RUNTIME_FILES)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${SIGNAL_STUDIO_MSVC_TEST_RUNTIME_FILES} "$<TARGET_FILE_DIR:${target}>"
      COMMAND_EXPAND_LISTS
      COMMENT "部署仅用于测试运行的匹配 VC143 运行库")
  endif()
endfunction()

string(TOUPPER "${SIGNAL_STUDIO_CUDA_MODE}" _signal_studio_cuda_mode)
if(NOT _signal_studio_cuda_mode MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "SIGNAL_STUDIO_CUDA_MODE must be AUTO, ON, or OFF")
endif()

string(TOUPPER "${SIGNAL_STUDIO_ONEMKL_MODE}" _signal_studio_onemkl_mode)
if(NOT _signal_studio_onemkl_mode MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "SIGNAL_STUDIO_ONEMKL_MODE must be AUTO, ON, or OFF")
endif()

set(_signal_studio_local_dependency_prefix
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/vcpkg_installed/x64-windows")
if(EXISTS "${_signal_studio_local_dependency_prefix}")
  list(PREPEND CMAKE_PREFIX_PATH "${_signal_studio_local_dependency_prefix}")
endif()

find_package(SampleRate CONFIG QUIET)
if(NOT TARGET SampleRate::samplerate)
  message(FATAL_ERROR
    "MS-02 requires pinned libsamplerate. Run scripts/install-ms02-dependencies.ps1.")
endif()

set(SIGNAL_STUDIO_ONEMKL_AVAILABLE OFF)
if(NOT _signal_studio_onemkl_mode STREQUAL "OFF")
  set(MKL_LINK dynamic)
  set(MKL_INTERFACE lp64)
  set(MKL_THREADING sequential)
  find_package(MKL CONFIG QUIET)
  if(MKL_FOUND AND TARGET MKL::MKL)
    set(SIGNAL_STUDIO_ONEMKL_AVAILABLE ON)
  elseif(_signal_studio_onemkl_mode STREQUAL "ON")
    message(FATAL_ERROR
      "oneMKL was requested but not found. Run scripts/install-ms02-dependencies.ps1.")
  endif()
endif()
if(BUILD_TESTING AND SIGNAL_STUDIO_BUILD_TESTS AND NOT SIGNAL_STUDIO_ONEMKL_AVAILABLE)
  message(FATAL_ERROR
    "The MS-02 test suite requires the pinned oneMKL backend. Run scripts/install-ms02-dependencies.ps1.")
endif()

set(SIGNAL_STUDIO_CUDA_AVAILABLE OFF)
if(NOT _signal_studio_cuda_mode STREQUAL "OFF")
  find_package(CUDAToolkit 12.4.131 EXACT QUIET)
  if(CUDAToolkit_FOUND AND TARGET CUDA::cudart AND TARGET CUDA::cufft)
    set(_signal_studio_cuda_runtime_files
      "${CUDAToolkit_BIN_DIR}/cudart64_12.dll"
      "${CUDAToolkit_BIN_DIR}/cufft64_11.dll")
    get_filename_component(_signal_studio_cuda_root "${CUDAToolkit_BIN_DIR}" DIRECTORY)
    if(EXISTS "${CUDAToolkit_BIN_DIR}/cudart64_12.dll"
       AND EXISTS "${CUDAToolkit_BIN_DIR}/cufft64_11.dll")
      set(SIGNAL_STUDIO_CUDA_AVAILABLE ON)
    endif()
  endif()
  if(_signal_studio_cuda_mode STREQUAL "ON" AND NOT SIGNAL_STUDIO_CUDA_AVAILABLE)
    message(FATAL_ERROR
      "CUDA 12.4.131 with cudart/cuFFT was requested but not found. Use AUTO for an explicit CPU fallback.")
  endif()
endif()

if(SIGNAL_STUDIO_BUILD_UI)
  if(SIGNAL_STUDIO_QT_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${SIGNAL_STUDIO_QT_ROOT}")
  endif()
  find_package(Qt6 ${SIGNAL_STUDIO_MINIMUM_QT_VERSION} REQUIRED COMPONENTS Core Widgets)

  function(signal_studio_deploy_qt_runtime target)
    if(WIN32)
      if(NOT TARGET Qt6::QOffscreenIntegrationPlugin OR NOT TARGET Qt6::QWindowsIntegrationPlugin)
        message(FATAL_ERROR "${target} 需要 Qt offscreen/windows 平台插件导入目标")
      endif()
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          $<TARGET_RUNTIME_DLLS:${target}> "$<TARGET_FILE_DIR:${target}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cmake/qt.conf"
          "$<TARGET_FILE_DIR:${target}>/qt.conf"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
          "$<TARGET_FILE_DIR:${target}>/platforms"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
          "$<TARGET_FILE_DIR:${target}>/platforms/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
          "$<TARGET_FILE_DIR:${target}>/platforms/"
        COMMAND_EXPAND_LISTS
        COMMENT "部署 ${target} 的 Qt 运行时与 Windows/offscreen 平台插件")
    endif()
  endfunction()
endif()

execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  OUTPUT_VARIABLE SIGNAL_STUDIO_GIT_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT SIGNAL_STUDIO_GIT_REVISION)
  set(SIGNAL_STUDIO_GIT_REVISION "unknown")
endif()
string(TIMESTAMP SIGNAL_STUDIO_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)
configure_file(cmake/build_config.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/signal_studio/core/build_config.hpp" @ONLY)

function(signal_studio_add_module target export_name)
  add_library(${target} STATIC ${ARGN})
  add_library(SignalStudio::${export_name} ALIAS ${target})
  set_target_properties(${target} PROPERTIES EXPORT_NAME ${export_name})
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  signal_studio_enable_warnings(${target})
endfunction()

function(signal_studio_add_shared_module target export_name)
  add_library(${target} SHARED ${ARGN})
  add_library(SignalStudio::${export_name} ALIAS ${target})
  set_target_properties(${target} PROPERTIES
    EXPORT_NAME ${export_name}
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  signal_studio_enable_warnings(${target})
endfunction()

signal_studio_add_module(SignalStudioCore Core src/platform/core/version.cpp src/platform/core/error.cpp
  src/platform/core/capability.cpp src/platform/core/module_descriptor.cpp src/platform/core/module.cpp
  src/platform/core/services.cpp src/platform/core/artifact.cpp)
target_include_directories(SignalStudioCore PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
signal_studio_add_shared_module(SignalStudioCompute Compute src/platform/compute/module.cpp
  src/platform/compute/compute.cpp src/platform/compute/cuda_memory.cpp)
target_link_libraries(SignalStudioCompute PUBLIC SignalStudio::Core)
if(SIGNAL_STUDIO_CUDA_AVAILABLE)
  target_compile_definitions(SignalStudioCompute PRIVATE SIGNAL_STUDIO_HAVE_CUDA_MEMORY=1)
  target_link_libraries(SignalStudioCompute PRIVATE CUDA::cudart)
  add_custom_command(TARGET SignalStudioCompute POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      ${_signal_studio_cuda_runtime_files} "$<TARGET_FILE_DIR:SignalStudioCompute>"
    COMMAND_EXPAND_LISTS
    COMMENT "部署 CUDA cudart/cuFFT 精确运行时闭包到测试目录")
endif()
signal_studio_add_module(SignalStudioData Data src/platform/data/module.cpp src/platform/data/signal.cpp
  src/platform/data/raw.cpp src/platform/data/io.cpp src/platform/data/loading.cpp
  src/platform/data/preview.cpp src/platform/data/index.cpp)
target_link_libraries(SignalStudioData PUBLIC SignalStudio::Core)
signal_studio_add_module(SignalStudioTaskRuntime TaskRuntime src/platform/task_runtime/module.cpp
  src/platform/task_runtime/task_runtime.cpp)
target_link_libraries(SignalStudioTaskRuntime PUBLIC SignalStudio::Compute SignalStudio::Core)
signal_studio_add_shared_module(SignalStudioDSP DSP src/platform/dsp/module.cpp src/platform/dsp/analysis.cpp
  src/platform/dsp/pipeline.cpp src/platform/dsp/browse_performance.cpp src/platform/dsp/mkl_adapter.cpp
  src/platform/dsp/cuda_fft_adapter.cpp src/platform/dsp/fft_backend_factory.cpp
  src/platform/dsp/signal_compute_adapter.cpp)
target_link_libraries(SignalStudioDSP PUBLIC SignalStudio::Data SignalStudio::Compute SignalStudio::Core)
target_link_libraries(SignalStudioDSP PRIVATE SampleRate::samplerate)
add_custom_command(TARGET SignalStudioDSP POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:SampleRate::samplerate>" "$<TARGET_FILE_DIR:SignalStudioDSP>"
  COMMENT "部署 libsamplerate 精确运行时到测试目录")
if(SIGNAL_STUDIO_ONEMKL_AVAILABLE)
  target_compile_definitions(SignalStudioDSP PRIVATE SIGNAL_STUDIO_HAVE_ONEMKL=1)
  target_link_libraries(SignalStudioDSP PRIVATE MKL::MKL)
  file(GLOB _signal_studio_mkl_runtime_files CONFIGURE_DEPENDS
    "${_signal_studio_local_dependency_prefix}/bin/mkl_core*.dll"
    "${_signal_studio_local_dependency_prefix}/bin/mkl_sequential*.dll"
    "${_signal_studio_local_dependency_prefix}/bin/mkl_def*.dll"
    "${_signal_studio_local_dependency_prefix}/bin/mkl_avx2*.dll"
    "${_signal_studio_local_dependency_prefix}/bin/mkl_avx512*.dll"
    "${_signal_studio_local_dependency_prefix}/bin/mkl_mc3*.dll"
    "${_signal_studio_local_dependency_prefix}/bin/mkl_vml_def*.dll"
    "${_signal_studio_local_dependency_prefix}/bin/mkl_vml_avx2*.dll"
    "${_signal_studio_local_dependency_prefix}/bin/mkl_vml_avx512*.dll"
    "${_signal_studio_local_dependency_prefix}/bin/mkl_vml_mc3*.dll"
    "${_signal_studio_local_dependency_prefix}/bin/mkl_vml_cmpt*.dll")
  if(_signal_studio_mkl_runtime_files)
    add_custom_command(TARGET SignalStudioDSP POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${_signal_studio_mkl_runtime_files} "$<TARGET_FILE_DIR:SignalStudioDSP>"
      COMMAND_EXPAND_LISTS
      COMMENT "部署 oneMKL 动态运行时到测试目录")
  endif()
endif()
if(SIGNAL_STUDIO_CUDA_AVAILABLE)
  target_compile_definitions(SignalStudioDSP PRIVATE SIGNAL_STUDIO_HAVE_CUFFT=1)
  target_link_libraries(SignalStudioDSP PRIVATE CUDA::cudart CUDA::cufft)
  add_custom_command(TARGET SignalStudioDSP POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      ${_signal_studio_cuda_runtime_files} "$<TARGET_FILE_DIR:SignalStudioDSP>"
    COMMAND_EXPAND_LISTS
    COMMENT "部署 CUDA cudart/cuFFT 精确运行时闭包到测试目录")
endif()
signal_studio_add_module(SignalStudioModelRuntime ModelRuntime src/platform/model_runtime/module.cpp)
target_link_libraries(SignalStudioModelRuntime PUBLIC SignalStudio::Data SignalStudio::Compute SignalStudio::TaskRuntime SignalStudio::Core)
signal_studio_add_module(SignalStudioDataset Dataset src/platform/dataset/module.cpp)
target_link_libraries(SignalStudioDataset PUBLIC SignalStudio::Data SignalStudio::TaskRuntime SignalStudio::Core)
signal_studio_add_module(SignalStudioPluginSDK PluginSDK src/platform/plugin_sdk/module.cpp src/platform/plugin_sdk/plugin_abi_v1.cpp)
target_link_libraries(SignalStudioPluginSDK PUBLIC SignalStudio::Data SignalStudio::TaskRuntime SignalStudio::Core)

set(_signal_studio_headless_targets SignalStudioCore SignalStudioData SignalStudioDSP SignalStudioCompute
  SignalStudioTaskRuntime SignalStudioPluginSDK SignalStudioModelRuntime SignalStudioDataset)
set(_signal_studio_ui_targets)
if(SIGNAL_STUDIO_BUILD_UI)
  signal_studio_add_module(SignalStudioVisualization Visualization
    src/platform/visualization/module.cpp
    src/platform/visualization/visualization.cpp
    src/platform/visualization/qt_visualization.cpp)
  target_link_libraries(SignalStudioVisualization PUBLIC SignalStudio::Data SignalStudio::TaskRuntime SignalStudio::Core
    PRIVATE Qt6::Core Qt6::Widgets)
  signal_studio_add_module(SignalStudioWorkbench Workbench
    src/platform/workbench/module.cpp
    src/platform/workbench/workbench.cpp
    src/platform/workbench/inspector.cpp
    src/platform/workbench/qt_workbench.cpp)
  qt_wrap_ui(_signal_studio_workbench_ui_headers
    src/platform/workbench/ui/SignalWorkbenchMainWindow.ui
    src/platform/workbench/ui/SignalInspectorPanel.ui
    src/platform/workbench/ui/SignalTaskCenterPanel.ui
    src/platform/workbench/ui/SignalResultCenterPanel.ui
    src/platform/workbench/ui/SignalSettingsPanel.ui
    src/platform/workbench/ui/SignalDiagnosticsPanel.ui)
  target_sources(SignalStudioWorkbench PRIVATE
    ${_signal_studio_workbench_ui_headers}
    src/platform/workbench/ui/SignalWorkbenchMainWindow.ui
    src/platform/workbench/ui/SignalInspectorPanel.ui
    src/platform/workbench/ui/SignalTaskCenterPanel.ui
    src/platform/workbench/ui/SignalResultCenterPanel.ui
    src/platform/workbench/ui/SignalSettingsPanel.ui
    src/platform/workbench/ui/SignalDiagnosticsPanel.ui)
  target_include_directories(SignalStudioWorkbench PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(SignalStudioWorkbench PUBLIC SignalStudio::Visualization SignalStudio::TaskRuntime SignalStudio::Core
    PRIVATE Qt6::Core Qt6::Widgets)
  set(_signal_studio_ui_targets SignalStudioVisualization SignalStudioWorkbench)

  add_library(SignalStudioApplicationCore STATIC
    apps/signal_studio/application.cpp)
  target_compile_features(SignalStudioApplicationCore PUBLIC cxx_std_20)
  target_include_directories(SignalStudioApplicationCore
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/apps/signal_studio")
  target_link_libraries(SignalStudioApplicationCore
    PUBLIC SignalStudio::Core SignalStudio::Data SignalStudio::DSP SignalStudio::Compute
      SignalStudio::TaskRuntime SignalStudio::Visualization SignalStudio::Workbench)
  signal_studio_enable_warnings(SignalStudioApplicationCore)

  qt_wrap_ui(_signal_studio_application_ui_headers
    apps/signal_studio/ui/SignalStudioProjectHome.ui
    apps/signal_studio/ui/SignalImportWizard.ui
    apps/signal_studio/ui/SignalLoadProgressDialog.ui
    apps/signal_studio/ui/SignalInspectorPage.ui
    apps/signal_studio/ui/SignalResultCenterPage.ui
    apps/signal_studio/ui/SignalAnalysisSettingsPanel.ui)
  add_executable(signal_studio WIN32
    apps/signal_studio/main.cpp
    apps/signal_studio/qt_application.cpp
    ${_signal_studio_application_ui_headers}
    apps/signal_studio/ui/SignalStudioProjectHome.ui
    apps/signal_studio/ui/SignalImportWizard.ui
    apps/signal_studio/ui/SignalLoadProgressDialog.ui
    apps/signal_studio/ui/SignalInspectorPage.ui
    apps/signal_studio/ui/SignalResultCenterPage.ui
    apps/signal_studio/ui/SignalAnalysisSettingsPanel.ui)
  set_target_properties(signal_studio PROPERTIES OUTPUT_NAME "SignalStudio")
  target_include_directories(signal_studio PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/signal_studio"
    "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(signal_studio PRIVATE SignalStudioApplicationCore Qt6::Core Qt6::Widgets)
  signal_studio_enable_warnings(signal_studio)
  signal_studio_deploy_msvc_runtime(signal_studio)
  signal_studio_deploy_qt_runtime(signal_studio)

  add_executable(signal_visualization_workbench_demo WIN32
    examples/visualization_workbench_demo.cpp)
  target_link_libraries(signal_visualization_workbench_demo
    PRIVATE SignalStudio::Visualization SignalStudio::Workbench Qt6::Core Qt6::Widgets)
  signal_studio_enable_warnings(signal_visualization_workbench_demo)
  signal_studio_deploy_msvc_runtime(signal_visualization_workbench_demo)
  signal_studio_deploy_qt_runtime(signal_visualization_workbench_demo)
endif()

signal_studio_verify_dependency_graph()

install(TARGETS ${_signal_studio_headless_targets} EXPORT SignalStudioHeadlessTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT SignalStudioHeadlessTargets FILE SignalStudioHeadlessTargets.cmake NAMESPACE SignalStudio::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SignalStudio")
if(SIGNAL_STUDIO_BUILD_UI)
  install(TARGETS ${_signal_studio_ui_targets} EXPORT SignalStudioUiTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(EXPORT SignalStudioUiTargets FILE SignalStudioUiTargets.cmake NAMESPACE SignalStudio::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SignalStudio")
  install(TARGETS signal_studio signal_visualization_workbench_demo
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime)
  install(FILES
    "$<TARGET_FILE:Qt6::Core>"
    "$<TARGET_FILE:Qt6::Gui>"
    "$<TARGET_FILE:Qt6::Widgets>"
    DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime)
  if(WIN32)
    install(FILES
      "$<TARGET_FILE:Qt6::QWindowsIntegrationPlugin>"
      "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
      DESTINATION "${CMAKE_INSTALL_BINDIR}/platforms" COMPONENT Runtime)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qt.conf"
      DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime)
  endif()
endif()
install(DIRECTORY include/signal_studio DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES examples/plugin_sdk/minimal_plugin.c DESTINATION "${CMAKE_INSTALL_DATADIR}/SignalStudio/examples/plugin_sdk")
if(SIGNAL_STUDIO_MSVC_INSTALL_RUNTIME_FILES)
  install(FILES ${SIGNAL_STUDIO_MSVC_INSTALL_RUNTIME_FILES} DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime)
endif()
if(_signal_studio_mkl_runtime_files)
  install(FILES ${_signal_studio_mkl_runtime_files}
    DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime)
  set(_signal_studio_mkl_license
    "${_signal_studio_local_dependency_prefix}/share/intel-mkl/copyright")
  if(EXISTS "${_signal_studio_mkl_license}")
    install(FILES "${_signal_studio_mkl_license}"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/SignalStudio/licenses"
      RENAME "intel-mkl.txt" COMPONENT Runtime)
  endif()
endif()
install(IMPORTED_RUNTIME_ARTIFACTS SampleRate::samplerate
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime)
set(_signal_studio_libsamplerate_license
  "${_signal_studio_local_dependency_prefix}/share/libsamplerate/copyright")
if(EXISTS "${_signal_studio_libsamplerate_license}")
  install(FILES "${_signal_studio_libsamplerate_license}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/SignalStudio/licenses"
    RENAME "libsamplerate.txt" COMPONENT Runtime)
endif()
if(_signal_studio_cuda_runtime_files)
  install(FILES ${_signal_studio_cuda_runtime_files}
    DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime)
  if(EXISTS "${_signal_studio_cuda_root}/EULA.txt")
    install(FILES "${_signal_studio_cuda_root}/EULA.txt"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/SignalStudio/licenses"
      RENAME "nvidia-cuda-toolkit.txt" COMPONENT Runtime)
  endif()
endif()

configure_package_config_file(cmake/SignalStudioConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/SignalStudioConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SignalStudio")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/SignalStudioConfigVersion.cmake" VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/SignalStudioConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/SignalStudioConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SignalStudio")

if(SIGNAL_STUDIO_BUILD_SDK_EXAMPLE)
  add_library(signal_studio_minimal_plugin SHARED examples/plugin_sdk/minimal_plugin.c)
  target_include_directories(signal_studio_minimal_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  signal_studio_enable_warnings(signal_studio_minimal_plugin)
endif()

if(BUILD_TESTING AND SIGNAL_STUDIO_BUILD_TESTS)
  add_subdirectory(tests/platform)
  add_subdirectory(tests/core)
  add_subdirectory(tests/data)
  add_subdirectory(tests/task_runtime)
  add_subdirectory(tests/compute)
  add_subdirectory(tests/dsp)
  add_subdirectory(tests/integration/ms02)
  add_subdirectory(benchmarks)
  if(SIGNAL_STUDIO_BUILD_UI)
    add_subdirectory(tests/visualization)
    add_subdirectory(tests/app)
  endif()
endif()

message(STATUS "Signal Processing Platform ${PROJECT_VERSION}")
message(STATUS "UI components: ${SIGNAL_STUDIO_BUILD_UI}")
if(SIGNAL_STUDIO_BUILD_UI)
  message(STATUS "Qt: ${Qt6_VERSION} (${Qt6_DIR})")
endif()
message(STATUS "CUDA mode: ${_signal_studio_cuda_mode}; toolkit available: ${SIGNAL_STUDIO_CUDA_AVAILABLE}")
message(STATUS "oneMKL mode: ${_signal_studio_onemkl_mode}; adapter available: ${SIGNAL_STUDIO_ONEMKL_AVAILABLE}")
message(STATUS "Approved external test data available: ${SIGNAL_STUDIO_EXTERNAL_TEST_DATA_AVAILABLE}")
message(STATUS "MS-02 real recording performance data available: ${SIGNAL_STUDIO_MS02_PERF_RECORDING_AVAILABLE}")
