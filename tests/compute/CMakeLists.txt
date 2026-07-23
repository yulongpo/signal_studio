add_executable(signal_studio_compute_tests compute_tests.cpp)
target_link_libraries(signal_studio_compute_tests PRIVATE SignalStudio::Compute)
signal_studio_enable_warnings(signal_studio_compute_tests)
signal_studio_deploy_msvc_runtime(signal_studio_compute_tests)

foreach(_case IN ITEMS
    cpu-backend-capabilities
    buffer-pool-budget
    buffer-pool-zero-rejected
    selector-cpu-fallback
    selector-gpu-required-missing
    provenance-recorded)
  add_test(NAME "compute.${_case}" COMMAND signal_studio_compute_tests --case "${_case}")
  set_tests_properties("compute.${_case}" PROPERTIES TIMEOUT 20 LABELS "unit;contract;ms-02;compute")
endforeach()

if(SIGNAL_STUDIO_CUDA_AVAILABLE)
  foreach(_case IN ITEMS cuda-backend-probe selector-cuda-preferred)
    add_test(NAME "compute.${_case}" COMMAND signal_studio_compute_tests --case "${_case}")
    set_tests_properties("compute.${_case}" PROPERTIES TIMEOUT 30 LABELS "unit;contract;ms-02;compute;cuda")
  endforeach()
endif()
