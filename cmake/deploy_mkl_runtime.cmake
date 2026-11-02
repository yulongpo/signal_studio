# Stage the oneMKL DFT runtime beside the executable. This is an explicit file
# copy, not a PATH or current-directory search. A missing toolchain leaves the
# process on the handwritten transform.
if(NOT DEFINED SS_MKL_BIN OR NOT DEFINED SS_OUTPUT_DIR)
    message(FATAL_ERROR "Missing oneMKL deploy parameters")
endif()

set(_ss_mkl_required
    mkl_rt.3.dll
    mkl_intel_thread.3.dll
    mkl_core.3.dll
    mkl_def.3.dll
    libiomp5md.dll
    libimalloc.dll)
set(_ss_mkl_kernels
    mkl_avx2.3.dll
    mkl_avx512.3.dll
    mkl_avx10.3.dll
    mkl_mc3.3.dll
    mkl_vml_def.3.dll
    mkl_vml_avx2.3.dll
    mkl_vml_avx512.3.dll
    mkl_vml_avx10.3.dll
    mkl_vml_mc3.3.dll
    mkl_vml_cmpt.3.dll)

foreach(_ss_mkl_name IN LISTS _ss_mkl_required)
    if(NOT EXISTS "${SS_MKL_BIN}/${_ss_mkl_name}")
        message(STATUS "oneMKL deploy skipped; missing ${SS_MKL_BIN}/${_ss_mkl_name}")
        return()
    endif()
endforeach()

foreach(_ss_mkl_name IN LISTS _ss_mkl_required _ss_mkl_kernels)
    if(NOT EXISTS "${SS_MKL_BIN}/${_ss_mkl_name}")
        continue()
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SS_MKL_BIN}/${_ss_mkl_name}"
            "${SS_OUTPUT_DIR}/${_ss_mkl_name}"
        RESULT_VARIABLE _ss_mkl_copy)
    if(NOT _ss_mkl_copy EQUAL 0)
        message(FATAL_ERROR "oneMKL copy failed for ${_ss_mkl_name}")
    endif()
endforeach()
