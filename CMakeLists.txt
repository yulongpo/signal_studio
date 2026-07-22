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
option(SIGNAL_STUDIO_BUILD_TESTS "Build MS-00 verification tests" ${BUILD_TESTING})
option(SIGNAL_STUDIO_BUILD_SDK_EXAMPLE "Build the C ABI SDK example" ${BUILD_TESTING})
set(SIGNAL_STUDIO_QT_ROOT "$ENV{SIGNAL_STUDIO_QT_ROOT}" CACHE PATH "Qt installation prefix for UI builds")
set(SIGNAL_STUDIO_MINIMUM_QT_VERSION "6.10.3")
set(SIGNAL_STUDIO_CUDA_MODE "AUTO" CACHE STRING "CUDA request: AUTO, ON, or OFF")
set_property(CACHE SIGNAL_STUDIO_CUDA_MODE PROPERTY STRINGS AUTO ON OFF)

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
  src/platform/core/capability.cpp src/platform/core/module_descriptor.cpp src/platform/core/module.cpp)
target_include_directories(SignalStudioCore PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
signal_studio_add_module(SignalStudioCompute Compute src/platform/compute/module.cpp)
target_link_libraries(SignalStudioCompute PUBLIC SignalStudio::Core)
signal_studio_add_module(SignalStudioData Data src/platform/data/module.cpp)
target_link_libraries(SignalStudioData PUBLIC SignalStudio::Core)
signal_studio_add_module(SignalStudioTaskRuntime TaskRuntime src/platform/task_runtime/module.cpp)
target_link_libraries(SignalStudioTaskRuntime PUBLIC SignalStudio::Compute SignalStudio::Core)
signal_studio_add_module(SignalStudioDSP DSP src/platform/dsp/module.cpp)
target_link_libraries(SignalStudioDSP PUBLIC SignalStudio::Data SignalStudio::Compute SignalStudio::Core)
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
  signal_studio_add_module(SignalStudioVisualization Visualization src/platform/visualization/module.cpp)
  target_link_libraries(SignalStudioVisualization PUBLIC SignalStudio::Data SignalStudio::TaskRuntime SignalStudio::Core
    PRIVATE Qt6::Core Qt6::Widgets)
  signal_studio_add_module(SignalStudioWorkbench Workbench src/platform/workbench/module.cpp)
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
endif()

message(STATUS "Signal Processing Platform ${PROJECT_VERSION}")
message(STATUS "UI components: ${SIGNAL_STUDIO_BUILD_UI}")
if(SIGNAL_STUDIO_BUILD_UI)
  message(STATUS "Qt: ${Qt6_VERSION} (${Qt6_DIR})")
endif()
message(STATUS "CUDA mode: ${_signal_studio_cuda_mode}; toolkit available: ${SIGNAL_STUDIO_CUDA_AVAILABLE}")
