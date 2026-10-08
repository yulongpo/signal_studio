if(NOT DEFINED SS_UI_TEST OR NOT DEFINED SS_UI_LOG)
    message(FATAL_ERROR "SS_UI_TEST and SS_UI_LOG are required")
endif()
# The Windows Qt GUI entry point may have no stdout console. Preserve QtTest's
# explicit file logger and surface its actual diagnostics through CTest.
file(REMOVE "${SS_UI_LOG}")
execute_process(COMMAND "${SS_UI_TEST}" -o "${SS_UI_LOG},txt"
    RESULT_VARIABLE ui_result)
if(EXISTS "${SS_UI_LOG}")
    file(READ "${SS_UI_LOG}" ui_output)
    message("${ui_output}")
endif()
if(NOT "${ui_result}" STREQUAL "0")
    message(FATAL_ERROR "Qt UI regression failed: ${ui_result}")
endif()
