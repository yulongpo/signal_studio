cmake_minimum_required(VERSION 3.28)
project(SignalStudioInstalledConsumer LANGUAGES CXX)

find_package(SignalStudio 1 CONFIG REQUIRED COMPONENTS Core Data DSP Compute TaskRuntime Visualization Workbench PluginSDK ModelRuntime Dataset)

set(_required_targets Core Data DSP Compute TaskRuntime Visualization Workbench PluginSDK ModelRuntime Dataset)
foreach(_target IN LISTS _required_targets)
  if(NOT TARGET SignalStudio::${_target})
    message(FATAL_ERROR "Installed package is missing SignalStudio::${_target}")
  endif()
endforeach()

add_executable(signal_studio_consumer_smoke main.cpp)
target_compile_features(signal_studio_consumer_smoke PRIVATE cxx_std_20)
target_link_libraries(
  signal_studio_consumer_smoke
  PRIVATE
    SignalStudio::Core
    SignalStudio::Data
    SignalStudio::DSP
    SignalStudio::Compute
    SignalStudio::TaskRuntime
    SignalStudio::Visualization
    SignalStudio::Workbench
    SignalStudio::PluginSDK
    SignalStudio::ModelRuntime
    SignalStudio::Dataset
)
