#include "application.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace signal::studio {
// GUI entry point defined in gui.cpp; kept out of main.cpp so the headless self-test path has
// no Qt linkage requirement and can run without a platform plugin.
int runGui(int argc, char** argv);
}  // namespace signal::studio

namespace {

// Headless self-test: import the bundled minimal WAV, read samples, report. Used by CI to verify
// the application composes correctly without a Qt event loop or window. Runs WITHOUT a
// QApplication so it never depends on a Qt platform plugin.
int runSelfTest() {
  signal::studio::Application app;
  const std::filesystem::path minimal =
      std::filesystem::current_path() / "test_data" / "minimal" / "stereo_iq_int16.wav";
  std::filesystem::path wav = minimal;
  if (!std::filesystem::exists(wav)) {
    wav = std::filesystem::current_path() / "test_data" / "minimal" / "tone.wav";
  }
  if (!std::filesystem::exists(wav)) {
    std::printf("self-test: no minimal WAV fixture found, skipping import (environment)\n");
    return 0;
  }
  auto imp = app.import_wav(wav);
  if (!imp.ok()) {
    std::fprintf(stderr, "self-test: import failed: %s\n", std::string(imp.error().message()).c_str());
    return 1;
  }
  const std::uint64_t count = std::min<std::uint64_t>(imp->total_samples, 4096);
  auto slice = app.read_samples(*imp->source, 0, count, 16 * 1024 * 1024);
  if (!slice.ok()) {
    std::fprintf(stderr, "self-test: read failed: %s\n", std::string(slice.error().message()).c_str());
    return 1;
  }
  std::printf("self-test: imported %llu samples @ %.3f Hz, read %llu\n",
              static_cast<unsigned long long>(imp->total_samples), imp->descriptor.sample_rate_hz,
              static_cast<unsigned long long>(count));
  return 0;
}

bool has_self_test_flag(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--self-test") return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (has_self_test_flag(argc, argv)) {
    return runSelfTest();
  }

  // GUI path: defer Qt includes so the headless self-test above has no Qt linkage requirement.
  // The QApplication is constructed only when a real GUI session is requested.
  return signal::studio::runGui(argc, argv);
}
