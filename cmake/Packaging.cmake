# Packaging: produces a self-contained portable ZIP via CPack. Qt runtime/plugins are deployed
# next to the executable by windeployqt; MSVC runtime DLLs are already copied by
# signal_studio_deploy_msvc_runtime. NSIS installer is configured but only generated when
# makensis is available on the host (recorded as an environment deviation otherwise).

# windeployqt is invoked as a POST_BUILD step on the signal_studio target from
# apps/signal_studio/CMakeLists.txt (where the target is created). This file only configures CPack.

set(CPACK_PACKAGE_NAME "SignalStudio")
set(CPACK_PACKAGE_VENDOR "Signal Processing Platform")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Signal Studio - Windows IQ signal analysis platform")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/yulongpo/signal_studio")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/LICENSE.txt")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "SignalStudio")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_BINARY_DIR}/packages")

# Default generator: ZIP portable package always available on Windows.
set(CPACK_GENERATOR "ZIP")
find_program(MAKENSIS_EXECUTABLE makensis)
if(MAKENSIS_EXECUTABLE)
  list(APPEND CPACK_GENERATOR "NSIS")
endif()

set(CPACK_COMPONENTS_ALL applications runtime headers cmake)
set(CPACK_COMPONENT_APPLICATIONS_DISPLAY_NAME "Signal Studio application")
set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "Runtime libraries (Qt/VC)")
set(CPACK_COMPONENT_HEADERS_DISPLAY_NAME "Public C++ headers")
set(CPACK_COMPONENT_CMAKE_DISPLAY_NAME "CMake package config")
set(CPACK_COMPONENT_APPLICATIONS_REQUIRED TRUE)
set(CPACK_COMPONENT_RUNTIME_REQUIRED TRUE)

include(CPack)
