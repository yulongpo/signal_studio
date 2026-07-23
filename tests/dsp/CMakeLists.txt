add_executable(signal_studio_dsp_tests dsp_tests.cpp)
target_link_libraries(signal_studio_dsp_tests PRIVATE SignalStudio::DSP SignalStudio::Data)
signal_studio_enable_warnings(signal_studio_dsp_tests)
signal_studio_deploy_msvc_runtime(signal_studio_dsp_tests)

# CPU-only DSP tests: windows, statistics, filtering, resampling. These do not require an FFT
# backend and run in every configuration (including headless / CUDA-off).
foreach(_case IN ITEMS
    windows-length-and-range
    windows-enbw
    windows-symmetry
    statistics-real
    statistics-complex
    filter-impulse-response
    filter-lowpass-attenuates
    resample-identity
    resample-length-up
    resample-length-down
    resample-sine-amplitude)
  add_test(NAME "dsp.${_case}" COMMAND signal_studio_dsp_tests --case "${_case}")
  set_tests_properties("dsp.${_case}" PROPERTIES TIMEOUT 20 LABELS "unit;contract;ms-02;dsp")
endforeach()

# FFT/PSD/STFT tests require a working FFT backend. The BL1.0-sanctioned CPU backend (oneMKL) is
# not installed in this environment, so these run only when the cuFFT (CUDA) backend is built.
if(SIGNAL_STUDIO_CUDA_AVAILABLE)
  foreach(_case IN ITEMS
      fft-single-tone-bin
      fft-parseval
      fft-ifft-roundtrip
      psd-tone-frequency
      stft-tone-timefreq)
    add_test(NAME "dsp.${_case}" COMMAND signal_studio_dsp_tests --case "${_case}")
    set_tests_properties("dsp.${_case}" PROPERTIES TIMEOUT 40 LABELS "unit;contract;ms-02;dsp;cuda")
  endforeach()
endif()
