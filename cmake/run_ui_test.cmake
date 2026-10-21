if(NOT DEFINED SS_UI_TEST OR NOT DEFINED SS_UI_LOG)
    message(FATAL_ERROR "SS_UI_TEST and SS_UI_LOG are required")
endif()
# The Windows Qt GUI entry point may have no stdout console. Preserve QtTest's
# explicit file logger and surface its actual diagnostics through CTest.
file(REMOVE "${SS_UI_LOG}")
file(REMOVE "${SS_UI_LOG}.timing.jsonl")
set(ENV{SS_UI_TIMING_LOG} "${SS_UI_LOG}.timing.jsonl")
execute_process(COMMAND "${SS_UI_TEST}" ${SS_UI_CASES} -o "${SS_UI_LOG},txt"
    -o "${SS_UI_LOG}.xml,junitxml"
    RESULT_VARIABLE ui_result)
if(EXISTS "${SS_UI_LOG}")
    file(READ "${SS_UI_LOG}" ui_output)
    message("${ui_output}")
    if(ui_output MATCHES "(^|[\r\n])(FAIL!|XPASS|QFATAL)[ \t]*:" OR
       NOT ui_output MATCHES "Totals: [0-9]+ passed, 0 failed")
        message(FATAL_ERROR "Qt UI raw log contains a failure or no successful totals")
    endif()
else()
    message(FATAL_ERROR "Qt UI raw log was not produced")
endif()
if(NOT "${ui_result}" STREQUAL "0")
    message(FATAL_ERROR "Qt UI regression failed: ${ui_result}")
endif()
