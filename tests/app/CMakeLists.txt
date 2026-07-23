add_executable(signal_studio_app_tests application_tests.cpp)
target_link_libraries(signal_studio_app_tests PRIVATE SignalStudio::AppCore)
signal_studio_enable_warnings(signal_studio_app_tests)
signal_studio_deploy_msvc_runtime(signal_studio_app_tests)

foreach(_case IN ITEMS
    filename-parse
    import-wav
    import-sc16-override
    import-sc16-no-sr-fails
    analyze-psd
    external-wav
    external-sc16
    wideband-narrowband-link
    narrowband-extraction)
  add_test(NAME "app.${_case}" COMMAND signal_studio_app_tests --case "${_case}")
  set_tests_properties("app.${_case}" PROPERTIES TIMEOUT 60 LABELS "unit;contract;ms-04;ms-05;app"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endforeach()

# Headless application self-test: the real signal_studio.exe with --self-test runs the Qt-free
# import/read path against the bundled minimal fixture. Only registered in UI builds (the exe is
# not built headless). Uses the offscreen platform as a safety net even though the self-test path
# never constructs a QApplication.
if(TARGET signal_studio)
  add_test(NAME "app.self-test" COMMAND signal_studio --self-test)
  set_tests_properties("app.self-test" PROPERTIES TIMEOUT 60 LABELS "unit;contract;ms-04;app"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
