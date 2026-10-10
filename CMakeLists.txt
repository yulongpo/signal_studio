cmake_minimum_required(VERSION 4.2)
project(SignalStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

option(SS_BUILD_TESTS "Build the state, persistence, and Qt UI tests" ON)
option(SS_DEPLOY_RUNTIME "Deploy the Qt runtime next to the Windows executable" ON)
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
    domain/sample_format.cpp
    domain/project.cpp
    domain/project.h
    application/session.cpp
    application/session.h
)
set_target_properties(SignalStudioCore PROPERTIES AUTOMOC OFF)
ss_target_defaults(SignalStudioCore)

add_library(SignalStudioInfrastructure STATIC
    infrastructure/raw_sample_reader.cpp
    infrastructure/channel_processor.cpp
    infrastructure/channel_processor.h
    infrastructure/spectral_analysis.cpp
    infrastructure/spectral_windows.cpp
    infrastructure/source_loader.cpp
    infrastructure/spectral_analysis.h
    infrastructure/int16_iq_file.cpp
    infrastructure/int16_iq_file.h
    infrastructure/project_store.cpp
    infrastructure/project_store.h
)
set_target_properties(SignalStudioInfrastructure PROPERTIES AUTOMOC OFF)
target_link_libraries(SignalStudioInfrastructure PUBLIC SignalStudioCore Qt6::Core)
ss_target_defaults(SignalStudioInfrastructure)

set(SS_UI_SOURCES
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
add_executable(SignalStudio WIN32 app/main.cpp ${SS_UI_SOURCES})
target_link_libraries(SignalStudio PRIVATE
    SignalStudioCore SignalStudioInfrastructure Qt6::Widgets
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
endif()

if(SS_BUILD_TESTS)
    enable_testing()
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
    add_executable(SignalStudioChannelProcessorTests tests/channel_processor_tests.cpp)
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
    target_link_libraries(SignalStudioUiTests PRIVATE
        SignalStudioCore SignalStudioInfrastructure Qt6::Widgets Qt6::Test
        Qt6::GuiPrivate Qt6::OpenGL Threads::Threads)
    ss_target_defaults(SignalStudioUiTests)
    ss_add_graphics_shaders(SignalStudioUiTests signalstudio_ui_test_shaders)
    ss_add_ui_assets(SignalStudioUiTests)
    qt_add_resources(SignalStudioUiTests signalstudio_ui_test_narrowband_demo
        PREFIX "/signalstudio/demo"
        BASE "${CMAKE_SOURCE_DIR}/resources"
        FILES "${CMAKE_SOURCE_DIR}/resources/narrowband_demo.iq"
    )
    add_executable(SignalStudioUiCapture tests/ui_capture.cpp ${SS_UI_SOURCES})
    target_link_libraries(SignalStudioUiCapture PRIVATE
        SignalStudioCore SignalStudioInfrastructure Qt6::Widgets Qt6::Test
        Qt6::GuiPrivate Qt6::OpenGL Threads::Threads)
    ss_target_defaults(SignalStudioUiCapture)
    ss_add_graphics_shaders(SignalStudioUiCapture signalstudio_ui_capture_shaders)
    ss_add_ui_assets(SignalStudioUiCapture)
    qt_add_resources(SignalStudioUiCapture signalstudio_ui_capture_narrowband_demo
        PREFIX "/signalstudio/demo"
        BASE "${CMAKE_SOURCE_DIR}/resources"
        FILES "${CMAKE_SOURCE_DIR}/resources/narrowband_demo.iq"
    )
    add_test(NAME signal_studio_ui COMMAND "${CMAKE_COMMAND}"
        "-DSS_UI_TEST=$<TARGET_FILE:SignalStudioUiTests>"
        "-DSS_UI_LOG=${CMAKE_BINARY_DIR}/ui-test-results-$<LOWER_CASE:$<CONFIG>>.txt"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_ui_test.cmake")
    set_tests_properties(signal_studio_ui PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 300)
    set_tests_properties(signal_studio_core PROPERTIES TIMEOUT 60)

    if(WIN32 AND SS_DEPLOY_RUNTIME)
        # Stage the shared Qt runtime once, before either test binary is built.
        add_dependencies(SignalStudioCoreTests SignalStudio)
        add_dependencies(SignalStudioSpectralAnalysisTests SignalStudio)
        add_dependencies(SignalStudioUiTests SignalStudio)
        add_dependencies(SignalStudioUiCapture SignalStudio)
        add_custom_command(TARGET SignalStudioUiTests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:Qt6::Test>" "$<TARGET_FILE_DIR:SignalStudioUiTests>"
            VERBATIM
        )
        add_custom_command(TARGET SignalStudioUiCapture POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:Qt6::Test>" "$<TARGET_FILE_DIR:SignalStudioUiCapture>"
            VERBATIM)
    endif()
endif()
