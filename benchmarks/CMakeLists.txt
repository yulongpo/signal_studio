find_package(benchmark CONFIG REQUIRED)
add_executable(signal_studio_ms02_benchmarks ms02_benchmarks.cpp)
target_include_directories(signal_studio_ms02_benchmarks PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src/platform/dsp")
target_compile_definitions(signal_studio_ms02_benchmarks PRIVATE
  SIGNAL_STUDIO_MS02_PERF_RECORDING="${SIGNAL_STUDIO_MS02_PERF_RECORDING}"
  SIGNAL_STUDIO_MS02_PERF_RECORDING_AVAILABLE=$<BOOL:${SIGNAL_STUDIO_MS02_PERF_RECORDING_AVAILABLE}>
  SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256="${SIGNAL_STUDIO_MS02_PERF_RECORDING_SHA256}")
target_link_libraries(signal_studio_ms02_benchmarks
  PRIVATE SignalStudio::DSP SignalStudio::Data SignalStudio::Compute benchmark::benchmark)
if(SIGNAL_STUDIO_CUDA_AVAILABLE)
  target_compile_definitions(signal_studio_ms02_benchmarks PRIVATE
    SIGNAL_STUDIO_BENCHMARK_HAVE_CUDA=1)
  target_link_libraries(signal_studio_ms02_benchmarks PRIVATE CUDA::cudart CUDA::cufft)
endif()
signal_studio_enable_warnings(signal_studio_ms02_benchmarks)
signal_studio_deploy_msvc_runtime(signal_studio_ms02_benchmarks)
add_custom_command(TARGET signal_studio_ms02_benchmarks POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:benchmark::benchmark>"
    "$<TARGET_FILE_DIR:signal_studio_ms02_benchmarks>"
  COMMENT "部署锁定的 Google Benchmark 1.9.5 运行时")
add_test(NAME benchmark.ms02
  COMMAND signal_studio_ms02_benchmarks --benchmark_color=false)
set_tests_properties(benchmark.ms02 PROPERTIES
  TIMEOUT 900 LABELS "benchmark;integration;ms-02;performance" RUN_SERIAL TRUE)
