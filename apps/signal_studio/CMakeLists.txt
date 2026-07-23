# Qt-free application core: composes Data/DSP/Compute/TaskRuntime/Core. Shared by the GUI
# executable and the headless self-test so import/analyze logic is testable without QApplication.
add_library(signal_studio_app_core STATIC application.cpp)
add_library(SignalStudio::AppCore ALIAS signal_studio_app_core)
target_link_libraries(signal_studio_app_core PUBLIC
  SignalStudio::Data SignalStudio::DSP SignalStudio::Compute SignalStudio::TaskRuntime SignalStudio::Core)
target_include_directories(signal_studio_app_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
signal_studio_enable_warnings(signal_studio_app_core)

add_executable(signal_studio main.cpp gui.cpp main_window.cpp import_wizard.cpp)
set_target_properties(signal_studio PROPERTIES AUTOMOC ON WIN32_EXECUTABLE ON)
target_link_libraries(signal_studio PRIVATE
  signal_studio_app_core
  SignalStudio::Visualization SignalStudio::Workbench
  Qt6::Core Qt6::Widgets)
signal_studio_enable_warnings(signal_studio)
signal_studio_deploy_msvc_runtime(signal_studio)

install(TARGETS signal_studio RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES main_window.ui DESTINATION "${CMAKE_INSTALL_DATADIR}/SignalStudio/ui")
