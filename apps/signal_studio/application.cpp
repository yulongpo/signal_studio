#include "application.hpp"

#include "signal_studio/data/signal.hpp"

#include <cmath>
#include <filesystem>
#include <regex>

namespace signal::studio {

namespace {
double unit_scale_freq(std::string_view unit) {
  if (unit == "GHz")
    return 1.0e9;
  if (unit == "MHz")
    return 1.0e6;
  if (unit == "kHz")
    return 1.0e3;
  return 1.0;
}
double unit_scale_rate(std::string_view unit) {
  if (unit == "GSps")
    return 1.0e9;
  if (unit == "MSps")
    return 1.0e6;
  if (unit == "kSps")
    return 1.0e3;
  return 1.0;
}
} // namespace

FilenameHint parse_capture_filename(const std::filesystem::path& path) {
  FilenameHint hint;
  const std::string name = path.filename().string();
  std::smatch m;
  static const std::regex cf_re("cf([0-9]+(?:\\.[0-9]+)?)(GHz|MHz|kHz|Hz)");
  if (std::regex_search(name, m, cf_re)) {
    hint.center_frequency_hz = std::stod(m[1].str()) * unit_scale_freq(m[2].str());
    hint.had_center_frequency = true;
  }
  static const std::regex sr_re("sr([0-9]+(?:\\.[0-9]+)?)(GSps|MSps|kSps|Sps)");
  if (std::regex_search(name, m, sr_re)) {
    hint.sample_rate_hz = std::stod(m[1].str()) * unit_scale_rate(m[2].str());
    hint.had_sample_rate = true;
  }
  return hint;
}

Application::Application() {
  task::RuntimeConfig cfg;
  cfg.worker_count = 2;
  runtime_ = std::make_unique<task::TaskRuntime>(cfg);
  auto backend = dsp::make_fft_backend(compute::ComputeDeviceType::cuda);
  if (backend.ok()) {
    fft_ = std::move(*backend);
    psd_ = std::make_unique<dsp::WelchPsdEstimator>(*fft_);
    stft_ = std::make_unique<dsp::StftProcessor>(*fft_);
  }
}

Application::~Application() = default;

task::TaskRuntime& Application::task_runtime() noexcept {
  return *runtime_;
}

bool Application::fft_available() const noexcept {
  return fft_ != nullptr;
}

namespace {
core::Result<ImportResult> finalize_import(std::shared_ptr<data::FileDataSource> source,
                                           const std::filesystem::path& path, const data::SignalDescriptor& descriptor,
                                           std::string version_id) {
  if (!source) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::core, core::ErrorReason::internal_failure},
                                 "import produced a null data source");
  }
  ImportResult result;
  result.source = std::move(source);
  result.descriptor = descriptor;
  result.data_source_version_id = std::move(version_id);
  std::error_code ec;
  const std::uint64_t file_bytes = std::filesystem::file_size(path, ec);
  if (!ec) {
    auto frame_bytes = descriptor.frame_bytes();
    if (frame_bytes.ok() && *frame_bytes > 0) {
      const std::uint64_t usable = (file_bytes > descriptor.byte_offset) ? (file_bytes - descriptor.byte_offset) : 0;
      result.total_samples = usable / *frame_bytes;
    }
  }
  return result;
}
} // namespace

core::Result<ImportResult> Application::import_wav(const std::filesystem::path& path) {
  auto wd = data::read_wav_descriptor(path, true);
  if (!wd.ok()) {
    return core::Status(wd.error());
  }
  std::string version_id = "wav:" + path.filename().string();
  auto source = data::FileDataSource::open_wav(path, version_id, true);
  if (!source.ok()) {
    return core::Status(source.error());
  }
  return finalize_import(std::move(*source), path, wd->descriptor, std::move(version_id));
}

core::Result<ImportResult> Application::import_raw(const std::filesystem::path& path,
                                                   data::SignalDescriptor descriptor) {
  auto status = descriptor.validate();
  if (!status.ok()) {
    return status;
  }
  std::error_code ec;
  const std::uint64_t file_bytes = std::filesystem::file_size(path, ec);
  if (ec) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::data, core::ErrorReason::invalid_argument},
                                 "cannot size RAW source: " + ec.message());
  }
  // Confirm the full available frame range so subsequent reads are not rejected by the
  // requested_sample_range containment check (open_raw does not set it implicitly).
  auto facts = data::calculate_data_facts(file_bytes, descriptor, 1U);
  if (!facts.ok()) {
    return core::Status(facts.error());
  }
  auto range = data::SampleRange::from_count(0, facts->available_frames);
  if (!range.ok()) {
    return core::Status(range.error());
  }
  descriptor.requested_sample_range = *range;
  std::string version_id = "raw:" + path.filename().string();
  auto source = data::FileDataSource::open_raw(path, descriptor, version_id);
  if (!source.ok()) {
    return core::Status(source.error());
  }
  return finalize_import(std::move(*source), path, descriptor, std::move(version_id));
}

core::Result<ImportResult> Application::import_sc16(const std::filesystem::path& path,
                                                    std::optional<double> override_sample_rate_hz,
                                                    std::optional<double> override_center_frequency_hz) {
  const FilenameHint hint = parse_capture_filename(path);
  if (!override_sample_rate_hz && !hint.had_sample_rate) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::data, core::ErrorReason::invalid_argument},
                                 "SC16 import requires a sample rate (filename hint or override)");
  }
  data::SignalDescriptor d;
  d.signal_kind = data::SignalKind::complex;
  d.scalar_type = data::ScalarType::int16;
  d.component_layout = data::ComponentLayout::interleaved;
  d.component_order = data::ComponentOrder::iq;
  d.endianness = data::Endianness::little;
  d.sample_rate_hz = override_sample_rate_hz.value_or(hint.sample_rate_hz);
  if (override_center_frequency_hz) {
    d.center_frequency_hz = *override_center_frequency_hz;
  } else if (hint.had_center_frequency) {
    d.center_frequency_hz = hint.center_frequency_hz;
  }
  return import_raw(path, d);
}

core::Result<data::SignalSlice> Application::read_samples(const data::FileDataSource& source, std::uint64_t begin,
                                                          std::uint64_t count, std::uint64_t maximum_read_bytes,
                                                          std::function<bool()> cancel) {
  auto range = data::SampleRange::from_count(begin, count);
  if (!range.ok()) {
    return core::Status(range.error());
  }
  data::ReadRequest req;
  req.range = *range;
  req.maximum_read_bytes = maximum_read_bytes;
  req.cancellation_requested = std::move(cancel);
  auto r = source.read(req);
  if (!r.ok()) {
    return core::Status(r.error());
  }
  return r->samples.view();
}

core::Result<dsp::PsdResult> Application::analyze_psd(const data::SignalSlice& slice, double sample_rate_hz,
                                                      std::uint64_t nfft, std::uint64_t overlap_samples) {
  if (!psd_) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::dsp, core::ErrorReason::unavailable},
                                 "no FFT backend available (CUDA required for PSD in this environment)");
  }
  dsp::PsdRequest req;
  req.window = dsp::WindowType::hann;
  req.nfft = nfft;
  req.overlap_samples = overlap_samples;
  req.scaling = dsp::SpectrumScaling::density;
  req.one_sided = (slice.kind() == data::SignalKind::real);
  return psd_->process(slice, sample_rate_hz, req);
}

core::Result<dsp::StftResult> Application::analyze_stft(const data::SignalSlice& slice, double sample_rate_hz,
                                                        std::uint64_t nfft, std::uint64_t hop_samples) {
  if (!stft_) {
    return core::Status::failure(core::ErrorCode{core::ErrorDomain::dsp, core::ErrorReason::unavailable},
                                 "no FFT backend available (CUDA required for STFT in this environment)");
  }
  dsp::StftRequest req;
  req.window = dsp::WindowType::hann;
  req.nfft = nfft;
  req.hop_samples = hop_samples;
  req.output_magnitude = true;
  return stft_->process(slice, sample_rate_hz, req);
}

core::Result<NarrowbandChannel> Application::extract_narrowband(const data::SignalSlice& wideband,
                                                                double sample_rate_hz,
                                                                const NarrowbandChannelSpec& spec,
                                                                std::uint64_t source_start_sample) {
  return extract_narrowband_channel(wideband, sample_rate_hz, spec, source_start_sample);
}

} // namespace signal::studio
