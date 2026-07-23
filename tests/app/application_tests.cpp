#include "application.hpp"
#include "wideband_narrowband.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>

namespace {

int g_failures = 0;
const std::filesystem::path g_fixtures = std::filesystem::current_path() / "test_data" / "minimal";
const std::filesystem::path g_external = std::filesystem::current_path().parent_path() / "test_data";

void check(bool cond, std::string_view msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++g_failures;
  }
}
bool approx(double a, double b, double tol) {
  return std::fabs(a - b) <= tol;
}

int case_filename_parse() {
  using namespace signal::studio;
  const auto hint =
      parse_capture_filename(std::filesystem::path("x310_capture_cf1245MHz_sr50MSps_20260521_144927.sc16"));
  check(hint.had_center_frequency, "cf parsed");
  check(approx(hint.center_frequency_hz, 1.245e9, 1.0), "cf value 1245 MHz");
  check(hint.had_sample_rate, "sr parsed");
  check(approx(hint.sample_rate_hz, 50.0e6, 1.0), "sr value 50 MSps");
  const auto hint2 = parse_capture_filename(std::filesystem::path("plain.raw"));
  check(!hint2.had_center_frequency && !hint2.had_sample_rate, "no hints in plain name");
  return g_failures == 0 ? 0 : 1;
}

int case_import_wav() {
  using namespace signal::studio;
  Application app;
  const auto path = g_fixtures / "stereo_iq_int16.wav";
  if (!std::filesystem::exists(path)) {
    std::cerr << "SKIP: fixture missing: " << path << "\n";
    return 0;
  }
  auto imp = app.import_wav(path);
  check(imp.ok(), "wav import ok");
  if (!imp.ok())
    return 1;
  check(imp->total_samples > 0, "wav total samples > 0");
  check(imp->descriptor.signal_kind == signal::data::SignalKind::complex, "wav is complex IQ");
  const std::uint64_t count = std::min<std::uint64_t>(imp->total_samples, 1024);
  auto slice = app.read_samples(*imp->source, 0, count, 16 * 1024 * 1024);
  check(slice.ok(), "wav read ok");
  check(slice->kind() == signal::data::SignalKind::complex, "wav slice complex");
  return g_failures == 0 ? 0 : 1;
}

int case_import_sc16_override() {
  using namespace signal::studio;
  Application app;
  const auto path = g_fixtures / "complex_sine_sc16le.raw";
  if (!std::filesystem::exists(path)) {
    std::cerr << "SKIP: fixture missing: " << path << "\n";
    return 0;
  }
  auto imp = app.import_sc16(path, 64000.0, 0.0);
  check(imp.ok(), "sc16 import with override ok");
  if (!imp.ok())
    return 1;
  check(approx(imp->descriptor.sample_rate_hz, 64000.0, 1e-6), "sc16 sr override");
  check(imp->descriptor.scalar_type == signal::data::ScalarType::int16, "sc16 int16");
  check(imp->descriptor.component_layout == signal::data::ComponentLayout::interleaved, "sc16 interleaved");
  const std::uint64_t count = std::min<std::uint64_t>(imp->total_samples, 1024);
  auto slice = app.read_samples(*imp->source, 0, count, 16 * 1024 * 1024);
  check(slice.ok() && slice->size() == count, "sc16 read ok");
  return g_failures == 0 ? 0 : 1;
}

int case_import_sc16_no_sr_fails() {
  using namespace signal::studio;
  Application app;
  const auto path = g_fixtures / "complex_sine_sc16le.raw";
  if (!std::filesystem::exists(path)) {
    std::cerr << "SKIP: fixture missing\n";
    return 0;
  }
  auto imp = app.import_sc16(path); // no override, filename has no sr hint
  check(!imp.ok(), "sc16 import without sr fails");
  return g_failures == 0 ? 0 : 1;
}

int case_analyze_psd() {
  using namespace signal::studio;
  Application app;
  if (!app.fft_available()) {
    std::cerr << "SKIP: no FFT backend (CUDA) in this build\n";
    return 0;
  }
  const auto path = g_fixtures / "stereo_iq_int16.wav";
  if (!std::filesystem::exists(path)) {
    std::cerr << "SKIP: fixture missing\n";
    return 0;
  }
  auto imp = app.import_wav(path);
  check(imp.ok(), "import for psd");
  if (!imp.ok())
    return 1;
  const std::uint64_t count = std::min<std::uint64_t>(imp->total_samples, 4096);
  auto slice = app.read_samples(*imp->source, 0, count, 16 * 1024 * 1024);
  check(slice.ok(), "read for psd");
  if (!slice.ok())
    return 1;
  auto psd = app.analyze_psd(*slice, imp->descriptor.sample_rate_hz, 1024, 512);
  check(psd.ok(), "psd ok");
  if (psd.ok()) {
    check(psd->frame_count > 0, "psd frame count > 0");
    check(psd->provenance.device == signal::compute::ComputeDeviceType::cuda, "psd cuda provenance");
  }
  return g_failures == 0 ? 0 : 1;
}

int case_external_wav() {
  using namespace signal::studio;
  const auto path = g_external / "20241110-174401-662_bw_12800000_sampleTime_0.4_rollOff_0.3.wav";
  if (!std::filesystem::exists(path)) {
    std::cerr << "SKIP: external WAV not present at " << path << "\n";
    return 0; // environment deviation, not a failure
  }
  Application app;
  auto imp = app.import_wav(path);
  check(imp.ok(), "external wav import ok");
  if (!imp.ok())
    return 1;
  check(approx(imp->descriptor.sample_rate_hz, 12.8e6, 1.0), "external wav sr 12.8 MSps");
  check(imp->total_samples > 1'000'000, "external wav large frame count");
  // Bounded window read: never load the whole 20 MB file.
  const std::uint64_t count = 8192;
  auto slice = app.read_samples(*imp->source, 0, count, 16 * 1024 * 1024);
  check(slice.ok() && slice->size() == count, "external wav bounded read");
  return g_failures == 0 ? 0 : 1;
}

int case_external_sc16() {
  using namespace signal::studio;
  const auto path = g_external / "x310_capture_cf1245MHz_sr50MSps_20260521_144927.sc16";
  if (!std::filesystem::exists(path)) {
    std::cerr << "SKIP: external SC16 not present at " << path << "\n";
    return 0;
  }
  // Filename hint should populate cf=1245 MHz, sr=50 MSps.
  const auto hint = parse_capture_filename(path);
  check(hint.had_center_frequency && approx(hint.center_frequency_hz, 1.245e9, 1.0), "sc16 filename cf hint");
  check(hint.had_sample_rate && approx(hint.sample_rate_hz, 50.0e6, 1.0), "sc16 filename sr hint");
  Application app;
  auto imp = app.import_sc16(path);
  check(imp.ok(), "external sc16 import ok (filename hints)");
  if (!imp.ok())
    return 1;
  check(approx(imp->descriptor.sample_rate_hz, 50.0e6, 1.0), "sc16 sr from filename");
  check(approx(*imp->descriptor.center_frequency_hz, 1.245e9, 1.0), "sc16 cf from filename");
  // Bounded window read on the 1 GB file: never load it wholly.
  const std::uint64_t count = 8192;
  auto slice = app.read_samples(*imp->source, 0, count, 16 * 1024 * 1024);
  check(slice.ok() && slice->size() == count, "external sc16 bounded read");
  return g_failures == 0 ? 0 : 1;
}

int case_wideband_narrowband_link() {
  using namespace signal::studio;
  WidebandNarrowbandController ctrl(50.0e6, 1.245e9);
  check(!ctrl.link().has_value(), "no link initially");
  // Selection within [cf-25MHz, cf+25MHz].
  check(ctrl.set_wideband_selection(1.240e9, 1.250e9).ok(), "valid selection");
  check(ctrl.link().has_value(), "link set after selection");
  const auto& link = *ctrl.link();
  check(approx(link.channel.center_frequency_hz, 1.245e9, 1.0), "channel center = midpoint");
  check(approx(link.channel.bandwidth_hz, 10.0e6, 1.0), "channel bandwidth = span");
  check(approx(link.channel.output_sample_rate_hz, 20.0e6, 1.0), "output rate = 2x bandwidth");
  // Inverted selection rejected.
  check(!ctrl.set_wideband_selection(1.250e9, 1.240e9).ok(), "inverted selection rejected");
  // Out-of-band selection rejected.
  check(!ctrl.set_wideband_selection(1.0e9, 1.1e9).ok(), "out-of-band selection rejected");
  return g_failures == 0 ? 0 : 1;
}

int case_narrowband_extraction() {
  using namespace signal::studio;
  // Synthesize a complex tone at 1 kHz within a 64 kHz capture, then extract a 200 Hz channel
  // around 1 kHz and verify the baseband output is dominated by a low-frequency component.
  constexpr std::uint64_t N = 4096;
  const double fs = 64000.0;
  const double tone = 1000.0;
  std::vector<signal::data::ComplexSample> iq(N);
  for (std::uint64_t n = 0; n < N; ++n) {
    const double phase = 2.0 * std::numbers::pi * tone * static_cast<double>(n) / fs;
    iq[n].real = std::cos(phase);
    iq[n].imag = std::sin(phase);
  }
  auto buf = signal::data::SignalBuffer::from_complex(iq);
  NarrowbandChannelSpec spec;
  spec.center_frequency_hz = tone;
  spec.bandwidth_hz = 200.0;
  spec.output_sample_rate_hz = 400.0; // 2x bandwidth
  auto ch = extract_narrowband_channel(buf.view(), fs, spec);
  check(ch.ok(), "narrowband extraction ok");
  if (!ch.ok())
    return 1;
  check(!ch->samples.empty(), "channel has samples");
  check(approx(ch->output_sample_rate_hz, 400.0, 1e-6), "channel output rate");
  // After down-conversion the 1 kHz tone sits at baseband (~0 Hz). Verify the extracted samples
  // have near-constant magnitude (a pure tone at baseband has constant envelope).
  double mag_mean = 0.0, mag_var = 0.0;
  for (const auto& s : ch->samples) {
    mag_mean += std::sqrt(s.real * s.real + s.imag * s.imag);
  }
  mag_mean /= static_cast<double>(ch->samples.size());
  for (const auto& s : ch->samples) {
    const double m = std::sqrt(s.real * s.real + s.imag * s.imag);
    mag_var += (m - mag_mean) * (m - mag_mean);
  }
  mag_var /= static_cast<double>(ch->samples.size());
  check(mag_mean > 0.05, "channel magnitude non-trivial");
  check(mag_var / (mag_mean * mag_mean + 1e-12) < 0.5, "channel envelope stable (baseband tone)");
  return g_failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string_view{argv[1]} != "--case") {
    std::cerr << "usage: application_tests --case <name>\n";
    return 2;
  }
  std::string_view name = argv[2];
  if (name == "filename-parse")
    return case_filename_parse();
  if (name == "import-wav")
    return case_import_wav();
  if (name == "import-sc16-override")
    return case_import_sc16_override();
  if (name == "import-sc16-no-sr-fails")
    return case_import_sc16_no_sr_fails();
  if (name == "analyze-psd")
    return case_analyze_psd();
  if (name == "external-wav")
    return case_external_wav();
  if (name == "external-sc16")
    return case_external_sc16();
  if (name == "wideband-narrowband-link")
    return case_wideband_narrowband_link();
  if (name == "narrowband-extraction")
    return case_narrowband_extraction();
  std::cerr << "unknown case: " << name << "\n";
  return 2;
}
