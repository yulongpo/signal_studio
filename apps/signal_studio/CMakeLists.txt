# Qt-free application core: composes Data/DSP/Compute/TaskRuntime/Core. Shared by the GUI
# executable and the headless self-test so import/analyze logic is testable without QApplication.
add_library(signal_studio_app_core STATIC application.cpp wideband_narrowband.cpp)
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

# Deploy Qt DLLs and plugins next to the executable so the portable package runs without a Qt
# installation. windeployqt runs as a POST_BUILD step; CPack then installs the whole bin tree.
find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${SIGNAL_STUDIO_QT_ROOT}/bin")
if(WINDEPLOYQT_EXECUTABLE)
  add_custom_command(TARGET signal_studio POST_BUILD
    COMMAND "${WINDEPLOYQT_EXECUTABLE}" --no-translations --no-system-d3d-compiler
            --no-opengl-sw --release --no-compiler-runtime
            "$<TARGET_FILE:signal_studio>"
    COMMENT "Deploying Qt runtime for portable package")
  install(DIRECTORY "$<TARGET_FILE_DIR:signal_studio>/"
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT runtime
    FILES_MATCHING PATTERN "*.dll")
  install(DIRECTORY "$<TARGET_FILE_DIR:signal_studio>/platforms"
    DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime OPTIONAL)
  install(DIRECTORY "$<TARGET_FILE_DIR:signal_studio>/styles"
    DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime OPTIONAL)
endif()

install(TARGETS signal_studio RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT applications)
install(FILES main_window.ui DESTINATION "${CMAKE_INSTALL_DATADIR}/SignalStudio/ui" COMPONENT applications)
