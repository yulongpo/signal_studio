#include "signal_studio/data/signal.hpp"
#include "signal_studio/dsp/fft.hpp"
#include "signal_studio/dsp/filter.hpp"
#include "signal_studio/dsp/resample.hpp"
#include "signal_studio/dsp/spectrum.hpp"
#include "signal_studio/dsp/statistics.hpp"
#include "signal_studio/dsp/stft.hpp"
#include "signal_studio/dsp/windows.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, std::string_view msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++g_failures;
  }
}

bool approx(double a, double b, double tol) {
  return std::fabs(a - b) <= tol;
}

std::vector<double> sine(int n, double freq, double fs, double amp = 1.0) {
  std::vector<double> x(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    x[static_cast<std::size_t>(i)] = amp * std::sin(2.0 * std::numbers::pi * freq * i / fs);
  }
  return x;
}

std::vector<signal::data::ComplexSample> to_complex(const std::vector<double>& re) {
  std::vector<signal::data::ComplexSample> out(re.size());
  for (std::size_t i = 0; i < re.size(); ++i) {
    out[i].real = re[i];
    out[i].imag = 0.0;
  }
  return out;
}

double rms(std::span<const double> v) {
  double s = 0.0;
  for (double x : v)
    s += x * x;
  return std::sqrt(s / static_cast<double>(v.size()));
}

} // namespace

int case_windows_length_and_range() {
  signal::dsp::WindowSpec spec;
  spec.type = signal::dsp::WindowType::hann;
  spec.length = 64;
  spec.symmetric = false;
  auto r = signal::dsp::generate_window(spec);
  check(r.ok(), "hann window generation succeeds");
  check(r->size() == 64, "hann window length 64");
  double mn = 1e9, mx = -1e9;
  for (double v : *r) {
    mn = std::min(mn, v);
    mx = std::max(mx, v);
  }
  check(approx(mn, 0.0, 1e-9), "periodic hann starts near 0");
  check(approx(mx, 1.0, 1e-9), "hann peak near 1");
  return g_failures == 0 ? 0 : 1;
}

int case_windows_enbw() {
  check(approx(signal::dsp::window_enbw_bins(signal::dsp::WindowType::rectangular), 1.0, 1e-9), "rect enbw 1.0");
  check(approx(signal::dsp::window_enbw_bins(signal::dsp::WindowType::hann), 1.5, 1e-9), "hann enbw 1.5");
  check(approx(signal::dsp::window_enbw_bins(signal::dsp::WindowType::blackman), 1.7268, 1e-3), "blackman enbw");
  return g_failures == 0 ? 0 : 1;
}

int case_windows_symmetry() {
  signal::dsp::WindowSpec spec;
  spec.type = signal::dsp::WindowType::hann;
  spec.length = 64;
  spec.symmetric = true;
  auto r = signal::dsp::generate_window(spec);
  check(r.ok(), "symmetric hann generation");
  bool symmetric = true;
  for (std::size_t i = 0; i < r->size(); ++i) {
    if (!approx((*r)[i], (*r)[r->size() - 1 - i], 1e-9)) {
      symmetric = false;
      break;
    }
  }
  check(symmetric, "symmetric hann is symmetric");
  return g_failures == 0 ? 0 : 1;
}

int case_statistics_real() {
  std::vector<double> x = {1.0, 2.0, 3.0, 4.0};
  auto buf = signal::data::SignalBuffer::from_real(x);
  auto r = signal::dsp::compute_real_stats(buf.view());
  check(r.ok(), "real stats succeeds");
  check(approx(r->mean, 2.5, 1e-12), "mean 2.5");
  check(approx(r->variance, 1.25, 1e-12), "variance 1.25");
  check(approx(r->rms, std::sqrt(7.5), 1e-9), "rms sqrt(7.5)");
  check(approx(r->peak, 4.0, 1e-12), "peak 4");
  check(r->peak_index == 3, "peak index 3");
  check(approx(r->min, 1.0, 1e-12) && approx(r->max, 4.0, 1e-12), "min/max");
  return g_failures == 0 ? 0 : 1;
}

int case_statistics_complex() {
  std::vector<signal::data::ComplexSample> c = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
  auto buf = signal::data::SignalBuffer::from_complex(c);
  auto r = signal::dsp::compute_complex_stats(buf.view());
  check(r.ok(), "complex stats succeeds");
  check(approx(r->magnitude_mean, 1.0, 1e-12), "unit circle magnitude mean 1");
  check(approx(r->magnitude_rms, 1.0, 1e-12), "unit circle magnitude rms 1");
  check(approx(r->magnitude_peak, 1.0, 1e-12), "unit circle magnitude peak 1");
  return g_failures == 0 ? 0 : 1;
}

int case_filter_impulse_response() {
  signal::dsp::FilterSpec spec;
  spec.type = signal::dsp::FilterType::lowpass;
  spec.order = 64;
  spec.sample_rate_hz = 1000.0;
  spec.cutoff_low_hz = 100.0;
  auto f = signal::dsp::FirFilter::design(spec);
  check(f.ok(), "fir design succeeds");
  const std::size_t taps = f->taps();
  std::vector<double> impulse(taps, 0.0);
  impulse[0] = 1.0;
  std::vector<double> out(taps, 0.0);
  signal::dsp::FilterState state;
  auto st = f->process(impulse, state, out);
  check(st.ok(), "fir process succeeds");
  const auto& coeffs = f->coefficients();
  bool match = true;
  for (std::size_t i = 0; i < taps; ++i) {
    if (!approx(out[i], coeffs[i], 1e-12)) {
      match = false;
      break;
    }
  }
  check(match, "impulse response equals coefficients");
  return g_failures == 0 ? 0 : 1;
}

int case_filter_lowpass_attenuates() {
  signal::dsp::FilterSpec spec;
  spec.type = signal::dsp::FilterType::lowpass;
  spec.order = 64;
  spec.sample_rate_hz = 1000.0;
  spec.cutoff_low_hz = 100.0;
  auto f = signal::dsp::FirFilter::design(spec);
  check(f.ok(), "lowpass design");
  // Passband: 50 Hz tone should pass largely intact (skip transient).
  auto x_pass = sine(1000, 50.0, 1000.0);
  std::vector<double> y_pass(x_pass.size(), 0.0);
  signal::dsp::FilterState s1;
  check(f->process(x_pass, s1, y_pass).ok(), "lowpass process passband");
  double pass_rms = rms(std::span<const double>(y_pass).subspan(200));
  check(pass_rms > 0.6, "50 Hz passes lowpass (rms preserved)");
  // Stopband: 300 Hz tone should be attenuated.
  auto x_stop = sine(1000, 300.0, 1000.0);
  std::vector<double> y_stop(x_stop.size(), 0.0);
  signal::dsp::FilterState s2;
  check(f->process(x_stop, s2, y_stop).ok(), "lowpass process stopband");
  double stop_rms = rms(std::span<const double>(y_stop).subspan(200));
  check(stop_rms < 0.15, "300 Hz attenuated by lowpass");
  return g_failures == 0 ? 0 : 1;
}

int case_resample_identity() {
  std::vector<double> x = sine(200, 10.0, 200.0);
  signal::dsp::PolyphaseResampler r;
  signal::dsp::ResampleRatio ratio{1, 1};
  auto y = r.process(ratio, x);
  check(y.ok(), "identity resample succeeds");
  check(y->size() == x.size(), "identity preserves length");
  bool match = true;
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (!approx((*y)[i], x[i], 1e-9)) {
      match = false;
      break;
    }
  }
  check(match, "identity preserves samples");
  return g_failures == 0 ? 0 : 1;
}

int case_resample_length_up() {
  std::vector<double> x = sine(2000, 50.0, 1000.0);
  signal::dsp::PolyphaseResampler r;
  signal::dsp::ResampleRatio ratio{2, 1};
  auto y = r.process(ratio, x);
  check(y.ok(), "upsample succeeds");
  check(y->size() >= 3900 && y->size() <= 4001, "upsample length ~2x");
  return g_failures == 0 ? 0 : 1;
}

int case_resample_length_down() {
  std::vector<double> x = sine(2000, 50.0, 1000.0);
  signal::dsp::PolyphaseResampler r;
  signal::dsp::ResampleRatio ratio{1, 2};
  auto y = r.process(ratio, x);
  check(y.ok(), "downsample succeeds");
  check(y->size() >= 950 && y->size() <= 1001, "downsample length ~0.5x");
  return g_failures == 0 ? 0 : 1;
}

int case_resample_sine_amplitude() {
  std::vector<double> x = sine(2000, 50.0, 1000.0);
  signal::dsp::PolyphaseResampler r;
  signal::dsp::ResampleRatio ratio{2, 1};
  auto y = r.process(ratio, x);
  check(y.ok(), "sine upsample succeeds");
  double peak = 0.0;
  for (std::size_t i = 100; i < y->size(); ++i) {
    peak = std::max(peak, std::fabs((*y)[i]));
  }
  check(approx(peak, 1.0, 0.25), "upsampled sine amplitude preserved");
  return g_failures == 0 ? 0 : 1;
}

int case_fft_single_tone_bin() {
  auto backend = signal::dsp::make_fft_backend(signal::compute::ComputeDeviceType::cuda);
  if (!backend.ok()) {
    std::cerr << "SKIP: cuda fft backend unavailable\n";
    return 0;
  }
  constexpr int N = 1024;
  const double fs = 1024.0; // fs == N so 100 Hz lands exactly on bin 100 (no scalloping loss)
  const double f = 100.0;
  auto x = sine(N, f, fs);
  auto cx = to_complex(x);
  auto r = signal::dsp::fft(cx, *backend.value());
  check(r.ok(), "fft succeeds");
  int peak = 1;
  double peak_mag = 0.0;
  for (int k = 1; k <= N / 2; ++k) {
    const auto& b = r->bins[static_cast<std::size_t>(k)];
    const double m = std::sqrt(b.real * b.real + b.imag * b.imag);
    if (m > peak_mag) {
      peak_mag = m;
      peak = k;
    }
  }
  const int expected = static_cast<int>(std::lround(f * N / fs));
  check(peak == expected, "fft peak at tone bin");
  check(peak_mag > N * 0.4, "fft peak magnitude ~ N/2");
  return g_failures == 0 ? 0 : 1;
}

int case_fft_parseval() {
  auto backend = signal::dsp::make_fft_backend(signal::compute::ComputeDeviceType::cuda);
  if (!backend.ok()) {
    std::cerr << "SKIP: cuda fft backend unavailable\n";
    return 0;
  }
  constexpr int N = 1024;
  auto x = sine(N, 100.0, 1000.0);
  auto cx = to_complex(x);
  auto r = signal::dsp::fft(cx, *backend.value());
  check(r.ok(), "fft succeeds");
  double time_energy = 0.0, freq_energy = 0.0;
  for (int i = 0; i < N; ++i)
    time_energy += x[static_cast<std::size_t>(i)] * x[static_cast<std::size_t>(i)];
  for (int k = 0; k < N; ++k) {
    const auto& b = r->bins[static_cast<std::size_t>(k)];
    freq_energy += b.real * b.real + b.imag * b.imag;
  }
  // Parseval: sum |x|^2 = (1/N) sum |X|^2
  check(approx(time_energy, freq_energy / N, time_energy * 1e-3), "parseval holds");
  return g_failures == 0 ? 0 : 1;
}

int case_fft_ifft_roundtrip() {
  auto backend = signal::dsp::make_fft_backend(signal::compute::ComputeDeviceType::cuda);
  if (!backend.ok()) {
    std::cerr << "SKIP: cuda fft backend unavailable\n";
    return 0;
  }
  constexpr int N = 512;
  auto x = sine(N, 100.0, 1000.0, 0.5);
  auto cx = to_complex(x);
  auto fwd = signal::dsp::fft(cx, *backend.value());
  check(fwd.ok(), "fft succeeds");
  auto inv = signal::dsp::ifft(fwd->bins, *backend.value());
  check(inv.ok(), "ifft succeeds");
  double max_err = 0.0;
  for (int i = 0; i < N; ++i) {
    max_err =
        std::max(max_err, std::fabs(inv->bins[static_cast<std::size_t>(i)].real - x[static_cast<std::size_t>(i)]));
  }
  check(max_err < 1e-7, "ifft(fft(x)) recovers x");
  return g_failures == 0 ? 0 : 1;
}

int case_psd_tone_frequency() {
  auto backend = signal::dsp::make_fft_backend(signal::compute::ComputeDeviceType::cuda);
  if (!backend.ok()) {
    std::cerr << "SKIP: cuda fft backend unavailable\n";
    return 0;
  }
  constexpr int N = 1024;
  const double fs = 1000.0;
  const double f = 100.0;
  auto x = sine(N, f, fs);
  auto buf = signal::data::SignalBuffer::from_real(x);
  signal::dsp::WelchPsdEstimator est(*backend.value());
  signal::dsp::PsdRequest req;
  req.window = signal::dsp::WindowType::hann;
  req.nfft = N;
  req.overlap_samples = N / 2;
  req.scaling = signal::dsp::SpectrumScaling::density;
  req.one_sided = true;
  auto r = est.process(buf.view(), fs, req);
  check(r.ok(), "psd succeeds");
  std::size_t peak = 0;
  double peak_v = -1e9;
  for (std::size_t k = 0; k < r->power.size(); ++k) {
    if (r->power[k] > peak_v) {
      peak_v = r->power[k];
      peak = k;
    }
  }
  check(approx(r->frequencies_hz[peak], f, fs / N * 1.5), "psd peak at tone frequency");
  auto db = signal::dsp::to_db_hz(r->power);
  bool finite = true;
  for (double v : db) {
    if (!std::isfinite(v))
      finite = false;
  }
  check(finite, "psd to_db_hz finite");
  check(r->provenance.device == signal::compute::ComputeDeviceType::cuda, "psd provenance cuda");
  return g_failures == 0 ? 0 : 1;
}

int case_stft_tone_timefreq() {
  auto backend = signal::dsp::make_fft_backend(signal::compute::ComputeDeviceType::cuda);
  if (!backend.ok()) {
    std::cerr << "SKIP: cuda fft backend unavailable\n";
    return 0;
  }
  const double fs = 1000.0;
  const double f = 100.0;
  auto x = sine(2000, f, fs);
  auto buf = signal::data::SignalBuffer::from_real(x);
  signal::dsp::StftProcessor proc(*backend.value());
  signal::dsp::StftRequest req;
  req.window = signal::dsp::WindowType::hann;
  req.nfft = 256;
  req.hop_samples = 64;
  req.output_magnitude = true;
  auto r = proc.process(buf.view(), fs, req);
  check(r.ok(), "stft succeeds");
  check(r->frame_count > 1, "stft multiple frames");
  check(r->freq_count == 256 / 2 + 1, "stft one-sided freq bins");
  // Average magnitude across frames; peak frequency should match the tone.
  std::vector<double> avg(r->freq_count, 0.0);
  for (std::uint64_t fr = 0; fr < r->frame_count; ++fr) {
    for (std::uint64_t k = 0; k < r->freq_count; ++k) {
      avg[static_cast<std::size_t>(k)] += r->matrix[static_cast<std::size_t>(fr * r->freq_count + k)];
    }
  }
  std::size_t peak = 0;
  double peak_v = -1e9;
  for (std::size_t k = 0; k < avg.size(); ++k) {
    if (avg[k] > peak_v) {
      peak_v = avg[k];
      peak = k;
    }
  }
  check(approx(r->freq_bins[peak], f, fs / 256 * 1.5), "stft peak frequency matches tone");
  // time_bins increasing.
  bool increasing = true;
  for (std::size_t i = 1; i < r->time_bins.size(); ++i) {
    if (r->time_bins[i] <= r->time_bins[i - 1])
      increasing = false;
  }
  check(increasing, "stft time bins increasing");
  return g_failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc != 3 || std::string_view{argv[1]} != "--case") {
    std::cerr << "usage: dsp_tests --case <name>\n";
    return 2;
  }
  std::string_view name = argv[2];
  if (name == "windows-length-and-range")
    return case_windows_length_and_range();
  if (name == "windows-enbw")
    return case_windows_enbw();
  if (name == "windows-symmetry")
    return case_windows_symmetry();
  if (name == "statistics-real")
    return case_statistics_real();
  if (name == "statistics-complex")
    return case_statistics_complex();
  if (name == "filter-impulse-response")
    return case_filter_impulse_response();
  if (name == "filter-lowpass-attenuates")
    return case_filter_lowpass_attenuates();
  if (name == "resample-identity")
    return case_resample_identity();
  if (name == "resample-length-up")
    return case_resample_length_up();
  if (name == "resample-length-down")
    return case_resample_length_down();
  if (name == "resample-sine-amplitude")
    return case_resample_sine_amplitude();
  if (name == "fft-single-tone-bin")
    return case_fft_single_tone_bin();
  if (name == "fft-parseval")
    return case_fft_parseval();
  if (name == "fft-ifft-roundtrip")
    return case_fft_ifft_roundtrip();
  if (name == "psd-tone-frequency")
    return case_psd_tone_frequency();
  if (name == "stft-tone-timefreq")
    return case_stft_tone_timefreq();
  std::cerr << "unknown case: " << name << "\n";
  return 2;
}
