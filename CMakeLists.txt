cmake_minimum_required(VERSION 4.2)
project(SignalStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

option(SS_BUILD_TESTS "Build the state, persistence, and Qt UI tests" ON)
option(SS_DEPLOY_RUNTIME "Deploy the Qt runtime next to the Windows executable" ON)
set(SS_GPU_DSP_RUNTIME_ROOT "$ENV{SS_GPU_DSP_RUNTIME_ROOT}" CACHE PATH
    "Optional flat CUDA DSP module/cuFFT runtime staging directory")
set(SS_QT_ROOT "$ENV{SS_QT_ROOT}" CACHE PATH "Qt 6.11 installation root")
if(SS_QT_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${SS_QT_ROOT}")
endif()

# QRhi uses limited-compatibility Gui APIs. Keep headers, tools and runtime matched.
find_package(Qt6 6.11.1 EXACT REQUIRED COMPONENTS Core Widgets GuiPrivate OpenGL ShaderTools)
find_package(Threads REQUIRED)
if(SS_BUILD_TESTS)
    find_package(Qt6 6.11.1 EXACT REQUIRED COMPONENTS Test)
endif()

# All configurations have a stable, separately deployable output directory.
set(SS_OUTPUT_PREFIX "vs2026-qt611" CACHE STRING "Prefix for the standalone output directory")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_SOURCE_DIR}/out/${SS_OUTPUT_PREFIX}-$<LOWER_CASE:$<CONFIG>>_bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

function(ss_target_defaults target)
    target_include_directories(${target} PUBLIC "${CMAKE_SOURCE_DIR}")
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(ss_stage_mkl_runtime target)
    if(NOT WIN32 OR NOT SS_DEPLOY_RUNTIME)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DSS_MKL_BIN=${CMAKE_SOURCE_DIR}/out/toolchains/fft-libs/conda/Library/bin"
            "-DSS_OUTPUT_DIR=$<TARGET_FILE_DIR:${target}>"
            -P "${CMAKE_SOURCE_DIR}/cmake/deploy_mkl_runtime.cmake"
        COMMENT "Staging oneMKL DFT runtime next to ${target}"
        VERBATIM)
endfunction()

function(ss_add_graphics_shaders target resource_name)
    # Default qsb output includes SPIR-V, GLSL, HLSL 5.0 and MSL. Do not require
    # the optional platform-native precompiler (fxc/xcrun) just to build shaders.
    qt_add_shaders(${target} "${resource_name}"
        PREFIX "/signalstudio/shaders"
        BASE "${CMAKE_SOURCE_DIR}/ui/charts/shaders"
        FILES
            "${CMAKE_SOURCE_DIR}/ui/charts/shaders/heatmap.frag"
            "${CMAKE_SOURCE_DIR}/ui/charts/shaders/texture.vert"
            "${CMAKE_SOURCE_DIR}/ui/charts/shaders/texture.frag"
    )
endfunction()

function(ss_add_ui_assets target)
    qt_add_resources(${target} "${target}_ui_assets"
        PREFIX "/signalstudio/ui"
        BASE "${CMAKE_SOURCE_DIR}/resources/ui"
        FILES "${CMAKE_SOURCE_DIR}/resources/ui/check.svg"
              "${CMAKE_SOURCE_DIR}/resources/ui/check-partial.svg")
endfunction()

add_library(SignalStudioCore STATIC
    domain/constellation.cpp
    domain/channel_sampling.cpp
    domain/annotation.cpp
    domain/detection.cpp
    domain/detection_postprocess.cpp
    domain/scn_detection_postprocess.cpp
    domain/detection_refinement.cpp
    domain/detection_refined_fusion.cpp
    application/annotation_service.cpp
    domain/sample_format.cpp
    domain/project.cpp
    domain/project.h
    application/session.cpp
    application/session.h
)
set_target_properties(SignalStudioCore PROPERTIES AUTOMOC OFF)
ss_target_defaults(SignalStudioCore)

add_library(SignalStudioInfrastructure STATIC
    application/detection_result_service.cpp
    infrastructure/wav_metadata.cpp
    infrastructure/label_import.cpp
    infrastructure/annotation_json.cpp
    infrastructure/annotation_migration.cpp
    infrastructure/annotation_store.cpp
    infrastructure/detection_source.cpp
    infrastructure/gpu_dsp.cpp
    infrastructure/mkl_dft.cpp
    infrastructure/detection_sample_stream.cpp
    infrastructure/detection_sample_stream_scratch.cpp
    infrastructure/detection_sample_stream_scratch_recovery.cpp
    infrastructure/detection_protocol.cpp
    infrastructure/detection_store.cpp
    infrastructure/detection_task.cpp
    infrastructure/detection_resource_diagnostics.cpp
    infrastructure/runtime_log.cpp
    infrastructure/detection_worker_client.cpp
    infrastructure/detection_projection.cpp
    infrastructure/detection_model_snapshot.cpp
    infrastructure/ffscn_engine_package.cpp
    infrastructure/ffscn_engine_package.h
    infrastructure/detection_frequency_analysis.cpp
    infrastructure/detection_spectrum_spool.cpp
    infrastructure/detection_spectrum_spool.h
    infrastructure/detection_sample_energy.cpp
    infrastructure/detection_time_analysis.cpp
    infrastructure/detection_refined_publication.cpp
    infrastructure/import_controller.cpp
    infrastructure/format_template_store.cpp
    infrastructure/source_metadata.cpp
    infrastructure/sigmf_metadata.cpp
    infrastructure/raw_sample_reader.cpp
    infrastructure/channel_processor.cpp
    infrastructure/narrowband_analysis/analysis_source.cpp
    infrastructure/narrowband_analysis/constellation_analysis.cpp
    infrastructure/narrowband_analysis/analysis_service.cpp
    infrastructure/narrowband_analysis/pulse_shaping.cpp
    infrastructure/narrowband_analysis/eye_analysis.cpp
    infrastructure/narrowband_analysis/symbol_rate_estimator.cpp
    infrastructure/narrowband_analysis/single_carrier_recovery.cpp
    infrastructure/channel_processor.h
    infrastructure/spectral_analysis.cpp
    infrastructure/spectral_windows.cpp
    infrastructure/source_loader.cpp
    infrastructure/spectral_analysis.h
    infrastructure/int16_iq_file.cpp
    infrastructure/int16_iq_file.h
    infrastructure/project_store.cpp
    infrastructure/project_store.h
    infrastructure/project_file.cpp
    infrastructure/project_file.h
    infrastructure/label_discovery.cpp
)
set_target_properties(SignalStudioInfrastructure PROPERTIES AUTOMOC OFF)
target_link_libraries(SignalStudioInfrastructure PUBLIC SignalStudioCore Qt6::Core)
ss_target_defaults(SignalStudioInfrastructure)
if(WIN32)
    target_link_libraries(SignalStudioInfrastructure PRIVATE psapi)
endif()

# Headless production-pipeline measurements, with isolated output roots. This
# does not replace native QWidget/QRhi acceptance or model-quality evaluation.
add_executable(SignalStudioDetectionBenchmark tools/detection_benchmark/main.cpp)
add_executable(SignalStudioSampleRefinementBenchmark tools/sample_refinement_benchmark/main.cpp)
set_target_properties(SignalStudioSampleRefinementBenchmark PROPERTIES AUTOMOC OFF)
ss_target_defaults(SignalStudioSampleRefinementBenchmark)
target_link_libraries(SignalStudioSampleRefinementBenchmark PRIVATE SignalStudioInfrastructure)
ss_stage_mkl_runtime(SignalStudioSampleRefinementBenchmark)
set_target_properties(SignalStudioDetectionBenchmark PROPERTIES AUTOMOC OFF)
ss_target_defaults(SignalStudioDetectionBenchmark)
target_link_libraries(SignalStudioDetectionBenchmark PRIVATE SignalStudioInfrastructure)
ss_stage_mkl_runtime(SignalStudioDetectionBenchmark)
if(WIN32)
    target_link_libraries(SignalStudioDetectionBenchmark PRIVATE psapi)
endif()

execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE SS_BUILD_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE)
add_library(SignalStudioBrand STATIC ui/brand/brand_assets.cpp ui/brand/brand_assets.h
    resources/branding/qt/resources/branding.qrc)
set_target_properties(SignalStudioBrand PROPERTIES AUTORCC ON AUTOMOC OFF)
ss_target_defaults(SignalStudioBrand)
target_link_libraries(SignalStudioBrand PUBLIC Qt6::Widgets)
target_compile_definitions(SignalStudioBrand PRIVATE SS_PRODUCT_VERSION="${PROJECT_VERSION}" SS_BUILD_REVISION="${SS_BUILD_REVISION}")

set(SS_UI_SOURCES
    ui/label_mapping_dialog.cpp
    ui/label_mapping_dialog.h
    ui/settings_popover.cpp
    ui/settings_popover.h
    ui/chart_control_strip.h
    ui/context_stack.h
    ui/detection/detection_card.cpp
    ui/detection/detection_card.h
    ui/detection/detection_panel.cpp
    ui/detection/detection_panel.h
    ui/detection/detection_resource_options.cpp
    ui/detection/detection_resource_options.h
    ui/detection/ffscn_engine_dialog.cpp
    ui/detection/ffscn_engine_dialog.h
    ui/detection/ffscn_engine_switch_dialog.cpp
    ui/detection/ffscn_engine_switch_dialog.h
    ui/annotations/annotation_panel.cpp
    ui/annotations/annotation_panel.h
    ui/annotations/annotation_evidence_widget.cpp
    ui/annotations/annotation_evidence_widget.h
    ui/controls/adaptive_value_edit.cpp
    ui/controls/adaptive_value_edit.h
    ui/import/source_preview_widget.cpp
    ui/import/source_preview_widget.h
    ui/import/signal_import_dialog.cpp
    ui/import/signal_import_dialog.h
    ui/parameter_input.h
    ui/spectral_settings_widget.cpp
    ui/spectral_settings_widget.h
    app/main_window.cpp
    app/main_window.h
    ui/narrowband_workspace.cpp
    ui/narrowband_workspace.h
    ui/charts/plot_widget.cpp
    ui/charts/plot_widget.h
    ui/charts/display_sampling.cpp
    ui/charts/display_sampling.h
    ui/charts/accelerated_surface.cpp
    ui/charts/accelerated_surface.h
)
add_executable(SignalStudioDetectionWorker tools/detection_worker/main.cpp)
ss_target_defaults(SignalStudioDetectionWorker)
set_target_properties(SignalStudioDetectionWorker PROPERTIES AUTOMOC OFF)
target_link_libraries(SignalStudioDetectionWorker PRIVATE SignalStudioInfrastructure)
ss_stage_mkl_runtime(SignalStudioDetectionWorker)
include(cmake/detection_model_runtime.cmake)
signalstudio_configure_detection_model_backend(SignalStudioDetectionWorker)
add_executable(SignalStudio WIN32 app/main.cpp ${SS_UI_SOURCES})
add_dependencies(SignalStudio SignalStudioDetectionWorker)
if(WIN32)
    enable_language(RC)
    target_sources(SignalStudio PRIVATE resources/branding/qt/windows/app_icon.rc)
    target_include_directories(SignalStudio PRIVATE resources/branding/qt/windows)
endif()
target_link_libraries(SignalStudio PRIVATE
    SignalStudioCore SignalStudioInfrastructure SignalStudioBrand Qt6::Widgets
    Qt6::GuiPrivate Qt6::OpenGL Threads::Threads)
ss_target_defaults(SignalStudio)
ss_add_graphics_shaders(SignalStudio signalstudio_app_shaders)
ss_add_ui_assets(SignalStudio)
qt_add_resources(SignalStudio signalstudio_narrowband_demo
    PREFIX "/signalstudio/demo"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES "${CMAKE_SOURCE_DIR}/resources/narrowband_demo.iq"
)

if(WIN32 AND SS_DEPLOY_RUNTIME)
    get_filename_component(_ss_qt_bin "${Qt6_DIR}/../../../bin" ABSOLUTE)
    # CMAKE_GENERATOR_INSTANCE contains the actual VS installation selected by
    # the generator; strip optional instance metadata such as ,version=...
    string(REGEX REPLACE ",.*$" "" _ss_vs_installation "${CMAKE_GENERATOR_INSTANCE}")
    find_program(SS_WINDEPLOYQT windeployqt
        HINTS "${_ss_qt_bin}" "${SS_QT_ROOT}/bin" NO_DEFAULT_PATH REQUIRED)
    add_custom_command(TARGET SignalStudio POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DSS_WINDEPLOYQT=${SS_WINDEPLOYQT}"
            "-DSS_QT_BIN=${_ss_qt_bin}"
            "-DSS_EXECUTABLE=$<TARGET_FILE:SignalStudio>"
            "-DSS_CONFIGURATION=$<CONFIG>"
            "-DSS_VS_INSTALLATION_PATH=${_ss_vs_installation}"
            -P "${CMAKE_SOURCE_DIR}/cmake/deploy_runtime.cmake"
        COMMENT "Deploying Qt and compiler runtime to the standalone output directory"
        VERBATIM
    )
    if(SS_GPU_DSP_RUNTIME_ROOT)
        foreach(_ss_gpu_dll IN ITEMS SignalStudioGpuDspCuda.dll cufft64_11.dll nvJitLink_120_0.dll)
            if(NOT EXISTS "${SS_GPU_DSP_RUNTIME_ROOT}/${_ss_gpu_dll}")
                message(FATAL_ERROR "Incomplete optional GPU DSP runtime: ${_ss_gpu_dll}")
            endif()
            add_custom_command(TARGET SignalStudio POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${SS_GPU_DSP_RUNTIME_ROOT}/${_ss_gpu_dll}"
                    "$<TARGET_FILE_DIR:SignalStudio>/${_ss_gpu_dll}"
                VERBATIM)
        endforeach()
    endif()
    ss_stage_mkl_runtime(SignalStudio)
endif()

if(SS_BUILD_TESTS)
    enable_testing()
    add_executable(SignalStudioParameterInputTests tests/parameter_input_tests.cpp)
    target_link_libraries(SignalStudioParameterInputTests PRIVATE Qt6::Widgets Qt6::Test)
    ss_target_defaults(SignalStudioParameterInputTests)
    add_test(NAME signal_studio_parameter_input COMMAND "${CMAKE_COMMAND}"
        "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioParameterInputTests>"
        "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/parameter-input-$<LOWER_CASE:$<CONFIG>>.txt"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
    set_tests_properties(signal_studio_parameter_input PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 60)
    add_executable(SignalStudioFfscnEngineDialogTests
        tests/ffscn_engine_dialog_tests.cpp
        ui/detection/ffscn_engine_dialog.cpp
        ui/detection/ffscn_engine_dialog.h)
    target_link_libraries(SignalStudioFfscnEngineDialogTests PRIVATE
        Qt6::Widgets Qt6::Test)
    ss_target_defaults(SignalStudioFfscnEngineDialogTests)
    add_test(NAME signal_studio_ffscn_engine_dialog COMMAND "${CMAKE_COMMAND}"
        "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioFfscnEngineDialogTests>"
        "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/ffscn-engine-dialog-$<LOWER_CASE:$<CONFIG>>.txt"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
    set_tests_properties(signal_studio_ffscn_engine_dialog PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 60)
    add_executable(SignalStudioDetectionResourceOptionsTests
        tests/detection_resource_options_tests.cpp
        ui/detection/detection_resource_options.cpp
        ui/detection/detection_resource_options.h)
    target_link_libraries(SignalStudioDetectionResourceOptionsTests PRIVATE
        SignalStudioInfrastructure Qt6::Widgets Qt6::Test)
    ss_target_defaults(SignalStudioDetectionResourceOptionsTests)
    add_test(NAME signal_studio_detection_resource_options COMMAND "${CMAKE_COMMAND}"
        "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioDetectionResourceOptionsTests>"
        "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/detection-resource-options-$<LOWER_CASE:$<CONFIG>>.txt"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
    set_tests_properties(signal_studio_detection_resource_options PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 60)
    add_executable(SignalStudioFfscnEngineSwitchTests
        tests/ffscn_engine_switch_tests.cpp)
    target_link_libraries(SignalStudioFfscnEngineSwitchTests PRIVATE
        SignalStudioInfrastructure Qt6::Test)
    ss_target_defaults(SignalStudioFfscnEngineSwitchTests)
    add_test(NAME signal_studio_ffscn_engine_switch
        COMMAND SignalStudioFfscnEngineSwitchTests)
    set_tests_properties(signal_studio_ffscn_engine_switch PROPERTIES TIMEOUT 60)
    add_executable(SignalStudioDetectionRefinedFusionTests tests/detection_refined_fusion_tests.cpp)
    ss_target_defaults(SignalStudioDetectionRefinedFusionTests)
    set_target_properties(SignalStudioDetectionRefinedFusionTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioDetectionRefinedFusionTests PRIVATE SignalStudioCore)
    add_test(NAME signal_studio_detection_refined_fusion COMMAND SignalStudioDetectionRefinedFusionTests)
    add_executable(SignalStudioDetectionTimePipelineTests tests/detection_time_pipeline_tests.cpp)
    ss_stage_mkl_runtime(SignalStudioDetectionTimePipelineTests)
    ss_target_defaults(SignalStudioDetectionTimePipelineTests)
    set_target_properties(SignalStudioDetectionTimePipelineTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioDetectionTimePipelineTests PRIVATE SignalStudioInfrastructure)
    add_test(NAME signal_studio_detection_time_pipeline COMMAND SignalStudioDetectionTimePipelineTests)
    add_executable(SignalStudioDetectionRefinementTests tests/detection_refinement_tests.cpp)
    ss_target_defaults(SignalStudioDetectionRefinementTests)
    set_target_properties(SignalStudioDetectionRefinementTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioDetectionRefinementTests PRIVATE SignalStudioCore)
    add_test(NAME signal_studio_detection_refinement COMMAND SignalStudioDetectionRefinementTests)
    add_executable(SignalStudioDetectionSampleEnergyTests tests/detection_sample_energy_tests.cpp)
    ss_stage_mkl_runtime(SignalStudioDetectionSampleEnergyTests)
    ss_target_defaults(SignalStudioDetectionSampleEnergyTests)
    set_target_properties(SignalStudioDetectionSampleEnergyTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioDetectionSampleEnergyTests PRIVATE SignalStudioInfrastructure)
    add_test(NAME signal_studio_detection_sample_energy COMMAND SignalStudioDetectionSampleEnergyTests)
    add_executable(SignalStudioSpeedEffectBenchmark tools/speed_effect_benchmark/main.cpp)
    ss_stage_mkl_runtime(SignalStudioSpeedEffectBenchmark)
    ss_target_defaults(SignalStudioSpeedEffectBenchmark)
    set_target_properties(SignalStudioSpeedEffectBenchmark PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioSpeedEffectBenchmark PRIVATE SignalStudioInfrastructure)
    add_executable(SignalStudioDetectionSampleStreamTests tests/detection_sample_stream_tests.cpp)
    ss_stage_mkl_runtime(SignalStudioDetectionSampleStreamTests)
    ss_target_defaults(SignalStudioDetectionSampleStreamTests)
    set_target_properties(SignalStudioDetectionSampleStreamTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioDetectionSampleStreamTests PRIVATE SignalStudioInfrastructure)
    add_test(NAME signal_studio_detection_sample_stream COMMAND SignalStudioDetectionSampleStreamTests --golden --long --run-budget)
    set_tests_properties(signal_studio_detection_sample_stream PROPERTIES TIMEOUT 180)
    add_executable(SignalStudioDetectionSampleRecoveryTests tests/detection_sample_stream_recovery_tests.cpp)
    ss_stage_mkl_runtime(SignalStudioDetectionSampleRecoveryTests)
    ss_target_defaults(SignalStudioDetectionSampleRecoveryTests)
    set_target_properties(SignalStudioDetectionSampleRecoveryTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioDetectionSampleRecoveryTests PRIVATE SignalStudioInfrastructure)
    add_test(NAME signal_studio_detection_sample_recovery COMMAND SignalStudioDetectionSampleRecoveryTests)
    set_tests_properties(signal_studio_detection_sample_recovery PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_executable(SignalStudioDetectionPipelineTests tests/detection_pipeline_tests.cpp)
    ss_target_defaults(SignalStudioDetectionPipelineTests)
    set_target_properties(SignalStudioDetectionPipelineTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioDetectionPipelineTests PRIVATE SignalStudioInfrastructure)
    add_test(NAME signal_studio_detection_pipeline COMMAND SignalStudioDetectionPipelineTests)
    add_executable(SignalStudioDetectionPostprocessTests tests/detection_postprocess_tests.cpp)
    ss_target_defaults(SignalStudioDetectionPostprocessTests)
    set_target_properties(SignalStudioDetectionPostprocessTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioDetectionPostprocessTests PRIVATE SignalStudioCore)
    add_test(NAME signal_studio_detection_postprocess COMMAND SignalStudioDetectionPostprocessTests)
    add_executable(SignalStudioDetectionModelBackendTests tests/detection_model_backend_tests.cpp)
    ss_target_defaults(SignalStudioDetectionModelBackendTests)
    set_target_properties(SignalStudioDetectionModelBackendTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioDetectionModelBackendTests PRIVATE SignalStudioInfrastructure)
    signalstudio_configure_detection_model_backend(SignalStudioDetectionModelBackendTests)
    add_test(NAME signal_studio_detection_model_backend COMMAND SignalStudioDetectionModelBackendTests)
    add_executable(SignalStudioDetectionFrequencyAnalysisTests tests/detection_frequency_analysis_tests.cpp)
    ss_target_defaults(SignalStudioDetectionFrequencyAnalysisTests)
    set_target_properties(SignalStudioDetectionFrequencyAnalysisTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioDetectionFrequencyAnalysisTests PRIVATE SignalStudioInfrastructure)
    add_test(NAME signal_studio_detection_frequency_analysis COMMAND SignalStudioDetectionFrequencyAnalysisTests)
    foreach(test_kind IN ITEMS Store Protocol Task)
        string(TOLOWER "${test_kind}" test_kind_lower)
        set(test_target "SignalStudioDetection${test_kind}Tests")
        add_executable(${test_target} "tests/detection_${test_kind_lower}_tests.cpp")
        ss_target_defaults(${test_target})
        set_target_properties(${test_target} PROPERTIES AUTOMOC OFF)
        target_link_libraries(${test_target} PRIVATE SignalStudioInfrastructure)
        add_test(NAME "signal_studio_detection_${test_kind_lower}" COMMAND ${test_target})
    endforeach()
    add_dependencies(SignalStudioDetectionTaskTests SignalStudio)
    add_executable(SignalStudioSsDeRpoLogicTests tests/ss_de_rpo_logic_tests.cpp)
    ss_target_defaults(SignalStudioSsDeRpoLogicTests)
    set_target_properties(SignalStudioSsDeRpoLogicTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioSsDeRpoLogicTests PRIVATE SignalStudioInfrastructure)
    add_test(NAME signal_studio_ss_de_rpo_logic COMMAND SignalStudioSsDeRpoLogicTests)
    set_tests_properties(signal_studio_ss_de_rpo_logic PROPERTIES TIMEOUT 60)
    add_executable(SignalStudioSsDeRpoResourceTests tests/ss_de_rpo_resource_tests.cpp)
    ss_target_defaults(SignalStudioSsDeRpoResourceTests)
    set_target_properties(SignalStudioSsDeRpoResourceTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioSsDeRpoResourceTests PRIVATE SignalStudioInfrastructure)
    add_test(NAME signal_studio_ss_de_rpo_resource COMMAND SignalStudioSsDeRpoResourceTests)
    add_executable(SignalStudioSsDeRpoGpuTests tests/ss_de_rpo_gpu_tests.cpp)
    ss_target_defaults(SignalStudioSsDeRpoGpuTests)
    set_target_properties(SignalStudioSsDeRpoGpuTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioSsDeRpoGpuTests PRIVATE SignalStudioInfrastructure)
    add_test(NAME signal_studio_ss_de_rpo_gpu COMMAND SignalStudioSsDeRpoGpuTests)
    add_executable(SignalStudioSsDeRpoUiTests tests/ss_de_rpo_ui_tests.cpp ${SS_UI_SOURCES})
    target_link_libraries(SignalStudioSsDeRpoUiTests PRIVATE
        SignalStudioCore SignalStudioInfrastructure SignalStudioBrand Qt6::Widgets Qt6::Test
        Qt6::GuiPrivate Qt6::OpenGL Threads::Threads)
    ss_target_defaults(SignalStudioSsDeRpoUiTests)
    target_compile_definitions(SignalStudioSsDeRpoUiTests PRIVATE SS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    ss_add_graphics_shaders(SignalStudioSsDeRpoUiTests signalstudio_ss_de_rpo_ui_shaders)
    ss_add_ui_assets(SignalStudioSsDeRpoUiTests)
    add_dependencies(SignalStudioSsDeRpoUiTests SignalStudio)
    add_test(NAME signal_studio_ss_de_rpo_ui COMMAND SignalStudioSsDeRpoUiTests)
    set_tests_properties(signal_studio_ss_de_rpo_ui PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 120)
    add_executable(SignalStudioDetectionSourceTests tests/detection_source_tests.cpp)
    ss_stage_mkl_runtime(SignalStudioDetectionSourceTests)
    add_executable(SignalStudioDetectionBenchmarkQualityTests tests/detection_benchmark_quality_tests.cpp)
    ss_target_defaults(SignalStudioDetectionBenchmarkQualityTests)
    set_target_properties(SignalStudioDetectionBenchmarkQualityTests PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioDetectionBenchmarkQualityTests PRIVATE SignalStudioInfrastructure)
    add_test(NAME signal_studio_detection_benchmark_quality COMMAND SignalStudioDetectionBenchmarkQualityTests)
    ss_target_defaults(SignalStudioDetectionSourceTests)
    target_link_libraries(SignalStudioDetectionSourceTests PRIVATE SignalStudioInfrastructure Qt6::Test)
    target_compile_definitions(SignalStudioDetectionSourceTests PRIVATE SS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME signal_studio_detection_source COMMAND "${CMAKE_COMMAND}"
        "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioDetectionSourceTests>"
        "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/detection-source-test-results-$<LOWER_CASE:$<CONFIG>>.txt"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
    add_executable(SignalStudioAnnotationTests tests/annotation_tests.cpp)
    target_link_libraries(SignalStudioAnnotationTests PRIVATE SignalStudioCore)
    set_target_properties(SignalStudioAnnotationTests PROPERTIES AUTOMOC OFF)
    ss_target_defaults(SignalStudioAnnotationTests)
    add_test(NAME signal_studio_annotations COMMAND SignalStudioAnnotationTests)
    add_executable(SignalStudioUxR5AnnotationTests tests/ux_r5_annotation_tests.cpp)
    target_link_libraries(SignalStudioUxR5AnnotationTests PRIVATE SignalStudioCore)
    set_target_properties(SignalStudioUxR5AnnotationTests PROPERTIES AUTOMOC OFF)
    ss_target_defaults(SignalStudioUxR5AnnotationTests)
    add_test(NAME signal_studio_ux_r5_annotations COMMAND SignalStudioUxR5AnnotationTests)
    set_tests_properties(signal_studio_ux_r5_annotations PROPERTIES TIMEOUT 60)
    add_executable(SignalStudioUxR5CompatTests tests/ux_r5_compat_tests.cpp)
    target_link_libraries(SignalStudioUxR5CompatTests PRIVATE SignalStudioInfrastructure Qt6::Test)
    ss_target_defaults(SignalStudioUxR5CompatTests)
    add_test(NAME signal_studio_ux_r5_compat COMMAND "${CMAKE_COMMAND}"
        "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioUxR5CompatTests>"
        "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/ux-r5-compat-results-$<LOWER_CASE:$<CONFIG>>.txt"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
    set_tests_properties(signal_studio_ux_r5_compat PROPERTIES TIMEOUT 120)
    add_executable(SignalStudioUxR5DetectionTests tests/ux_r5_detection_tests.cpp)
    target_link_libraries(SignalStudioUxR5DetectionTests PRIVATE SignalStudioInfrastructure Qt6::Core)
    set_target_properties(SignalStudioUxR5DetectionTests PROPERTIES AUTOMOC OFF)
    ss_target_defaults(SignalStudioUxR5DetectionTests)
    add_test(NAME signal_studio_ux_r5_detection COMMAND SignalStudioUxR5DetectionTests)
    set_tests_properties(signal_studio_ux_r5_detection PROPERTIES TIMEOUT 120)
    add_executable(SignalStudioAnnotationStoreTests tests/annotation_store_tests.cpp)
    target_link_libraries(SignalStudioAnnotationStoreTests PRIVATE SignalStudioInfrastructure Qt6::Core)
    set_target_properties(SignalStudioAnnotationStoreTests PROPERTIES AUTOMOC OFF)
    ss_target_defaults(SignalStudioAnnotationStoreTests)
    add_test(NAME signal_studio_annotation_store COMMAND SignalStudioAnnotationStoreTests)
    add_executable(SignalStudioLabelImportTests tests/label_import_tests.cpp)
    target_link_libraries(SignalStudioLabelImportTests PRIVATE SignalStudioInfrastructure Qt6::Test)
    target_compile_definitions(SignalStudioLabelImportTests PRIVATE SS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    ss_target_defaults(SignalStudioLabelImportTests)
    add_test(NAME signal_studio_label_import COMMAND "${CMAKE_COMMAND}"
        "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioLabelImportTests>"
        "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/label-test-results-$<LOWER_CASE:$<CONFIG>>.txt"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
    add_executable(SignalStudioWavTests tests/wav_input_tests.cpp)
    target_link_libraries(SignalStudioWavTests PRIVATE SignalStudioInfrastructure Qt6::Test)
    target_compile_definitions(SignalStudioWavTests PRIVATE SS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    ss_target_defaults(SignalStudioWavTests)
    add_test(NAME signal_studio_wav COMMAND "${CMAKE_COMMAND}"
        "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioWavTests>"
        "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/wav-test-results-$<LOWER_CASE:$<CONFIG>>.txt"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
    add_executable(SignalStudioImportTests tests/import_controller_tests.cpp)
    target_link_libraries(SignalStudioImportTests PRIVATE SignalStudioInfrastructure Qt6::Test)
    ss_target_defaults(SignalStudioImportTests)
    add_test(NAME signal_studio_import COMMAND SignalStudioImportTests)
    set_tests_properties(signal_studio_import PROPERTIES TIMEOUT 120)
    add_executable(SignalStudioSampleFormatTests tests/sample_format_tests.cpp)
    target_link_libraries(SignalStudioSampleFormatTests PRIVATE SignalStudioInfrastructure Qt6::Core)
    ss_target_defaults(SignalStudioSampleFormatTests)
    add_test(NAME signal_studio_sample_format COMMAND SignalStudioSampleFormatTests)
    add_executable(SignalStudioCoreTests tests/core_tests.cpp)
    target_link_libraries(SignalStudioCoreTests PRIVATE
        SignalStudioCore SignalStudioInfrastructure Qt6::Core)
    set_target_properties(SignalStudioCoreTests PROPERTIES AUTOMOC OFF)
    ss_target_defaults(SignalStudioCoreTests)
    add_test(NAME signal_studio_core COMMAND SignalStudioCoreTests)

    add_executable(SignalStudioProjectFileTests tests/project_file_tests.cpp)
    target_link_libraries(SignalStudioProjectFileTests PRIVATE SignalStudioInfrastructure Qt6::Core)
    set_target_properties(SignalStudioProjectFileTests PROPERTIES AUTOMOC OFF)
    ss_target_defaults(SignalStudioProjectFileTests)
    add_dependencies(SignalStudioProjectFileTests SignalStudio)
    add_test(NAME signal_studio_project_file COMMAND SignalStudioProjectFileTests)
    set_tests_properties(signal_studio_project_file PROPERTIES TIMEOUT 120)

    add_executable(SignalStudioUxR3AxisTests tests/ux_r3_axis_tests.cpp ${SS_UI_SOURCES})
    target_link_libraries(SignalStudioUxR3AxisTests PRIVATE SignalStudioInfrastructure SignalStudioBrand
        Qt6::Widgets Qt6::Test Qt6::GuiPrivate Qt6::OpenGL Threads::Threads)
    ss_target_defaults(SignalStudioUxR3AxisTests)
    ss_add_graphics_shaders(SignalStudioUxR3AxisTests signalstudio_ux_r3_axis_shaders)
    ss_add_ui_assets(SignalStudioUxR3AxisTests)
    add_dependencies(SignalStudioUxR3AxisTests SignalStudio)
    add_test(NAME signal_studio_ux_r3_axes COMMAND "${CMAKE_COMMAND}"
        "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioUxR3AxisTests>"
        "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/ux-r3-axes-$<LOWER_CASE:$<CONFIG>>.txt"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
    set_tests_properties(signal_studio_ux_r3_axes PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 120)

    add_executable(SignalStudioUxR3WorkflowTests tests/ux_r3_workflow_tests.cpp ${SS_UI_SOURCES})
    target_link_libraries(SignalStudioUxR3WorkflowTests PRIVATE SignalStudioInfrastructure SignalStudioBrand
        Qt6::Widgets Qt6::Test Qt6::GuiPrivate Qt6::OpenGL Threads::Threads)
    ss_target_defaults(SignalStudioUxR3WorkflowTests)
    ss_add_graphics_shaders(SignalStudioUxR3WorkflowTests signalstudio_ux_r3_workflow_shaders)
    ss_add_ui_assets(SignalStudioUxR3WorkflowTests)
    add_dependencies(SignalStudioUxR3WorkflowTests SignalStudio)
    add_test(NAME signal_studio_ux_r3_workflows COMMAND "${CMAKE_COMMAND}"
        "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioUxR3WorkflowTests>"
        "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/ux-r3-workflows-$<LOWER_CASE:$<CONFIG>>.txt"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
    set_tests_properties(signal_studio_ux_r3_workflows PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 180)
    add_executable(SignalStudioChannelProcessorTests tests/channel_processor_tests.cpp)
    add_executable(SignalStudioNarrowbandAnalysisTests tests/narrowband_analysis_tests.cpp)
    add_executable(SignalStudioNarrowbandRecoveryTests tests/narrowband_recovery_tests.cpp)
    add_executable(SignalStudioNarrowbandModulationTests tests/narrowband_modulation_tests.cpp)
    add_executable(SignalStudioNarrowbandFeedbackProbe tests/narrowband_feedback_probe.cpp)
    target_link_libraries(SignalStudioNarrowbandFeedbackProbe PRIVATE SignalStudioInfrastructure Qt6::Core)
    ss_target_defaults(SignalStudioNarrowbandFeedbackProbe)
    set_target_properties(SignalStudioNarrowbandFeedbackProbe PROPERTIES AUTOMOC OFF)
    target_link_libraries(SignalStudioNarrowbandModulationTests PRIVATE SignalStudioInfrastructure Qt6::Core)
    ss_target_defaults(SignalStudioNarrowbandModulationTests)
    set_target_properties(SignalStudioNarrowbandModulationTests PROPERTIES AUTOMOC OFF)
    add_test(NAME signal_studio_narrowband_modulation COMMAND SignalStudioNarrowbandModulationTests)
    set_tests_properties(signal_studio_narrowband_modulation PROPERTIES TIMEOUT 300)
    target_link_libraries(SignalStudioNarrowbandRecoveryTests PRIVATE SignalStudioInfrastructure Qt6::Core)
    ss_target_defaults(SignalStudioNarrowbandRecoveryTests)
    set_target_properties(SignalStudioNarrowbandRecoveryTests PROPERTIES AUTOMOC OFF)
    add_test(NAME signal_studio_narrowband_recovery COMMAND SignalStudioNarrowbandRecoveryTests)
    set_tests_properties(signal_studio_narrowband_recovery PROPERTIES TIMEOUT 180)
    target_link_libraries(SignalStudioNarrowbandAnalysisTests PRIVATE SignalStudioInfrastructure Qt6::Core)
    ss_target_defaults(SignalStudioNarrowbandAnalysisTests)
    set_target_properties(SignalStudioNarrowbandAnalysisTests PROPERTIES AUTOMOC OFF)
    add_test(NAME signal_studio_narrowband_analysis COMMAND SignalStudioNarrowbandAnalysisTests)
    set_tests_properties(signal_studio_narrowband_analysis PROPERTIES TIMEOUT 120)
    add_executable(SignalStudioChannelRateTests tests/channel_rate_tests.cpp)
    target_link_libraries(SignalStudioChannelRateTests PRIVATE SignalStudioInfrastructure Qt6::Core)
    set_target_properties(SignalStudioChannelRateTests PROPERTIES AUTOMOC OFF)
    ss_target_defaults(SignalStudioChannelRateTests)
    add_test(NAME signal_studio_channel_rate COMMAND SignalStudioChannelRateTests)
    set_tests_properties(signal_studio_channel_rate PROPERTIES TIMEOUT 120)
    target_link_libraries(SignalStudioChannelProcessorTests PRIVATE
        SignalStudioInfrastructure SignalStudioCore Qt6::Core)
    set_target_properties(SignalStudioChannelProcessorTests PROPERTIES AUTOMOC OFF)
    ss_target_defaults(SignalStudioChannelProcessorTests)
    add_test(NAME signal_studio_channel_processor COMMAND SignalStudioChannelProcessorTests)
    add_executable(SignalStudioSpectralAnalysisTests tests/spectral_analysis_tests.cpp)
    target_link_libraries(SignalStudioSpectralAnalysisTests PRIVATE SignalStudioInfrastructure Qt6::Core)
    set_target_properties(SignalStudioSpectralAnalysisTests PROPERTIES AUTOMOC OFF)
    ss_target_defaults(SignalStudioSpectralAnalysisTests)
    add_test(NAME signal_studio_spectral_analysis COMMAND SignalStudioSpectralAnalysisTests)
    set_tests_properties(signal_studio_spectral_analysis PROPERTIES TIMEOUT 120)
    set_tests_properties(signal_studio_channel_processor PROPERTIES TIMEOUT 120)
    add_executable(SignalStudioDisplaySamplingTests
        tests/display_sampling_tests.cpp ui/charts/display_sampling.cpp)
    set_target_properties(SignalStudioDisplaySamplingTests PROPERTIES AUTOMOC OFF)
    ss_target_defaults(SignalStudioDisplaySamplingTests)
    add_test(NAME signal_studio_display_sampling COMMAND SignalStudioDisplaySamplingTests)
    set_tests_properties(signal_studio_display_sampling PROPERTIES TIMEOUT 60)

    add_executable(SignalStudioUiTests tests/ui_tests.cpp ${SS_UI_SOURCES})
    add_executable(SignalStudioNarrowbandAnalysisUiTests tests/narrowband_analysis_ui_tests.cpp ${SS_UI_SOURCES})
    target_link_libraries(SignalStudioNarrowbandAnalysisUiTests PRIVATE
        SignalStudioCore SignalStudioInfrastructure SignalStudioBrand Qt6::Widgets Qt6::Test
        Qt6::GuiPrivate Qt6::OpenGL Threads::Threads)
    ss_target_defaults(SignalStudioNarrowbandAnalysisUiTests)
    ss_add_graphics_shaders(SignalStudioNarrowbandAnalysisUiTests signalstudio_nb_analysis_ui_shaders)
    ss_add_ui_assets(SignalStudioNarrowbandAnalysisUiTests)
    qt_add_resources(SignalStudioNarrowbandAnalysisUiTests signalstudio_nb_analysis_demo
        PREFIX "/signalstudio/demo" BASE "${CMAKE_SOURCE_DIR}/resources"
        FILES "${CMAKE_SOURCE_DIR}/resources/narrowband_demo.iq")
    add_dependencies(SignalStudioNarrowbandAnalysisUiTests SignalStudio)
    add_test(NAME signal_studio_narrowband_analysis_ui COMMAND "${CMAKE_COMMAND}"
        "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioNarrowbandAnalysisUiTests>"
        "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/nb-analysis-ui-$<LOWER_CASE:$<CONFIG>>.txt"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
    set_tests_properties(signal_studio_narrowband_analysis_ui PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 480)
    target_link_libraries(SignalStudioUiTests PRIVATE
        SignalStudioCore SignalStudioInfrastructure SignalStudioBrand Qt6::Widgets Qt6::Test
        Qt6::GuiPrivate Qt6::OpenGL Threads::Threads)
    ss_target_defaults(SignalStudioUiTests)
    target_compile_definitions(SignalStudioUiTests PRIVATE SS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    ss_add_graphics_shaders(SignalStudioUiTests signalstudio_ui_test_shaders)
    ss_add_ui_assets(SignalStudioUiTests)
    qt_add_resources(SignalStudioUiTests signalstudio_ui_test_narrowband_demo
        PREFIX "/signalstudio/demo"
        BASE "${CMAKE_SOURCE_DIR}/resources"
        FILES "${CMAKE_SOURCE_DIR}/resources/narrowband_demo.iq"
    )
    add_executable(SignalStudioProjectTreeTests tests/project_tree_presentation_tests.cpp)
    target_link_libraries(SignalStudioProjectTreeTests PRIVATE SignalStudioCore Qt6::Widgets)
    set_target_properties(SignalStudioProjectTreeTests PROPERTIES AUTOMOC OFF)
    ss_target_defaults(SignalStudioProjectTreeTests)
    add_dependencies(SignalStudioProjectTreeTests SignalStudio)
    add_test(NAME signal_studio_project_tree COMMAND SignalStudioProjectTreeTests)
    set_tests_properties(signal_studio_project_tree PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 60
        PASS_REGULAR_EXPRESSION "Totals: 8 passed, 0 failed")

    # Dedicated behavioral coverage for the wideband UI reorganization.
    add_executable(SignalStudioUxTests tests/ux_tests.cpp ${SS_UI_SOURCES})
    target_link_libraries(SignalStudioUxTests PRIVATE
        SignalStudioCore SignalStudioInfrastructure SignalStudioBrand Qt6::Widgets Qt6::Test
        Qt6::GuiPrivate Qt6::OpenGL Threads::Threads)
    ss_target_defaults(SignalStudioUxTests)
    target_compile_definitions(SignalStudioUxTests PRIVATE SS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    ss_add_graphics_shaders(SignalStudioUxTests signalstudio_ux_test_shaders)
    ss_add_ui_assets(SignalStudioUxTests)
    qt_add_resources(SignalStudioUxTests signalstudio_ux_test_narrowband_demo
        PREFIX "/signalstudio/demo"
        BASE "${CMAKE_SOURCE_DIR}/resources"
        FILES "${CMAKE_SOURCE_DIR}/resources/narrowband_demo.iq")
    function(ss_add_ux_test category)
        add_test(NAME signal_studio_ux_${category} COMMAND "${CMAKE_COMMAND}"
            "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioUxTests>"
            "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/ux-${category}-test-results-$<LOWER_CASE:$<CONFIG>>.txt"
            "-DSS_UI_CASES=${ARGN}"
            -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
        set_tests_properties(signal_studio_ux_${category} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 480
            LABELS "wideband-ux;${category}")
    endfunction()
    ss_add_ux_test(detection uxD01_historicalDerivation uxD02_viewSourceKeepsDraft
        uxD01_topDerivationDiscardsCrossFileDraft
        uxD03_crossFileCancellation uxD03_activeTaskCancellationAndFrozenDisplay
        uxD09_intermediatePreviewAndRedetectClear
        uxD04_modelSpectrumMapping uxD05_foldedParametersAndFirstScreen
        uxD06_visibleRangeEntry uxD08_preflightDetails)
    ss_add_ux_test(r6_preview uxR6_previewChannelScope)
    ss_add_ux_test(spectral uxS01_methodHistory uxS02_conditionalRows
        uxS03_spectralIndependence uxS03_framePsdPreservesSettings uxS06_invalidDraftRecovery)
    ss_add_ux_test(charts uxR02_chartSettingsStayStable uxA01_roiConfirmationAndEscape roundTwoNarrowbandPsdAndPages)
    ss_add_ux_test(annotation uxR4_evidenceDisclosureAndFullCopy
        uxR4_currentObjectAndActionStates uxR4_supplementalSourceAndPagedCopy
        uxR4_auxiliaryDetailsCloseWithHiddenContext uxR4_searchBoundaryAndOriginalNewlines)
    ss_add_ux_test(r5_ui uxR5_threeTabsReadOnlyAndConversion
        uxR5_layersAndIndependentFilters uxR5_crossViewAndUnknownObservation
        uxR5_readOnlyMenusAndCommandGuards
        uxR5_algorithmResultHideAndDelete)
    ss_add_ux_test(layout uxR01_chartToolbarLayout uxR02_inspectorRouting
        uxR02_fileProperties uxR02_paletteWorkspaceOwnership
        uxR03_taskKeyboardAndDetails uxR03_taskRowActions uxR01_lateRunFilterPopupAndRefresh roundTwoFlatResponsiveControls)
    ss_add_ux_test(popovers uxP01_popupDismissal uxP02_popupChildrenAndGeometry
        uxS06_popoverDraftPolicy)

    add_executable(SignalStudioUiCapture tests/ui_capture.cpp ${SS_UI_SOURCES})
    target_link_libraries(SignalStudioUiCapture PRIVATE
        SignalStudioCore SignalStudioInfrastructure SignalStudioBrand Qt6::Widgets Qt6::Test
        Qt6::GuiPrivate Qt6::OpenGL Threads::Threads)
    ss_target_defaults(SignalStudioUiCapture)
    target_compile_definitions(SignalStudioUiCapture PRIVATE SS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    ss_add_graphics_shaders(SignalStudioUiCapture signalstudio_ui_capture_shaders)
    ss_add_ui_assets(SignalStudioUiCapture)
    qt_add_resources(SignalStudioUiCapture signalstudio_ui_capture_narrowband_demo
        PREFIX "/signalstudio/demo"
        BASE "${CMAKE_SOURCE_DIR}/resources"
        FILES "${CMAKE_SOURCE_DIR}/resources/narrowband_demo.iq"
    )
    # Keep every original function/data row, with bounded independent groups.
    # The frozen Qt -datatags inventory is checked again at delivery.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/ui_tests.cpp")
    file(READ "${CMAKE_SOURCE_DIR}/tests/ui_tests.cpp" ss_ui_source)
    string(REPLACE "\r" "" ss_ui_source "${ss_ui_source}")
    string(FIND "${ss_ui_source}" "  void workspaceLayout();" ss_ui_begin)
    string(FIND "${ss_ui_source}" "\n};\n\nvoid UiTests::workspaceLayout()" ss_ui_end)
    if(ss_ui_begin LESS 0 OR ss_ui_end LESS ss_ui_begin)
        message(FATAL_ERROR "Cannot identify the complete main UI slot declarations")
    endif()
    math(EXPR ss_ui_length "${ss_ui_end}-${ss_ui_begin}")
    string(SUBSTRING "${ss_ui_source}" ${ss_ui_begin} ${ss_ui_length} ss_ui_declarations)
    string(REGEX MATCHALL "void [A-Za-z_][A-Za-z0-9_]*\\(\\)" ss_ui_slots "${ss_ui_declarations}")
    set(ss_ui_group geometry)
    foreach(ss_ui_slot IN LISTS ss_ui_slots)
        string(REGEX REPLACE "void ([A-Za-z_][A-Za-z0-9_]*)\\(\\)" "\\1" ss_ui_case "${ss_ui_slot}")
        if(ss_ui_case MATCHES "_data$")
            continue()
        endif()
        if(ss_ui_case STREQUAL "powerInputsCommitAndFitReusesData")
            set(ss_ui_group parameters)
        elseif(ss_ui_case STREQUAL "unifiedAnnotationDraftsAndSegmentSwitch")
            set(ss_ui_group annotations)
        endif()
        list(APPEND "SS_MAIN_UI_${ss_ui_group}" "${ss_ui_case}")
    endforeach()
    foreach(ss_ui_group IN ITEMS geometry parameters annotations)
        set(ss_ui_test "signal_studio_ui_${ss_ui_group}")
        if(ss_ui_group STREQUAL "geometry")
            set(ss_ui_test signal_studio_ui)
        endif()
        add_test(NAME ${ss_ui_test} COMMAND "${CMAKE_COMMAND}"
            "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioUiTests>"
            "-DSS_UI_CASES=${SS_MAIN_UI_${ss_ui_group}}"
            "-DSS_UI_ALLOW_OVERRIDE=ON"
            "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/ui-${ss_ui_group}-test-results-$<LOWER_CASE:$<CONFIG>>.txt"
            -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
        set_tests_properties(${ss_ui_test} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 480 LABELS "ui;${ss_ui_group}")
    endforeach()
    set_tests_properties(signal_studio_core PROPERTIES TIMEOUT 60)

    if(WIN32 AND SS_DEPLOY_RUNTIME)
        # Stage the shared Qt runtime once, before either test binary is built.
        add_dependencies(SignalStudioCoreTests SignalStudio)
        add_dependencies(SignalStudioSpectralAnalysisTests SignalStudio)
        add_dependencies(SignalStudioUiTests SignalStudio)
        add_dependencies(SignalStudioUiCapture SignalStudio)
        foreach(test_target IN ITEMS SignalStudioDetectionSourceTests SignalStudioLabelImportTests SignalStudioWavTests
                SignalStudioImportTests SignalStudioUiTests SignalStudioUxTests SignalStudioUiCapture)
            add_dependencies(${test_target} SignalStudio)
            add_custom_command(TARGET ${test_target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:Qt6::Test>" "$<TARGET_FILE_DIR:${test_target}>"
                VERBATIM)
        endforeach()
    endif()
endif()
