# Signal Review: second thin application proving platform reuse. Links ONLY public modules -
# no Qt, no private headers, no apps/signal_studio dependency. This is the reuse audit target.
add_executable(signal_review main.cpp)
target_link_libraries(signal_review PRIVATE
  SignalStudio::Data SignalStudio::DSP SignalStudio::Compute SignalStudio::Core)
signal_studio_enable_warnings(signal_review)
signal_studio_deploy_msvc_runtime(signal_review)

install(TARGETS signal_review RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT applications)
