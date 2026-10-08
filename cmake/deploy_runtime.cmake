foreach(required SS_WINDEPLOYQT SS_QT_BIN SS_EXECUTABLE SS_CONFIGURATION)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing runtime deployment parameter: ${required}")
    endif()
endforeach()

get_filename_component(output_dir "${SS_EXECUTABLE}" DIRECTORY)
set(ENV{PATH} "${SS_QT_BIN};$ENV{PATH}")
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
file(WRITE "${output_dir}/qt.conf" "[Paths]\nPrefix=.\nPlugins=.\n")
