# The production backend is optional. Models are independent deployment assets;
# a build without these exact headers still supports the explicit test fixture.
set(SS_DETECTION_TRT_ROOT "$ENV{SS_DETECTION_TRT_ROOT}" CACHE PATH
    "Official TensorRT 10.11.0.33 / CUDA 12.9 SDK staging root")

function(signalstudio_configure_detection_model_backend target)
    target_sources(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/infrastructure/detection_model_backend.cpp"
        "${CMAKE_SOURCE_DIR}/infrastructure/detection_model_backend.h")
    if(SS_DETECTION_TRT_ROOT)
        if(NOT WIN32)
            message(FATAL_ERROR "The frozen production TensorRT package is Windows x64 only")
        endif()
        set(_ss_trt_include "${SS_DETECTION_TRT_ROOT}/include")
        set(_ss_cuda_include "${SS_DETECTION_TRT_ROOT}/nvidia/cuda_runtime/include")
        foreach(_ss_header IN ITEMS "${_ss_trt_include}/NvInferVersion.h"
                "${_ss_trt_include}/NvInferRuntime.h"
                "${_ss_cuda_include}/cuda_runtime_api.h"
                "${_ss_cuda_include}/crt/host_defines.h")
            if(NOT EXISTS "${_ss_header}")
                message(FATAL_ERROR "Incomplete production model SDK: ${_ss_header}")
            endif()
        endforeach()
        file(READ "${_ss_trt_include}/NvInferVersion.h" _ss_trt_version)
        foreach(_ss_version IN ITEMS "MAJOR;10" "MINOR;11" "PATCH;0" "BUILD;33")
            list(GET _ss_version 0 _ss_version_name)
            list(GET _ss_version 1 _ss_version_value)
            if(NOT _ss_trt_version MATCHES
                    "#define TRT_${_ss_version_name}_ENTERPRISE +${_ss_version_value}([^0-9]|$)" OR
                    NOT _ss_trt_version MATCHES
                    "#define NV_TENSORRT_${_ss_version_name} +TRT_${_ss_version_name}_ENTERPRISE([^A-Z_]|$)")
                message(FATAL_ERROR "Production model SDK requires TensorRT 10.11.0.33 EXACT")
            endif()
        endforeach()
        target_include_directories(${target} SYSTEM PRIVATE
            "${_ss_trt_include}" "${_ss_cuda_include}")
        target_compile_definitions(${target} PRIVATE SS_DETECTION_TENSORRT=1)
        # QLibrary resolves both official DLLs lazily after hash/device preflight.
        # There are no TensorRT/CUDA imports that would block fixture startup.
    endif()
endfunction()
