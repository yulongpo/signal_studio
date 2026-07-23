#include "wideband_narrowband.hpp"

#include "signal_studio/dsp/filter.hpp"
#include "signal_studio/dsp/resample.hpp"

#include <cmath>
#include <numbers>

namespace signal::studio {

namespace {
core::Status app_failure(core::ErrorReason reason, std::string message) {
  return core::Status::failure(core::ErrorCode{core::ErrorDomain::core, reason}, std::move(message));
}
}  // namespace

core::Result<NarrowbandChannel>
extract_narrowband_channel(const data::SignalSlice& wideband, double sample_rate_hz,
                           const NarrowbandChannelSpec& spec, std::uint64_t source_start_sample) {
  if (sample_rate_hz <= 0.0) {
    return app_failure(core::ErrorReason::invalid_argument, "sample rate must be positive");
  }
  if (spec.bandwidth_hz <= 0.0 || spec.output_sample_rate_hz <= 0.0) {
    return app_failure(core::ErrorReason::invalid_argument, "channel bandwidth and output rate must be positive");
  }
  if (wideband.kind() != data::SignalKind::complex) {
    return app_failure(core::ErrorReason::invalid_argument, "narrowband extraction requires complex IQ input");
  }
  const auto in = wideband.complex_values();
  if (in.empty()) {
    return app_failure(core::ErrorReason::invalid_argument, "wideband slice is empty");
  }
  const double nyquist = sample_rate_hz / 2.0;
  const double cutoff = std::min(spec.bandwidth_hz / 2.0, nyquist);

  // 1. Digital down-conversion: multiply by exp(-j*2*pi*fc*n/fs) to shift fc to baseband.
  // (I + jQ) * (cos(theta) - j*sin(theta)) = (I*cos + Q*sin) + j(Q*cos - I*sin), theta = 2*pi*fc*n/fs.
  const double fc = spec.center_frequency_hz;
  std::vector<double> I(in.size()), Q(in.size());
  for (std::size_t n = 0; n < in.size(); ++n) {
    const double theta = 2.0 * std::numbers::pi * fc * static_cast<double>(n) / sample_rate_hz;
    const double c = std::cos(theta);
    const double s = std::sin(theta);
    I[n] = in[n].real * c + in[n].imag * s;
    Q[n] = in[n].imag * c - in[n].real * s;
  }

  // 2. Low-pass filter both legs (cutoff = bandwidth/2). Order 64 is a reasonable default.
  dsp::FilterSpec fspec;
  fspec.type = dsp::FilterType::lowpass;
  fspec.order = 64;
  fspec.sample_rate_hz = sample_rate_hz;
  fspec.cutoff_low_hz = cutoff;
  auto filter = dsp::FirFilter::design(fspec);
  if (!filter.ok()) {
    return core::Status(filter.error());
  }
  std::vector<double> I_filt(I.size(), 0.0), Q_filt(Q.size(), 0.0);
  dsp::FilterState si, sq;
  auto st = filter->process(I, si, I_filt);
  if (!st.ok()) return st;
  st = filter->process(Q, sq, Q_filt);
  if (!st.ok()) return st;

  // 3. Resample both legs to the output rate. Reduce L/M to lowest terms so the polyphase
  // filter does not balloon to millions of taps (e.g. 64000->400 is 1:160, not 400000:64000000).
  auto gcd = [](std::uint64_t a, std::uint64_t b) {
    while (b != 0) {
      std::uint64_t t = a % b;
      a = b;
      b = t;
    }
    return a;
  };
  const std::uint64_t L_raw = static_cast<std::uint64_t>(std::llround(spec.output_sample_rate_hz));
  const std::uint64_t M_raw = static_cast<std::uint64_t>(std::llround(sample_rate_hz));
  if (L_raw == 0 || M_raw == 0) {
    return app_failure(core::ErrorReason::invalid_argument, "resample ratio zero");
  }
  const std::uint64_t g = gcd(L_raw, M_raw);
  dsp::ResampleRatio ratio{static_cast<std::uint32_t>(L_raw / g), static_cast<std::uint32_t>(M_raw / g)};
  dsp::PolyphaseResampler resampler;
  auto I_out = resampler.process(ratio, I_filt);
  if (!I_out.ok()) return core::Status(I_out.error());
  auto Q_out = resampler.process(ratio, Q_filt);
  if (!Q_out.ok()) return core::Status(Q_out.error());

  const std::size_t n_out = std::min(I_out->size(), Q_out->size());
  NarrowbandChannel channel;
  channel.spec = spec;
  channel.output_sample_rate_hz = spec.output_sample_rate_hz;
  channel.source_start_sample = source_start_sample;
  channel.samples.reserve(n_out);
  for (std::size_t n = 0; n < n_out; ++n) {
    channel.samples.push_back(data::ComplexSample{(*I_out)[n], (*Q_out)[n]});
  }
  return channel;
}

WidebandNarrowbandController::WidebandNarrowbandController(double wideband_sample_rate_hz,
                                                            double center_frequency_hz)
    : wideband_sample_rate_hz_(wideband_sample_rate_hz), center_frequency_hz_(center_frequency_hz) {}

core::Status WidebandNarrowbandController::set_wideband_selection(double lo_hz, double hi_hz) {
  if (lo_hz > hi_hz) {
    return app_failure(core::ErrorReason::invalid_argument, "wideband selection must not be inverted");
  }
  const double nyquist = wideband_sample_rate_hz_ / 2.0;
  const double min_f = center_frequency_hz_ - nyquist;
  const double max_f = center_frequency_hz_ + nyquist;
  if (lo_hz < min_f || hi_hz > max_f) {
    return app_failure(core::ErrorReason::invalid_argument, "wideband selection exceeds capture band");
  }
  WidebandNarrowbandLink link;
  link.wideband_selection_lo_hz = lo_hz;
  link.wideband_selection_hi_hz = hi_hz;
  link.channel.center_frequency_hz = (lo_hz + hi_hz) / 2.0;
  link.channel.bandwidth_hz = hi_hz - lo_hz;
  // Output rate = bandwidth * 2 satisfies Nyquist for the channel without oversampling.
  link.channel.output_sample_rate_hz = (hi_hz - lo_hz) * 2.0;
  link_ = link;
  return core::Status::success();
}

core::Result<NarrowbandChannelSpec>
WidebandNarrowbandController::derive_channel_spec(std::optional<double> override_output_rate_hz) const {
  if (!link_) {
    return app_failure(core::ErrorReason::unavailable, "no wideband selection set");
  }
  NarrowbandChannelSpec spec = link_->channel;
  if (override_output_rate_hz && *override_output_rate_hz > 0.0) {
    spec.output_sample_rate_hz = *override_output_rate_hz;
  }
  return spec;
}

}  // namespace signal::studio
