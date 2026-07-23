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

set(SIGNAL_STUDIO_MSVC_RUNTIME_FILES)
if(MSVC AND WIN32)
  if(NOT DEFINED ENV{VCToolsRedistDir})
    message(FATAL_ERROR "缺少 VCToolsRedistDir，无法部署与当前 MSVC 工具集匹配的 VC 运行库")
  endif()
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}/debug_nonredist/x64/Microsoft.VC143.DebugCRT"
      SIGNAL_STUDIO_MSVC_RUNTIME_DIR)
  else()
    file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}/x64/Microsoft.VC143.CRT" SIGNAL_STUDIO_MSVC_RUNTIME_DIR)
  endif()
  file(GLOB SIGNAL_STUDIO_MSVC_RUNTIME_FILES CONFIGURE_DEPENDS
    "${SIGNAL_STUDIO_MSVC_RUNTIME_DIR}/*.dll")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(NOT DEFINED ENV{UniversalCRTSdkDir} OR NOT DEFINED ENV{UCRTVersion})
      message(FATAL_ERROR "缺少 UniversalCRTSdkDir/UCRTVersion，无法部署 Debug UCRT")
    endif()
    file(TO_CMAKE_PATH "$ENV{UniversalCRTSdkDir}/bin/$ENV{UCRTVersion}/x64/ucrt/ucrtbased.dll"
      _signal_studio_debug_ucrt)
    if(NOT EXISTS "${_signal_studio_debug_ucrt}")
      message(FATAL_ERROR "未找到与 Windows SDK 匹配的 Debug UCRT：${_signal_studio_debug_ucrt}")
    endif()
    list(APPEND SIGNAL_STUDIO_MSVC_RUNTIME_FILES "${_signal_studio_debug_ucrt}")
  endif()
  if(NOT SIGNAL_STUDIO_MSVC_RUNTIME_FILES)
    message(FATAL_ERROR "未找到与构建类型匹配的 x64 VC143 运行库集：${SIGNAL_STUDIO_MSVC_RUNTIME_DIR}")
  endif()
endif()

function(signal_studio_deploy_msvc_runtime target)
  if(SIGNAL_STUDIO_MSVC_RUNTIME_FILES)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${SIGNAL_STUDIO_MSVC_RUNTIME_FILES} "$<TARGET_FILE_DIR:${target}>"
      COMMAND_EXPAND_LISTS
      COMMENT "部署与 MSVC 工具集匹配的 VC143 运行库")
  endif()
endfunction()

string(TOUPPER "${SIGNAL_STUDIO_CUDA_MODE}" _signal_studio_cuda_mode)
if(NOT _signal_studio_cuda_mode MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "SIGNAL_STUDIO_CUDA_MODE must be AUTO, ON, or OFF")
endif()

set(SIGNAL_STUDIO_CUDA_AVAILABLE OFF)
if(NOT _signal_studio_cuda_mode STREQUAL "OFF")
  find_program(SIGNAL_STUDIO_NVCC_EXECUTABLE nvcc)
  if(SIGNAL_STUDIO_NVCC_EXECUTABLE)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
      set(SIGNAL_STUDIO_CUDA_AVAILABLE ON)
    endif()
  endif()
  if(_signal_studio_cuda_mode STREQUAL "ON" AND NOT SIGNAL_STUDIO_CUDA_AVAILABLE)
    message(WARNING "CUDA was requested but no CUDA Toolkit/nvcc was found; continuing with the supported CPU backend")
  endif()
endif()

if(SIGNAL_STUDIO_BUILD_UI)
  if(SIGNAL_STUDIO_QT_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${SIGNAL_STUDIO_QT_ROOT}")
  endif()
  find_package(Qt6 ${SIGNAL_STUDIO_MINIMUM_QT_VERSION} REQUIRED COMPONENTS Core Widgets)
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

signal_studio_add_module(SignalStudioCore Core src/platform/core/version.cpp src/platform/core/error.cpp
  src/platform/core/capability.cpp src/platform/core/module_descriptor.cpp src/platform/core/module.cpp
  src/platform/core/services.cpp)
target_include_directories(SignalStudioCore PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
signal_studio_add_module(SignalStudioCompute Compute src/platform/compute/module.cpp
  src/platform/compute/backend.cpp)
target_link_libraries(SignalStudioCompute PUBLIC SignalStudio::Core)
if(SIGNAL_STUDIO_CUDA_AVAILABLE)
  target_link_libraries(SignalStudioCompute PRIVATE CUDA::cudart)
  target_compile_definitions(SignalStudioCompute PRIVATE SIGNAL_STUDIO_HAVE_CUDA)
endif()
signal_studio_add_module(SignalStudioData Data src/platform/data/module.cpp src/platform/data/signal.cpp
  src/platform/data/raw.cpp src/platform/data/io.cpp src/platform/data/loading.cpp
  src/platform/data/preview.cpp src/platform/data/index.cpp)
target_link_libraries(SignalStudioData PUBLIC SignalStudio::Core)
signal_studio_add_module(SignalStudioTaskRuntime TaskRuntime src/platform/task_runtime/module.cpp
  src/platform/task_runtime/task_runtime.cpp)
target_link_libraries(SignalStudioTaskRuntime PUBLIC SignalStudio::Compute SignalStudio::Core)
signal_studio_add_module(SignalStudioDSP DSP src/platform/dsp/module.cpp src/platform/dsp/windows.cpp
  src/platform/dsp/statistics.cpp src/platform/dsp/fft.cpp src/platform/dsp/spectrum.cpp
  src/platform/dsp/stft.cpp src/platform/dsp/filter.cpp src/platform/dsp/resample.cpp)
target_link_libraries(SignalStudioDSP PUBLIC SignalStudio::Data SignalStudio::Compute SignalStudio::Core)
if(SIGNAL_STUDIO_CUDA_AVAILABLE)
  target_link_libraries(SignalStudioDSP PRIVATE CUDA::cufft)
  target_compile_definitions(SignalStudioDSP PRIVATE SIGNAL_STUDIO_HAVE_CUDA)
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
  signal_studio_add_module(SignalStudioVisualization Visualization src/platform/visualization/module.cpp
    src/platform/visualization/data_series.cpp src/platform/visualization/viewport.cpp
    src/platform/visualization/overlay.cpp src/platform/visualization/views.cpp)
  target_link_libraries(SignalStudioVisualization PUBLIC SignalStudio::Data SignalStudio::TaskRuntime SignalStudio::Core
    PRIVATE Qt6::Core Qt6::Widgets)
  signal_studio_add_module(SignalStudioWorkbench Workbench src/platform/workbench/module.cpp
    src/platform/workbench/workbench.cpp)
  target_link_libraries(SignalStudioWorkbench PUBLIC SignalStudio::Visualization SignalStudio::TaskRuntime SignalStudio::Core
    PRIVATE Qt6::Core Qt6::Widgets)
  set(_signal_studio_ui_targets SignalStudioVisualization SignalStudioWorkbench)
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
endif()
install(DIRECTORY include/signal_studio DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES examples/plugin_sdk/minimal_plugin.c DESTINATION "${CMAKE_INSTALL_DATADIR}/SignalStudio/examples/plugin_sdk")
if(SIGNAL_STUDIO_MSVC_RUNTIME_FILES)
  install(FILES ${SIGNAL_STUDIO_MSVC_RUNTIME_FILES} DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime)
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

if(SIGNAL_STUDIO_BUILD_UI)
  add_subdirectory(apps/signal_studio)
endif()

if(BUILD_TESTING AND SIGNAL_STUDIO_BUILD_TESTS)
  add_subdirectory(tests/platform)
  add_subdirectory(tests/core)
  add_subdirectory(tests/data)
  add_subdirectory(tests/task_runtime)
  add_subdirectory(tests/compute)
  add_subdirectory(tests/dsp)
  if(SIGNAL_STUDIO_BUILD_UI)
    add_subdirectory(tests/visualization)
    add_subdirectory(tests/workbench)
    add_subdirectory(tests/app)
  endif()
endif()

message(STATUS "Signal Processing Platform ${PROJECT_VERSION}")
message(STATUS "UI components: ${SIGNAL_STUDIO_BUILD_UI}")
if(SIGNAL_STUDIO_BUILD_UI)
  message(STATUS "Qt: ${Qt6_VERSION} (${Qt6_DIR})")
endif()
message(STATUS "CUDA mode: ${_signal_studio_cuda_mode}; toolkit available: ${SIGNAL_STUDIO_CUDA_AVAILABLE}")
