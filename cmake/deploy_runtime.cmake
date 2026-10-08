foreach(required SS_WINDEPLOYQT SS_QT_BIN SS_EXECUTABLE SS_CONFIGURATION)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing runtime deployment parameter: ${required}")
    endif()
endforeach()

get_filename_component(output_dir "${SS_EXECUTABLE}" DIRECTORY)
set(ENV{PATH} "${SS_QT_BIN};$ENV{PATH}")
if(DEFINED SS_VS_INSTALLATION_PATH AND IS_DIRECTORY "${SS_VS_INSTALLATION_PATH}/VC")
    # Help windeployqt detect the selected installation. The explicit staging
    # below also covers a newer Visual Studio that windeployqt cannot enumerate.
    set(ENV{VCINSTALLDIR} "${SS_VS_INSTALLATION_PATH}/VC/")
endif()
if(SS_CONFIGURATION STREQUAL "Debug")
    set(configuration_flag --debug)
else()
    set(configuration_flag --release)
endif()
execute_process(
    COMMAND "${SS_WINDEPLOYQT}" "${configuration_flag}" --compiler-runtime
        --no-translations --include-plugins qoffscreen,qminimal
        --dir "${output_dir}" "${SS_EXECUTABLE}"
    RESULT_VARIABLE deployment_result
)
if(NOT deployment_result EQUAL 0)
    message(FATAL_ERROR "windeployqt failed with exit code ${deployment_result}")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/deploy_msvc_runtime.cmake")
file(WRITE "${output_dir}/qt.conf" "[Paths]\nPrefix=.\nPlugins=.\n")
