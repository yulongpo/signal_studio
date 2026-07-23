add_executable(signal_studio_visualization_tests visualization_tests.cpp)
target_link_libraries(signal_studio_visualization_tests PRIVATE SignalStudio::Visualization Qt6::Core Qt6::Widgets)
signal_studio_enable_warnings(signal_studio_visualization_tests)
signal_studio_deploy_msvc_runtime(signal_studio_visualization_tests)

# Qt offscreen platform plugin discovery: the test creates QWidgets under QT_QPA_PLATFORM=offscreen,
# so the Qt platforms plugin directory must be on the plugin path.
get_filename_component(_signal_studio_qt_root "${Qt6_DIR}/../../.." ABSOLUTE)
set(_signal_studio_qt_plugin_path "${_signal_studio_qt_root}/plugins")

foreach(_case IN ITEMS
    viewport-window-clamp
    viewport-zoom-pan
    frequency-unit
    format-frequency
    overlay-model
    data-series
    color-map
    view-factory)
  add_test(NAME "visualization.${_case}" COMMAND signal_studio_visualization_tests --case "${_case}")
  set_tests_properties("visualization.${_case}" PROPERTIES TIMEOUT 30 LABELS "unit;contract;ms-03;visualization"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_signal_studio_qt_plugin_path}")
endforeach()
