#include "application.hpp"

#include "signal_studio/dsp/analysis.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <regex>
#include <sstream>
#include <system_error>

namespace signal::studio {
namespace {

[[nodiscard]] core::Status failure(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::core, reason}, std::move(message), std::move(diagnostic));
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
  const auto text = path.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

[[nodiscard]] std::filesystem::path utf8_path(std::string_view value) {
  const auto* begin = reinterpret_cast<const char8_t*>(value.data());
  return std::filesystem::path{std::u8string{begin, begin + value.size()}};
}

[[nodiscard]] std::vector<std::byte> as_bytes(std::string_view text) {
  std::vector<std::byte> result(text.size());
  if (!text.empty()) {
    std::memcpy(result.data(), text.data(), text.size());
  }
  return result;
}

[[nodiscard]] task::RuntimeConfig runtime_config(const std::filesystem::path& state_directory) {
  std::error_code error;
  std::filesystem::create_directories(state_directory, error);
  return {.worker_count = 2U,
          .budget = {.cpu_units = 2U, .io_units = 1U, .gpu_units = 1U, .runtime_threads = 2U},
          .history_file = state_directory / "task-history.log"};
}

[[nodiscard]] task::FailureInfo task_failure(const core::Status& status, std::string action) {
  return {status.code().stable_text(),
          std::string{status.message()},
          std::string{status.diagnostic()},
          std::move(action),
          true,
          "retry",
          "log://signal-studio/import"};
}

[[nodiscard]] double unit_multiplier(std::string_view unit) {
  if (unit == "GHz" || unit == "GSps") {
    return 1.0e9;
  }
  if (unit == "MHz" || unit == "MSps") {
    return 1.0e6;
  }
  if (unit == "kHz" || unit == "kSps") {
    return 1.0e3;
  }
  return 1.0;
}

[[nodiscard]] std::optional<double> parse_named_value(std::string_view filename, const std::regex& pattern) {
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_search(filename.begin(), filename.end(), match, pattern) || match.size() != 3U) {
    return std::nullopt;
  }
  double value{};
  const auto number = std::string{match[1].first, match[1].second};
  const auto converted = std::from_chars(number.data(), number.data() + number.size(), value);
  if (converted.ec != std::errc{} || converted.ptr != number.data() + number.size()) {
    return std::nullopt;
  }
  return value * unit_multiplier(std::string_view{match[2].first, match[2].second});
}

[[nodiscard]] std::string artifact_id(std::string_view prefix) {
  return std::string{prefix} + "-" + task::TaskId::generate().value;
}

[[nodiscard]] std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
  return right > std::numeric_limits<std::uint64_t>::max() - left ? std::numeric_limits<std::uint64_t>::max()
                                                                  : left + right;
}

template <typename Value> [[nodiscard]] std::uint64_t vector_bytes(const std::vector<Value>& values) noexcept {
  if (values.capacity() > std::numeric_limits<std::uint64_t>::max() / sizeof(Value)) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return static_cast<std::uint64_t>(values.capacity()) * sizeof(Value);
}

[[nodiscard]] std::uint64_t analysis_bundle_bytes(const AnalysisBundle& bundle) noexcept {
  std::uint64_t bytes{sizeof(AnalysisBundle)};
  const auto add = [&](std::uint64_t value) { bytes = saturating_add(bytes, value); };
  add(vector_bytes(bundle.frame.time_primary));
  add(vector_bytes(bundle.frame.time_secondary));
  add(vector_bytes(bundle.frame.constellation_i));
  add(vector_bytes(bundle.frame.constellation_q));
  add(vector_bytes(bundle.frame.psd_db_hz));
  add(vector_bytes(bundle.frame.stft_db));
  add(vector_bytes(bundle.spectrum.frequency_hz));
  add(vector_bytes(bundle.spectrum.magnitude_dbfs));
  add(vector_bytes(bundle.spectrum.raw_linear_values));
  add(vector_bytes(bundle.spectrum.raw_amplitude_linear));
  add(vector_bytes(bundle.spectrum.raw_power_linear));
  add(vector_bytes(bundle.spectrum.raw_density_linear));
  add(vector_bytes(bundle.spectrum.raw_values));
  add(vector_bytes(bundle.spectrum.values));
  add(vector_bytes(bundle.psd.frequency_hz));
  add(vector_bytes(bundle.psd.db_per_hz));
  add(vector_bytes(bundle.psd.raw_linear_values));
  add(vector_bytes(bundle.psd.raw_density_linear));
  add(vector_bytes(bundle.psd.raw_values));
  add(vector_bytes(bundle.psd.values));
  add(vector_bytes(bundle.psd.raw_db_per_hz));
  add(vector_bytes(bundle.stft.time_seconds));
  add(vector_bytes(bundle.stft.frequency_hz));
  add(vector_bytes(bundle.stft.db_per_hz));
  add(vector_bytes(bundle.stft.raw_linear_values));
  add(vector_bytes(bundle.stft.raw_density_linear));
  add(vector_bytes(bundle.stft.raw_values));
  add(vector_bytes(bundle.stft.values));
  add(vector_bytes(bundle.stft.raw_db_per_hz));
  return bytes;
}

[[nodiscard]] std::string_view quantity_unit(dsp::SpectrumOutputQuantity quantity) noexcept {
  switch (quantity) {
  case dsp::SpectrumOutputQuantity::magnitude_dbfs:
  case dsp::SpectrumOutputQuantity::power_dbfs:
    return "dBFS";
  case dsp::SpectrumOutputQuantity::psd_dbfs_per_hz:
    return "dBFS/Hz";
  case dsp::SpectrumOutputQuantity::linear_amplitude:
    return "FS";
  case dsp::SpectrumOutputQuantity::linear_power:
    return "FS^2";
  case dsp::SpectrumOutputQuantity::linear_power_density:
    return "FS^2/Hz";
  }
  return "unknown";
}

[[nodiscard]] bool quantity_is_logarithmic(dsp::SpectrumOutputQuantity quantity) noexcept {
  return quantity == dsp::SpectrumOutputQuantity::magnitude_dbfs ||
         quantity == dsp::SpectrumOutputQuantity::power_dbfs ||
         quantity == dsp::SpectrumOutputQuantity::psd_dbfs_per_hz;
}

[[nodiscard]] bool quantity_is_amplitude(dsp::SpectrumOutputQuantity quantity) noexcept {
  return quantity == dsp::SpectrumOutputQuantity::magnitude_dbfs ||
         quantity == dsp::SpectrumOutputQuantity::linear_amplitude;
}

[[nodiscard]] data::SignalSlice bounded_slice(const ImportedSignal& imported, std::uint64_t maximum_samples) {
  const auto available = imported.loaded->samples();
  const auto count = std::min(available.size(), maximum_samples);
  auto sliced = available.slice(0U, count);
  return sliced ? sliced.value() : available;
}

[[nodiscard]] core::Result<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path,
                                                                     std::uint64_t maximum_bytes) {
  return core::AtomicFileStore{}.read(path, maximum_bytes);
}

[[nodiscard]] std::string source_link_text(const ImportedSignal& imported) {
  std::ostringstream output;
  output << "signal-source-link/1.0\n"
         << std::quoted(path_utf8(imported.source_path)) << '\n'
         << imported.fingerprint.version_id << '\n'
         << imported.fingerprint.size_bytes << '\n'
         << imported.fingerprint.modified_ticks << '\n'
         << imported.fingerprint.sampled_hash.hex() << '\n';
  return output.str();
}

[[nodiscard]] core::Result<std::filesystem::path> parse_source_link(const std::filesystem::path& link_path,
                                                                    std::string_view expected_version) {
  auto content = core::AtomicFileStore{}.read(link_path, 1024U * 1024U);
  if (!content) {
    return content.error();
  }
  std::istringstream input{std::string{reinterpret_cast<const char*>(content.value().data()), content.value().size()}};
  std::string signature;
  std::string source_path;
  std::string version;
  std::getline(input, signature);
  input >> std::quoted(source_path) >> version;
  if (signature != "signal-source-link/1.0" || source_path.empty() || version != expected_version) {
    return failure(core::ErrorReason::invalid_argument, "数据源链接损坏或版本不匹配", link_path.string());
  }
  auto fingerprint = core::fingerprint_source(utf8_path(source_path));
  if (!fingerprint || fingerprint.value().version_id != version) {
    return failure(core::ErrorReason::unavailable, "外部数据源已变化，必须重新确认版本", source_path);
  }
  return utf8_path(source_path);
}

[[nodiscard]] std::string scalar_type_label(data::ScalarType type) {
  switch (type) {
  case data::ScalarType::int8:
    return "Int8";
  case data::ScalarType::uint8:
    return "UInt8";
  case data::ScalarType::int16:
    return "Int16";
  case data::ScalarType::uint16:
    return "UInt16";
  case data::ScalarType::int24_packed:
    return "Int24 Packed";
  case data::ScalarType::int32:
    return "Int32";
  case data::ScalarType::float32:
    return "Float32";
  case data::ScalarType::float64:
    return "Float64";
  }
  return "未知";
}

[[nodiscard]] std::string json_string(std::string_view value) {
  std::string output{"\""};
  output.reserve(value.size() + 2U);
  for (const auto character : value) {
    switch (character) {
    case '\\':
      output += "\\\\";
      break;
    case '"':
      output += "\\\"";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      output.push_back(character);
      break;
    }
  }
  output.push_back('"');
  return output;
}

[[nodiscard]] core::Result<std::string> decode_extension_string(std::string_view value) {
  if (value.size() < 2U || value.front() != '"' || value.back() != '"') {
    return std::string{value};
  }
  std::string output;
  output.reserve(value.size() - 2U);
  for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
    auto character = value[index];
    if (character != '\\') {
      output.push_back(character);
      continue;
    }
    if (++index + 1U > value.size()) {
      return failure(core::ErrorReason::invalid_argument, "工程扩展字符串转义不完整");
    }
    switch (value[index]) {
    case '\\':
    case '"':
      output.push_back(value[index]);
      break;
    case 'n':
      output.push_back('\n');
      break;
    case 'r':
      output.push_back('\r');
      break;
    case 't':
      output.push_back('\t');
      break;
    default:
      return failure(core::ErrorReason::invalid_argument, "工程扩展字符串含不支持的转义");
    }
  }
  return output;
}

[[nodiscard]] std::string serialize_display_settings(const AnalysisDisplaySettings& settings) {
  std::ostringstream output;
  output << "signal.analysis-display/1.0\n"
         << std::setprecision(17) << static_cast<unsigned>(settings.mapping.amplitude_scale) << ' '
         << static_cast<unsigned>(settings.mapping.range_mode) << ' ' << settings.mapping.minimum << ' '
         << settings.mapping.maximum << ' ' << settings.mapping.reference_level << ' ' << settings.mapping.dynamic_range
         << ' ' << std::quoted(settings.mapping.color_map) << ' ' << std::quoted(settings.interpolation) << ' '
         << std::quoted(settings.frequency_axis_mode) << '\n';
  return output.str();
}

[[nodiscard]] core::Result<AnalysisDisplaySettings> parse_display_settings(std::string_view text) {
  std::istringstream input{std::string{text}};
  AnalysisDisplaySettings result;
  std::getline(input, result.schema);
  unsigned amplitude_scale{};
  unsigned range_mode{};
  if (result.schema.rfind("signal.analysis-display/1.", 0U) != 0U ||
      !(input >> amplitude_scale >> range_mode >> result.mapping.minimum >> result.mapping.maximum >>
        result.mapping.reference_level >> result.mapping.dynamic_range >> std::quoted(result.mapping.color_map) >>
        std::quoted(result.interpolation) >> std::quoted(result.frequency_axis_mode)) ||
      amplitude_scale > static_cast<unsigned>(visualization::AmplitudeScale::logarithmic) ||
      range_mode > static_cast<unsigned>(visualization::RangeMode::manual)) {
    return failure(core::ErrorReason::invalid_argument, "分析显示参数损坏或版本不兼容");
  }
  result.mapping.amplitude_scale = static_cast<visualization::AmplitudeScale>(amplitude_scale);
  result.mapping.range_mode = static_cast<visualization::RangeMode>(range_mode);
  if (const auto status = visualization::validate_display_mapping(result.mapping); !status) {
    return status;
  }
  if (result.interpolation != "nearest" && result.interpolation != "linear") {
    return failure(core::ErrorReason::invalid_argument, "STFT 插值只允许 nearest 或 linear");
  }
  return result;
}

[[nodiscard]] std::uint64_t adaptive_power_of_two(std::uint64_t available, std::uint64_t preferred) {
  const auto limit = std::max<std::uint64_t>(2U, std::min(available, preferred));
  std::uint64_t value{1U};
  while (value <= limit / 2U) {
    value *= 2U;
  }
  return std::max<std::uint64_t>(2U, value);
}

[[nodiscard]] dsp::AnalysisSettingsSnapshot adaptive_settings(const ImportedSignal& imported, std::string_view preset,
                                                              bool prefer_cuda = false,
                                                              std::uint64_t viewport_samples = 0U) {
  const auto loaded = imported.loaded ? imported.loaded->samples().size() : 0U;
  const auto available = viewport_samples == 0U ? loaded : std::min(loaded, viewport_samples);
  dsp::AnalysisSettingsSnapshot settings;
  const auto complex = imported.descriptor.signal_kind == data::SignalKind::complex;
  settings.spectrum.sidedness = complex ? dsp::SpectrumSidedness::two_sided_shifted : dsp::SpectrumSidedness::one_sided;
  settings.spectrum.frequency_reference =
      imported.descriptor.center_frequency_hz ? data::FrequencyReference::absolute : data::FrequencyReference::baseband;
  settings.spectrum.output_quantity = dsp::SpectrumOutputQuantity::psd_dbfs_per_hz;
  settings.spectrum.normalization = dsp::SpectrumNormalization::window_power;
  settings.spectrum.analysis_range_policy = dsp::AnalysisRangePolicy::all_complete_frames;
  settings.spectrogram.sidedness = settings.spectrum.sidedness;
  settings.spectrogram.output_quantity = dsp::SpectrumOutputQuantity::psd_dbfs_per_hz;
  settings.spectrogram.normalization = dsp::SpectrumNormalization::window_power;

  std::uint64_t spectrum_preferred{4096U};
  std::uint64_t stft_preferred{1024U};
  if (preset == "quick-preview") {
    spectrum_preferred = 1024U;
    stft_preferred = 256U;
  } else if (preset == "high-resolution") {
    spectrum_preferred = 65'536U;
    stft_preferred = 4096U;
  } else if (preset == "low-noise-psd") {
    spectrum_preferred = 8192U;
    stft_preferred = 1024U;
  } else if (preset == "burst-signal") {
    spectrum_preferred = 2048U;
    stft_preferred = 256U;
  } else if (preset == "narrowband-fine") {
    spectrum_preferred = 131'072U;
    stft_preferred = 8192U;
  }
  const auto sample_rate_scale = std::clamp(imported.descriptor.sample_rate_hz / 1.0e6, 0.25, 16.0);
  const auto backend_scale = prefer_cuda ? 2.0 : 1.0;
  spectrum_preferred = static_cast<std::uint64_t>(
      std::llround(static_cast<double>(spectrum_preferred) * sample_rate_scale * backend_scale));
  stft_preferred =
      static_cast<std::uint64_t>(std::llround(static_cast<double>(stft_preferred) * sample_rate_scale * backend_scale));
  settings.spectrum.frame_length = adaptive_power_of_two(available, spectrum_preferred);
  settings.spectrum.fft_length = settings.spectrum.frame_length;
  settings.spectrogram.frame_length = adaptive_power_of_two(available, stft_preferred);
  settings.spectrogram.fft_length = settings.spectrogram.frame_length;
  settings.spectrogram.hop_length =
      std::max<std::uint64_t>(1U, settings.spectrogram.frame_length / (preset == "burst-signal" ? 8U : 4U));

  if (preset == "low-noise-psd" || preset == "narrowband-fine") {
    settings.spectrum.estimator = {dsp::PsdEstimatorKind::welch, 0.5, 16U};
    settings.spectrum.accumulation = {dsp::SpectrumAccumulationMode::linear_average, 16U, 1.0, 0U};
    settings.spectrum.window = {dsp::WindowKind::blackman_harris, 0.0};
  } else if (preset == "quick-preview") {
    settings.spectrum.estimator = {dsp::PsdEstimatorKind::periodogram, 0.0, 1U};
    settings.spectrum.accumulation = {};
  } else {
    settings.spectrum.estimator = {dsp::PsdEstimatorKind::welch, 0.5, 8U};
    settings.spectrum.accumulation = {dsp::SpectrumAccumulationMode::linear_average, 8U, 1.0, 0U};
  }
  if (preset == "burst-signal") {
    settings.spectrogram.window = {dsp::WindowKind::tukey, 0.25};
  }
  constexpr std::uint64_t host_budget = 768ULL * 1024ULL * 1024ULL;
  constexpr std::uint64_t device_budget = 384ULL * 1024ULL * 1024ULL;
  while (settings.spectrogram.frame_length >= 16U) {
    auto cost = dsp::estimate_analysis_cost(settings, available, imported.descriptor.sample_rate_hz, host_budget,
                                            device_budget);
    if (cost && cost.value().within_host_budget && (!prefer_cuda || cost.value().within_device_budget)) {
      break;
    }
    if (settings.spectrogram.hop_length < settings.spectrogram.frame_length) {
      settings.spectrogram.hop_length =
          std::min(settings.spectrogram.frame_length, settings.spectrogram.hop_length * 2U);
      continue;
    }
    if (settings.spectrogram.frame_length == 16U) {
      break;
    }
    settings.spectrogram.frame_length /= 2U;
    settings.spectrogram.fft_length = settings.spectrogram.frame_length;
    settings.spectrogram.hop_length =
        std::max<std::uint64_t>(1U, settings.spectrogram.frame_length / (preset == "burst-signal" ? 8U : 4U));
    if (settings.spectrum.frame_length > settings.spectrogram.frame_length * 16U) {
      settings.spectrum.frame_length /= 2U;
      settings.spectrum.fft_length = settings.spectrum.frame_length;
    }
  }
  return settings;
}

[[nodiscard]] std::string window_label(const dsp::WindowSpecification& specification) {
  switch (specification.kind) {
  case dsp::WindowKind::rectangular:
    return "Rectangular";
  case dsp::WindowKind::hann:
    return "Hann";
  case dsp::WindowKind::hamming:
    return "Hamming";
  case dsp::WindowKind::blackman:
    return "Blackman";
  case dsp::WindowKind::blackman_harris:
    return "Blackman-Harris";
  case dsp::WindowKind::flat_top:
    return "Flat Top";
  case dsp::WindowKind::kaiser:
    return "Kaiser(beta=" + std::to_string(specification.parameter) + ')';
  case dsp::WindowKind::tukey:
    return "Tukey(alpha=" + std::to_string(specification.parameter) + ')';
  }
  return "Unknown";
}

} // namespace

struct ImportTask::SharedState final {
  mutable std::mutex mutex;
  std::string project_id;
  std::uint64_t project_generation{};
  std::optional<ImportedSignal> imported;
  std::optional<core::Status> error;
};

struct ApplicationController::RestoredAnalysisExtensions final {
  dsp::AnalysisSettingsSnapshot settings;
  AnalysisDisplaySettings display;
  std::map<std::string, dsp::AnalysisSettingsSnapshot, std::less<>> user_presets;
  std::string scope{"project-view"};
  std::string active_preset;
  std::string active_preset_hash;
};

FilenameHints parse_filename_hints(const std::filesystem::path& path) {
  FilenameHints hints;
  const auto filename = path.filename().string();
  static const std::regex sample_rate_pattern{R"((?:^|[_-])sr([0-9]+(?:\.[0-9]+)?)(GSps|MSps|kSps|Sps)(?:[_\.-]|$))",
                                              std::regex::icase};
  static const std::regex center_frequency_pattern{R"((?:^|[_-])cf([0-9]+(?:\.[0-9]+)?)(GHz|MHz|kHz|Hz)(?:[_\.-]|$))",
                                                   std::regex::icase};
  hints.sample_rate_hz = parse_named_value(filename, sample_rate_pattern);
  hints.center_frequency_hz = parse_named_value(filename, center_frequency_pattern);
  const auto extension = path.extension().string();
  if (extension == ".sc16" || extension == ".SC16") {
    hints.scalar_type = data::ScalarType::int16;
    hints.signal_kind = data::SignalKind::complex;
    hints.evidence.push_back(".sc16 仅作为待确认的复数 Int16 格式提示");
  }
  if (hints.sample_rate_hz) {
    hints.evidence.push_back("文件名包含 sr 采样率提示，未确认前不参与数值解释");
  }
  if (hints.center_frequency_hz) {
    hints.evidence.push_back("文件名包含 cf 中心频率提示，未确认前不参与数值解释");
  }
  return hints;
}

core::Result<data::SignalDescriptor>
make_confirmed_descriptor(const std::filesystem::path& path, const FilenameHints& hints, bool confirm_filename_hints) {
  std::error_code error;
  const auto file_bytes = std::filesystem::file_size(path, error);
  if (error || file_bytes == 0U) {
    return failure(core::ErrorReason::unavailable, "导入文件不存在或为空", error.message());
  }
  if (!confirm_filename_hints || !hints.sample_rate_hz || !hints.scalar_type || !hints.signal_kind) {
    return failure(core::ErrorReason::invalid_argument, "文件名提示必须由用户明确确认后才能生成 SignalDescriptor");
  }
  data::SignalDescriptor descriptor;
  descriptor.signal_kind = *hints.signal_kind;
  descriptor.scalar_type = *hints.scalar_type;
  descriptor.component_layout = descriptor.signal_kind == data::SignalKind::complex ? data::ComponentLayout::interleaved
                                                                                    : data::ComponentLayout::real;
  descriptor.component_order = descriptor.signal_kind == data::SignalKind::complex
                                   ? data::ComponentOrder::iq
                                   : data::ComponentOrder::not_applicable;
  descriptor.endianness =
      descriptor.scalar_type == data::ScalarType::int8 || descriptor.scalar_type == data::ScalarType::uint8
          ? data::Endianness::not_applicable
          : data::Endianness::little;
  descriptor.sample_rate_hz = *hints.sample_rate_hz;
  descriptor.center_frequency_hz = hints.center_frequency_hz;
  descriptor.amplitude_mode = descriptor.scalar_type == data::ScalarType::int16 ? "int16_scaled" : "linear";
  descriptor.scale_factor = descriptor.scalar_type == data::ScalarType::int16 ? 1.0 / 32768.0 : 1.0;
  auto frame_bytes = descriptor.frame_bytes();
  if (!frame_bytes || file_bytes % frame_bytes.value() != 0U) {
    return failure(core::ErrorReason::invalid_argument, "文件大小与确认后的完整样本帧不对齐");
  }
  auto range = data::SampleRange::from_count(0U, file_bytes / frame_bytes.value());
  if (!range) {
    return range.error();
  }
  descriptor.requested_sample_range = range.value();
  for (const auto* field : {"signalKind", "scalarType", "componentLayout", "componentOrder", "endianness",
                            "sampleRateHz", "centerFrequencyHz"}) {
    descriptor.provenance.emplace(field, data::FieldProvenance{data::FieldOrigin::filename_hint, true});
  }
  if (const auto status = descriptor.validate(); !status) {
    return status;
  }
  return descriptor;
}

ImportTask::ImportTask(task::TaskHandle handle, std::shared_ptr<SharedState> state)
    : handle_(std::move(handle)), state_(std::move(state)) {}

bool ImportTask::valid() const noexcept {
  return handle_.valid() && state_ != nullptr;
}

const task::TaskHandle& ImportTask::handle() const noexcept {
  return handle_;
}

core::Result<ImportedSignal> ImportTask::result() const {
  if (!valid()) {
    return failure(core::ErrorReason::invalid_argument, "导入任务句柄无效");
  }
  std::lock_guard lock{state_->mutex};
  if (state_->imported) {
    return *state_->imported;
  }
  if (state_->error) {
    return *state_->error;
  }
  return failure(core::ErrorReason::unavailable, "导入任务尚未产生可用结果");
}

ApplicationController::ApplicationController(std::filesystem::path state_directory)
    : state_directory_(std::move(state_directory)), recent_projects_(state_directory_ / "recent-projects.txt"),
      task_runtime_(runtime_config(state_directory_)) {}

ApplicationController::~ApplicationController() {
  task_runtime_.shutdown();
}

core::Status ApplicationController::create_project(const std::filesystem::path& project_path, std::string project_id) {
  if (project_path.empty() || project_path.extension() != ".signal-workspace") {
    return failure(core::ErrorReason::invalid_argument, "项目路径必须使用 .signal-workspace 扩展名");
  }
  auto created = workspace_store_.create(std::move(project_id));
  if (!created) {
    return created.error();
  }
  auto candidate_artifact_store = std::make_unique<core::ArtifactStore>(
      project_path.parent_path() / (project_path.stem().string() + ".assets") / "artifacts");
  if (const auto status = candidate_artifact_store->recover(); !status) {
    return status;
  }
  auto candidate_workspace = std::move(created).value();
  dsp::AnalysisSettingsSnapshot candidate_settings;
  AnalysisDisplaySettings candidate_display;
  if (auto serialized = dsp::serialize_analysis_settings(candidate_settings); serialized) {
    candidate_workspace.extensions["signal.analysis-settings"] = json_string(serialized.value());
  } else {
    return serialized.error();
  }
  candidate_workspace.extensions["signal.analysis-display"] =
      json_string(serialize_display_settings(candidate_display));
  candidate_workspace.extensions["signal.analysis-scope"] = json_string("project-view");
  candidate_workspace.extensions["signal.analysis-active-preset"] = json_string("");
  candidate_workspace.extensions["signal.analysis-active-preset-hash"] = json_string("");
  if (const auto status = workspace_store_.save(project_path, candidate_workspace); !status) {
    return status;
  }
  if (const auto status = recent_projects_.record(project_path); !status) {
    return status;
  }
  static_cast<void>(task_runtime_.issue_view_request("signal-studio.analysis"));
  {
    std::lock_guard state_lock{analysis_state_mutex_};
    workspace_ = std::move(candidate_workspace);
    project_path_ = project_path;
    artifact_store_ = std::move(candidate_artifact_store);
    ++project_generation_;
    current_signal_.reset();
    current_analysis_.reset();
    analysis_settings_ = {};
    analysis_display_settings_ = {};
    user_analysis_presets_.clear();
    analysis_cache_.clear();
    analysis_cache_bytes_ = 0U;
    analysis_cache_sequence_ = 0U;
    analysis_scope_ = "project-view";
    active_analysis_preset_.clear();
    active_analysis_preset_hash_.clear();
  }
  return core::Status::success();
}

core::Status ApplicationController::open_project(const std::filesystem::path& project_path, bool read_only) {
  auto loaded = workspace_store_.load(project_path, read_only);
  if (!loaded) {
    return loaded.error();
  }
  auto candidate_workspace = std::move(loaded).value();
  auto restored = restore_analysis_extensions(candidate_workspace);
  if (!restored) {
    return restored.error();
  }
  auto candidate_artifact_store = std::make_unique<core::ArtifactStore>(
      project_path.parent_path() / (project_path.stem().string() + ".assets") / "artifacts");
  if (const auto status = candidate_artifact_store->recover(); !status) {
    return status;
  }
  if (!candidate_workspace.data_sources.empty()) {
    const auto& source = candidate_workspace.data_sources.front();
    auto relative = core::resolve_relative_resource(project_path, utf8_path(source.relative_uri));
    if (!relative) {
      return relative.error();
    }
    auto resolved = parse_source_link(relative.value(), source.version_id);
    if (!resolved) {
      return resolved.error();
    }
  }
  if (!read_only) {
    if (const auto status = recent_projects_.record(project_path); !status) {
      return status;
    }
  }
  if (!candidate_workspace.data_sources.empty()) {
    const auto& source = candidate_workspace.data_sources.front();
    const auto context_status =
        current_context_.switch_to({candidate_workspace.project_id, source.id, source.version_id, 0U});
    if (!context_status) {
      return context_status;
    }
  }
  static_cast<void>(task_runtime_.issue_view_request("signal-studio.analysis"));
  {
    std::lock_guard state_lock{analysis_state_mutex_};
    workspace_ = std::move(candidate_workspace);
    project_path_ = project_path;
    artifact_store_ = std::move(candidate_artifact_store);
    ++project_generation_;
    analysis_settings_ = std::move(restored.value().settings);
    analysis_display_settings_ = std::move(restored.value().display);
    user_analysis_presets_ = std::move(restored.value().user_presets);
    analysis_scope_ = std::move(restored.value().scope);
    active_analysis_preset_ = std::move(restored.value().active_preset);
    active_analysis_preset_hash_ = std::move(restored.value().active_preset_hash);
    current_signal_.reset();
    current_analysis_.reset();
    analysis_cache_.clear();
    analysis_cache_bytes_ = 0U;
    analysis_cache_sequence_ = 0U;
  }
  return core::Status::success();
}

core::Status ApplicationController::save_project() {
  std::lock_guard state_lock{analysis_state_mutex_};
  if (project_path_.empty()) {
    return failure(core::ErrorReason::unavailable, "当前没有可保存项目");
  }
  persist_analysis_extensions();
  return workspace_store_.save(project_path_, workspace_);
}

core::Status ApplicationController::close_project() {
  static_cast<void>(task_runtime_.issue_view_request("signal-studio.analysis"));
  std::lock_guard state_lock{analysis_state_mutex_};
  ++project_generation_;
  current_signal_.reset();
  current_analysis_.reset();
  analysis_cache_.clear();
  analysis_cache_bytes_ = 0U;
  analysis_cache_sequence_ = 0U;
  artifact_store_.reset();
  project_path_.clear();
  return workspace_store_.close(workspace_);
}

core::Result<ImportTask> ApplicationController::start_import(ImportRequest request) {
  std::string project_id;
  std::uint64_t project_generation{};
  {
    std::lock_guard state_lock{analysis_state_mutex_};
    project_id = workspace_.project_id;
    project_generation = project_generation_;
  }
  if (project_id.empty()) {
    return failure(core::ErrorReason::unavailable, "导入前必须先新建或打开项目");
  }
  if (request.path.empty() || request.initial_bytes == 0U || request.chunk_bytes == 0U) {
    return failure(core::ErrorReason::invalid_argument, "导入路径或有界读取参数无效");
  }
  if (const auto status = request.descriptor.validate(); !status) {
    return status;
  }
  auto state = std::make_shared<ImportTask::SharedState>();
  state->project_id = project_id;
  state->project_generation = project_generation;
  task::TaskSpec spec;
  spec.task_id = task::TaskId::generate();
  spec.task_type = "signal-studio.import";
  spec.priority = task::TaskPriority::foreground;
  spec.resources = {.cpu_units = 1U, .io_units = 1U, .runtime_threads = 1U};
  spec.idempotency_key = path_utf8(request.path) + ":" +
                         std::to_string(request.descriptor.requested_sample_range.begin()) + ":" +
                         std::to_string(request.initial_bytes) + ":attempt:" + spec.task_id.value;
  spec.provenance = {{project_id}, {"pending-import"}, {"data-source", path_utf8(request.path)}};
  spec.timeout = std::chrono::minutes{30};
  spec.max_attempts = 2U;
  auto submitted = task_runtime_.submit(spec, [request = std::move(request),
                                               state](task::TaskContext& context) mutable {
    const auto fail = [&](core::Status status) {
      {
        std::lock_guard lock{state->mutex};
        state->error = status;
      }
      return task::TaskExecutionResult::failed(task_failure(status, "修改导入参数、确认格式后重试"));
    };
    auto fingerprint = core::fingerprint_source(request.path);
    if (!fingerprint) {
      return fail(fingerprint.error());
    }
    auto facts = data::calculate_data_facts(fingerprint.value().size_bytes, request.descriptor, request.initial_bytes);
    if (!facts) {
      return fail(facts.error());
    }
    auto plan = data::make_initial_load_plan(request.path, request.descriptor,
                                             request.descriptor.requested_sample_range.begin(), request.initial_bytes,
                                             request.chunk_bytes);
    if (!plan) {
      return fail(plan.error());
    }
    core::Result<std::shared_ptr<data::FileDataSource>> source =
        request.source_format == data::SourceFormat::wav
            ? data::FileDataSource::open_wav(request.path, fingerprint.value().version_id, true,
                                             request.descriptor.component_order)
            : data::FileDataSource::open_raw(request.path, request.descriptor, fingerprint.value().version_id);
    if (!source) {
      return fail(source.error());
    }
    if (request.source_format == data::SourceFormat::wav) {
      if (!context.checkpoint()) {
        return task::TaskExecutionResult::completed();
      }
      auto read = source.value()->read({plan.value().requested_range, plan.value().target_bytes,
                                        [&context] { return context.cancellation_requested(); }});
      if (!read) {
        return fail(read.error());
      }
      static_cast<void>(context.report_progress(1.0, "WAV 数据区已按容器偏移有界读取并发布"));
      ImportedSignal imported{request.path,
                              request.source_format,
                              request.descriptor,
                              fingerprint.value(),
                              facts.value(),
                              source.value(),
                              std::make_shared<const data::LoadedDataRange>(
                                  read.value().range, std::move(read.value().samples), fingerprint.value().version_id),
                              false};
      std::lock_guard lock{state->mutex};
      state->imported = std::move(imported);
      return task::TaskExecutionResult::completed();
    }
    auto loader =
        data::IncrementalLoader::create(fingerprint.value().version_id, request.path, request.descriptor, plan.value());
    if (!loader) {
      return fail(loader.error());
    }
    for (;;) {
      if (!context.checkpoint()) {
        const auto cancel_status = loader.value()->cancel_import();
        if (!cancel_status) {
          return fail(cancel_status);
        }
        const auto snapshot = loader.value()->snapshot();
        if (snapshot.published_range) {
          ImportedSignal partial{request.path,  request.source_format, request.descriptor,       fingerprint.value(),
                                 facts.value(), source.value(),        snapshot.published_range, true};
          std::lock_guard lock{state->mutex};
          state->imported = std::move(partial);
        }
        return task::TaskExecutionResult::completed();
      }
      const auto step = loader.value()->process_next();
      if (!step) {
        return fail(step);
      }
      const auto snapshot = loader.value()->snapshot();
      const auto denominator = std::max<std::uint64_t>(1U, plan.value().target_bytes);
      static_cast<void>(context.report_progress(
          std::min(1.0, static_cast<double>(snapshot.bytes_read) / static_cast<double>(denominator)),
          snapshot.has_remaining_chunks ? "正在分块读取并校验完整样本帧" : "初始可浏览范围已发布"));
      if (!snapshot.has_remaining_chunks) {
        if (!snapshot.published_range) {
          return fail(failure(core::ErrorReason::internal_failure, "导入完成但没有发布可浏览数据"));
        }
        ImportedSignal imported{request.path,  request.source_format, request.descriptor,       fingerprint.value(),
                                facts.value(), source.value(),        snapshot.published_range, false};
        std::lock_guard lock{state->mutex};
        state->imported = std::move(imported);
        return task::TaskExecutionResult::completed();
      }
    }
  });
  if (!submitted) {
    return submitted.error();
  }
  return ImportTask{std::move(submitted).value(), std::move(state)};
}

core::Result<ImportedSignal> ApplicationController::finalize_import(const ImportTask& task) {
  if (!task.valid()) {
    return failure(core::ErrorReason::invalid_argument, "导入任务无效");
  }
  auto terminal = task.handle().wait();
  if (!terminal) {
    return terminal.error();
  }
  auto imported = task.result();
  if (!imported) {
    return imported.error();
  }
  std::lock_guard state_lock{analysis_state_mutex_};
  if (task.state_->project_id.empty() || task.state_->project_id != workspace_.project_id ||
      task.state_->project_generation != project_generation_) {
    return failure(core::ErrorReason::cancelled, "导入任务所属工程已切换，旧导入结果不会写入当前工程");
  }
  const auto source_id = "source-" + imported.value().fingerprint.version_id.substr(0U, 12U);
  if (const auto status = persist_source_link(imported.value(), source_id); !status) {
    return status;
  }
  auto descriptor_json = data::serialize_sidecar(imported.value().descriptor);
  if (!descriptor_json) {
    return descriptor_json.error();
  }
  const auto existing =
      std::ranges::find_if(workspace_.data_sources, [&](const auto& source) { return source.id == source_id; });
  const auto link_uri = project_path_.stem().string() + ".assets/resources/" + source_id + ".source-link";
  const auto loaded_range = imported.value().loaded->range();
  core::WorkspaceDataSource source{source_id,
                                   imported.value().fingerprint.version_id,
                                   link_uri,
                                   descriptor_json.value(),
                                   loaded_range.begin(),
                                   loaded_range.end(),
                                   true};
  if (existing == workspace_.data_sources.end()) {
    workspace_.data_sources.push_back(std::move(source));
  } else {
    *existing = std::move(source);
  }
  if (const auto status =
          current_context_.switch_to({workspace_.project_id, source_id, imported.value().fingerprint.version_id, 0U});
      !status) {
    return status;
  }
  current_signal_ = imported.value();
  if (analysis_settings_.spectrum.frame_length == 0U && analysis_settings_.spectrogram.frame_length == 0U) {
    analysis_settings_ = adaptive_settings(imported.value(), "balanced-analysis");
    persist_analysis_extensions();
  }
  persist_analysis_extensions();
  if (const auto status = workspace_store_.save(project_path_, workspace_); !status) {
    return status;
  }
  return imported.value();
}

core::Result<AnalysisBundle> ApplicationController::analyze(const ImportedSignal& imported, bool prefer_cuda) {
  if (!imported.loaded) {
    return failure(core::ErrorReason::invalid_argument, "分析要求已发布的样本范围");
  }
  auto settings = analysis_settings();
  const auto request = task_runtime_.issue_view_request("signal-studio.analysis");
  auto analyzed = analyze(imported, settings, prefer_cuda, nullptr, request, task::TaskId::generate().value);
  if (!analyzed) {
    return analyzed.error();
  }
  if (!commit_analysis(analyzed.value(), request)) {
    return failure(core::ErrorReason::cancelled, "分析结果已被更新的视图请求取代");
  }
  if (const auto status = set_analysis_settings(analyzed.value().settings); !status) {
    return status;
  }
  return analyzed.value();
}

core::Result<AnalysisBundle>
ApplicationController::analyze(const ImportedSignal& imported, const dsp::AnalysisSettingsSnapshot& settings,
                               bool prefer_cuda, std::shared_ptr<const std::atomic_bool> cancellation,
                               task::ViewRequestId view_request, std::string task_id, AnalysisViewSelection views) {
  if (!imported.loaded || imported.loaded->samples().size() < 2U) {
    return failure(core::ErrorReason::invalid_argument, "参数化分析至少需要 2 个已校验样本");
  }
  if (!views.spectrum && !views.spectrogram) {
    return failure(core::ErrorReason::invalid_argument, "至少必须启用频谱或时频图中的一个分析视图");
  }
  const auto started = std::chrono::steady_clock::now();
  const auto samples = imported.loaded->samples();
  std::string backend_reason;
  auto backend = dsp::make_auto_fft_backend(prefer_cuda, &backend_reason);
  if (!backend) {
    return backend.error();
  }
  auto kernels = dsp::make_auto_signal_kernel_backend();
  if (!kernels) {
    return kernels.error();
  }
  auto effective_settings = settings;
  if (effective_settings.prefilter.enabled && !effective_settings.prefilter.chain.nodes.empty()) {
    const auto preview_count = std::min<std::uint64_t>(samples.size(), 512U);
    auto preview_slice = samples.slice(0U, preview_count);
    if (!preview_slice) {
      return preview_slice.error();
    }
    auto preview = dsp::preview_node(*kernels.value(), preview_slice.value(), imported.descriptor,
                                     effective_settings.prefilter.chain.nodes.front());
    if (!preview) {
      return preview.error();
    }
    effective_settings.prefilter.backend_id = std::string{kernels.value()->backend_id()};
    effective_settings.prefilter.group_delay_samples = preview.value().group_delay_samples;
  }
  if (const auto status = dsp::validate_analysis_settings(effective_settings, samples.size(), imported.descriptor,
                                                          views.spectrum, views.spectrogram);
      !status) {
    return status;
  }
  auto settings_hash = dsp::hash_analysis_settings(effective_settings);
  if (!settings_hash) {
    return settings_hash.error();
  }
  auto cost = dsp::estimate_analysis_cost(effective_settings, samples.size(), imported.descriptor.sample_rate_hz,
                                          1024ULL * 1024ULL * 1024ULL, 512ULL * 1024ULL * 1024ULL, views.spectrum,
                                          views.spectrogram);
  if (!cost) {
    return cost.error();
  }
  if (!cost.value().within_host_budget || (prefer_cuda && !cost.value().within_device_budget)) {
    return failure(core::ErrorReason::unavailable, "分析参数超出内存预算",
                   "主机=" + std::to_string(cost.value().host_memory_bytes) +
                       " 字节，设备=" + std::to_string(cost.value().device_memory_bytes) + " 字节");
  }
  std::shared_ptr<const AnalysisBundle> previous_analysis;
  std::optional<ImportedSignal> previous_signal;
  AnalysisDisplaySettings display_settings;
  std::string project_id;
  std::uint64_t project_generation{};
  {
    std::lock_guard lock{analysis_state_mutex_};
    previous_analysis = current_analysis_;
    previous_signal = current_signal_;
    display_settings = analysis_display_settings_;
    project_id = workspace_.project_id;
    project_generation = project_generation_;
  }
  if (project_id.empty() || !previous_signal ||
      previous_signal->fingerprint.version_id != imported.fingerprint.version_id) {
    return failure(core::ErrorReason::cancelled, "分析数据源已不属于当前工程");
  }
  const auto descriptor_text = data::serialize_sidecar(imported.descriptor);
  if (!descriptor_text) {
    return descriptor_text.error();
  }
  auto descriptor_hash = core::hash_bytes(
      std::as_bytes(std::span<const char>{descriptor_text.value().data(), descriptor_text.value().size()}));
  if (!descriptor_hash) {
    return descriptor_hash.error();
  }
  std::ostringstream key_stream;
  const auto backend_policy = prefer_cuda ? "cuda-preferred" : "cpu-only";
  key_stream << "signal.analysis-cache/1.2|" << project_id << '|' << project_generation << '|'
             << imported.fingerprint.version_id << '|' << imported.loaded->range().begin() << ':'
             << imported.loaded->range().end() << '|' << descriptor_hash.value().hex() << '|'
             << effective_settings.algorithm_version << '|' << backend_policy << '|' << backend.value()->backend_id()
             << '|' << kernels.value()->backend_id() << '|' << settings_hash.value().stable_text() << '|'
             << static_cast<unsigned>(effective_settings.spectrum.output_quantity) << '|'
             << static_cast<unsigned>(effective_settings.spectrogram.output_quantity)
             << "|views:" << static_cast<unsigned>(views.spectrum) << static_cast<unsigned>(views.spectrogram)
             << "|raw+smoothed";
  auto cache_key = key_stream.str();
  auto invalidation = previous_analysis ? dsp::classify_analysis_change(previous_analysis->settings, effective_settings)
                                        : dsp::AnalysisInvalidation::spectrum_transform |
                                              dsp::AnalysisInvalidation::spectrogram_transform;
  {
    std::lock_guard lock{analysis_state_mutex_};
    if (const auto cached = analysis_cache_.find(cache_key); cached != analysis_cache_.end()) {
      auto bundle = *cached->second.bundle;
      cached->second.access_sequence = ++analysis_cache_sequence_;
      bundle.cache_hit = true;
      bundle.view_request = std::move(view_request);
      bundle.task_id = std::move(task_id);
      bundle.invalidation = invalidation;
      bundle.compute_duration =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
      return bundle;
    }
  }

  const auto loaded_range = imported.loaded->range();
  const auto can_reuse_current = previous_analysis && previous_signal &&
                                 previous_signal->fingerprint.version_id == imported.fingerprint.version_id &&
                                 previous_analysis->source_range == loaded_range &&
                                 previous_analysis->backend_policy == backend_policy &&
                                 previous_analysis->backend_id == backend.value()->backend_id() &&
                                 !dsp::has_invalidation(invalidation, dsp::AnalysisInvalidation::prefilter);
  const auto reuse_spectrum_transform =
      views.spectrum && can_reuse_current && !previous_analysis->psd.frequency_hz.empty() &&
      !dsp::has_invalidation(invalidation, dsp::AnalysisInvalidation::spectrum_transform);
  const auto reuse_spectrogram_transform =
      views.spectrogram && can_reuse_current && !previous_analysis->stft.frequency_hz.empty() &&
      !dsp::has_invalidation(invalidation, dsp::AnalysisInvalidation::spectrogram_transform);
  std::optional<data::SignalBuffer> filtered;
  if ((views.spectrum && !reuse_spectrum_transform) || (views.spectrogram && !reuse_spectrogram_transform)) {
    auto filtered_result = dsp::apply_analysis_prefilter(*kernels.value(), samples, imported.descriptor,
                                                         effective_settings.prefilter, cancellation);
    if (!filtered_result) {
      return filtered_result.error();
    }
    filtered = std::move(filtered_result.value());
  }
  const auto center_frequency = effective_settings.spectrum.frequency_reference == data::FrequencyReference::absolute
                                    ? imported.descriptor.center_frequency_hz.value_or(0.0)
                                    : 0.0;
  dsp::SpectrumResult spectrum;
  dsp::PsdResult psd;
  dsp::StftResult stft;
  if (views.spectrum) {
    auto spectrum_psd_result = [&]() -> core::Result<dsp::SpectrumPsdResult> {
      if (reuse_spectrum_transform) {
        dsp::SpectrumPsdResult reused{previous_analysis->spectrum, previous_analysis->psd};
        if (dsp::has_invalidation(invalidation, dsp::AnalysisInvalidation::spectrum_smoothing)) {
          auto resmoothed_spectrum =
              dsp::resmooth_spectrum(previous_analysis->spectrum, effective_settings.spectrum, cancellation);
          auto resmoothed_psd = dsp::resmooth_psd(previous_analysis->psd, effective_settings.spectrum, cancellation);
          if (!resmoothed_spectrum || !resmoothed_psd) {
            return !resmoothed_spectrum ? resmoothed_spectrum.error() : resmoothed_psd.error();
          }
          reused.spectrum = std::move(resmoothed_spectrum.value());
          reused.psd = std::move(resmoothed_psd.value());
        }
        return reused;
      }
      return dsp::calculate_spectrum_psd(*backend.value(), filtered->view(), imported.descriptor.sample_rate_hz,
                                         center_frequency, effective_settings.spectrum, cancellation);
    }();
    if (!spectrum_psd_result) {
      return spectrum_psd_result.error();
    }
    spectrum = std::move(spectrum_psd_result.value().spectrum);
    psd = std::move(spectrum_psd_result.value().psd);
    spectrum.settings_hash = settings_hash.value();
    spectrum.prefilter_applied = effective_settings.prefilter.enabled;
    psd.settings_hash = settings_hash.value();
    psd.prefilter_applied = effective_settings.prefilter.enabled;
  }
  if (views.spectrogram) {
    auto stft_result = [&]() -> core::Result<dsp::StftResult> {
      if (reuse_spectrogram_transform) {
        if (dsp::has_invalidation(invalidation, dsp::AnalysisInvalidation::spectrogram_smoothing)) {
          return dsp::resmooth_stft(previous_analysis->stft, effective_settings.spectrogram, cancellation);
        }
        return previous_analysis->stft;
      }
      return dsp::calculate_stft(*backend.value(), filtered->view(), imported.descriptor.sample_rate_hz,
                                 center_frequency, effective_settings.spectrogram, cancellation,
                                 imported.loaded->range().begin());
    }();
    if (!stft_result) {
      return stft_result.error();
    }
    stft = std::move(stft_result.value());
    stft.settings_hash = settings_hash.value();
    stft.prefilter_applied = effective_settings.prefilter.enabled;
  }
  const auto& frequency_axis = views.spectrum ? psd.frequency_hz : stft.frequency_hz;
  if (frequency_axis.size() < 2U) {
    return failure(core::ErrorReason::internal_failure, "启用的分析视图未产生有效频率轴");
  }
  const auto frequency_begin = static_cast<std::int64_t>(std::llround(frequency_axis.front()));
  const auto bin_width = frequency_axis[1] - frequency_axis[0];
  const auto frequency_end = static_cast<std::int64_t>(std::llround(frequency_axis.back() + std::max(1.0, bin_width)));
  auto frequency_range = visualization::make_frequency_range(frequency_begin, frequency_end);
  if (!frequency_range) {
    return frequency_range.error();
  }
  visualization::ViewportController controller{"signal-studio-p02"};
  if (!controller.bind_source(imported.fingerprint.version_id, loaded_range, frequency_range.value(),
                              imported.partial_read,
                              imported.loaded->samples().size() * imported.descriptor.frame_bytes().value(),
                              imported.fingerprint.size_bytes)) {
    return failure(core::ErrorReason::internal_failure, "无法绑定真实导入视口");
  }
  auto viewport_range = data::SampleRange::from_count(loaded_range.begin(), samples.size());
  if (!viewport_range || !controller.set_time(viewport_range.value())) {
    return failure(core::ErrorReason::internal_failure, "无法建立真实时间视窗");
  }
  visualization::VisualizationFrame frame;
  frame.request_id = controller.snapshot().request_id;
  frame.time_range = controller.snapshot().time_viewport;
  frame.frequency_range = frequency_range.value();
  frame.quality = imported.partial_read ? visualization::ViewQuality::preview : visualization::ViewQuality::refined;
  frame.time_mode = imported.descriptor.signal_kind == data::SignalKind::complex
                        ? visualization::TimeDisplayMode::in_phase_quadrature
                        : visualization::TimeDisplayMode::real;
  frame.spectrum_layout = imported.descriptor.signal_kind == data::SignalKind::complex
                              ? visualization::SpectrumLayout::shifted_two_sided
                              : visualization::SpectrumLayout::one_sided;
  constexpr std::size_t maximum_time_points = 1600U;
  const auto stride = std::max<std::uint64_t>(1U, samples.size() / maximum_time_points);
  if (samples.kind() == data::SignalKind::complex) {
    const auto values = samples.complex_values();
    for (std::size_t index = 0; index < values.size(); index += stride) {
      frame.time_primary.push_back(values[index].real);
      frame.time_secondary.push_back(values[index].imag);
    }
    const auto constellation_stride =
        std::max<std::size_t>(1U, values.size() / std::min<std::size_t>(4096U, values.size()));
    for (std::size_t index = 0; index < values.size(); index += constellation_stride) {
      frame.constellation_i.push_back(values[index].real);
      frame.constellation_q.push_back(values[index].imag);
    }
  } else {
    const auto values = samples.real_values();
    for (std::size_t index = 0; index < values.size(); index += stride) {
      frame.time_primary.push_back(values[index]);
    }
  }
  if (views.spectrum) {
    frame.psd_db_hz = psd.values;
    frame.psd_values_logarithmic = quantity_is_logarithmic(effective_settings.spectrum.output_quantity);
    frame.psd_values_amplitude = quantity_is_amplitude(effective_settings.spectrum.output_quantity);
    frame.psd_metadata = {psd.frame_length,
                          std::max<std::uint64_t>(1U, psd.segment_count),
                          static_cast<std::uint32_t>(psd.fft_length),
                          window_label(effective_settings.spectrum.window),
                          effective_settings.spectrum.estimator.kind == dsp::PsdEstimatorKind::welch ? "Welch"
                                                                                                     : "Periodogram",
                          psd.equivalent_noise_bandwidth_hz,
                          std::string{quantity_unit(effective_settings.spectrum.output_quantity)}};
  }
  if (views.spectrogram) {
    frame.stft_db = stft.values;
    frame.stft_values_logarithmic = quantity_is_logarithmic(effective_settings.spectrogram.output_quantity);
    frame.stft_values_amplitude = quantity_is_amplitude(effective_settings.spectrogram.output_quantity);
    frame.stft_rows = static_cast<std::uint32_t>(stft.rows);
    frame.stft_columns = static_cast<std::uint32_t>(stft.columns);
    frame.stft_metadata = {static_cast<std::uint32_t>(stft.frame_length),
                           static_cast<std::uint32_t>(stft.hop_length),
                           static_cast<std::uint32_t>(stft.fft_length),
                           dsp::spectrogram_overlap_ratio(effective_settings.spectrogram),
                           display_settings.mapping.color_map,
                           display_settings.interpolation,
                           std::string{quantity_unit(effective_settings.spectrogram.output_quantity)}};
  }
  frame.absolute_frequency = effective_settings.spectrum.frequency_reference == data::FrequencyReference::absolute &&
                             imported.descriptor.center_frequency_hz.has_value();
  frame.center_frequency_hz =
      static_cast<std::int64_t>(std::llround(imported.descriptor.center_frequency_hz.value_or(0.0)));
  frame.data_source_version_id = imported.fingerprint.version_id;
  auto inspector = workbench::make_inspector_channel_state("CH-01", "channel-v1", imported.fingerprint.version_id,
                                                           imported.descriptor);
  if (!inspector) {
    return inspector.error();
  }
  AnalysisBundle bundle;
  bundle.project_id = project_id;
  bundle.project_generation = project_generation;
  bundle.viewport = controller.snapshot();
  bundle.frame = std::move(frame);
  bundle.inspector = inspector.value();
  const auto& provenance = views.spectrum ? psd.provenance : stft.provenance;
  if (provenance.backend_id != backend.value()->backend_id()) {
    cache_key += "|actual-backend:" + provenance.backend_id;
  }
  bundle.backend_id = provenance.backend_id;
  bundle.device_id = provenance.device;
  bundle.backend_policy = backend_policy;
  bundle.settings = effective_settings;
  bundle.settings_hash = settings_hash.value();
  bundle.cost = cost.value();
  bundle.spectrum = std::move(spectrum);
  bundle.psd = std::move(psd);
  bundle.stft = std::move(stft);
  bundle.source_range = viewport_range.value();
  bundle.view_request = std::move(view_request);
  bundle.cache_key = cache_key;
  bundle.task_id = std::move(task_id);
  bundle.compute_duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  bundle.spectrum_transform_reused = reuse_spectrum_transform;
  bundle.spectrogram_transform_reused = reuse_spectrogram_transform;
  bundle.invalidation = invalidation;
  constexpr std::uint64_t cache_budget = 256ULL * 1024ULL * 1024ULL;
  const auto bundle_bytes = analysis_bundle_bytes(bundle);
  std::lock_guard state_lock{analysis_state_mutex_};
  if (workspace_.project_id != project_id || project_generation_ != project_generation || !current_signal_ ||
      current_signal_->fingerprint.version_id != imported.fingerprint.version_id) {
    return failure(core::ErrorReason::cancelled, "分析期间工程或数据源已切换，旧结果不会进入缓存");
  }
  if (bundle_bytes <= cache_budget) {
    if (const auto existing = analysis_cache_.find(cache_key); existing != analysis_cache_.end()) {
      analysis_cache_bytes_ -= std::min(analysis_cache_bytes_, existing->second.bytes);
      analysis_cache_.erase(existing);
    }
    while (!analysis_cache_.empty() && bundle_bytes > cache_budget - analysis_cache_bytes_) {
      const auto oldest =
          std::ranges::min_element(analysis_cache_, {}, [](const auto& entry) { return entry.second.access_sequence; });
      analysis_cache_bytes_ -= std::min(analysis_cache_bytes_, oldest->second.bytes);
      analysis_cache_.erase(oldest);
    }
    auto cached_bundle = std::make_shared<const AnalysisBundle>(bundle);
    analysis_cache_.emplace(cache_key,
                            AnalysisCacheEntry{std::move(cached_bundle), bundle_bytes, ++analysis_cache_sequence_});
    analysis_cache_bytes_ += bundle_bytes;
  }
  return bundle;
}

bool ApplicationController::commit_analysis(AnalysisBundle analysis, const task::ViewRequestId& request) {
  auto permit = task_runtime_.try_begin_view_commit(request);
  if (!permit) {
    return false;
  }
  analysis.view_request = request;
  std::lock_guard lock{analysis_state_mutex_};
  if (analysis.project_id.empty() || analysis.project_id != workspace_.project_id ||
      analysis.project_generation != project_generation_ || !current_signal_ ||
      current_signal_->fingerprint.version_id != analysis.frame.data_source_version_id) {
    return false;
  }
  current_analysis_ = std::make_shared<const AnalysisBundle>(std::move(analysis));
  return true;
}

core::Result<core::ArtifactRecord> ApplicationController::commit_measurement(const AnalysisBundle& analysis,
                                                                             std::string selection_id,
                                                                             std::string channel_id) {
  std::lock_guard state_lock{analysis_state_mutex_};
  if (!artifact_store_ || workspace_.project_id.empty() || analysis.project_id != workspace_.project_id ||
      analysis.project_generation != project_generation_ || !current_signal_ ||
      current_signal_->fingerprint.version_id != analysis.frame.data_source_version_id ||
      analysis.frame.psd_db_hz.empty()) {
    return failure(core::ErrorReason::unavailable, "当前没有可提交的分析结果");
  }
  const auto& measurement_values = analysis.settings.spectrum.measurement_source == dsp::MeasurementSource::raw
                                       ? analysis.psd.raw_db_per_hz
                                       : analysis.psd.db_per_hz;
  if (measurement_values.empty()) {
    return failure(core::ErrorReason::unavailable, "分析结果缺少可提交的测量来源");
  }
  const auto peak = *std::ranges::max_element(measurement_values);
  const auto mean = std::accumulate(measurement_values.begin(), measurement_values.end(), 0.0) /
                    static_cast<double>(measurement_values.size());
  auto normalized_settings = dsp::serialize_analysis_settings(analysis.settings);
  if (!normalized_settings) {
    return normalized_settings.error();
  }
  core::ArtifactDescriptor descriptor;
  descriptor.id = artifact_id("RST");
  descriptor.kind = core::ArtifactKind::measurement;
  descriptor.format = core::ArtifactFormat::json;
  descriptor.provenance = {
      workspace_.project_id, analysis.frame.data_source_version_id, std::move(selection_id),
      std::move(channel_id), analysis.inspector.channel_version,    analysis.task_id,
      "signal.dsp.psd",      analysis.settings.algorithm_version,   analysis.settings_hash.stable_text()};
  descriptor.units = {{"peak", "dBFS/Hz"}, {"mean", "dBFS/Hz"}};
  descriptor.metadata = {
      {"backend", analysis.backend_id},
      {"device", analysis.device_id},
      {"parameterSchema", analysis.settings.schema},
      {"parameterSnapshot", normalized_settings.value()},
      {"parameterHash", analysis.settings_hash.stable_text()},
      {"sourceRangeBegin", std::to_string(analysis.source_range.begin())},
      {"sourceRangeEnd", std::to_string(analysis.source_range.end())},
      {"sampleRateHz", std::to_string(current_signal_ ? current_signal_->descriptor.sample_rate_hz : 0.0)},
      {"centerFrequencyHz", std::to_string(current_signal_ && current_signal_->descriptor.center_frequency_hz
                                               ? *current_signal_->descriptor.center_frequency_hz
                                               : 0.0)},
      {"fftSize", std::to_string(analysis.psd.fft_length)},
      {"frameLength", std::to_string(analysis.psd.frame_length)},
      {"window", window_label(analysis.settings.spectrum.window)},
      {"enbwHz", std::to_string(analysis.psd.equivalent_noise_bandwidth_hz)},
      {"rbwHz", std::to_string(analysis.psd.resolution_bandwidth_hz)},
      {"estimator",
       analysis.settings.spectrum.estimator.kind == dsp::PsdEstimatorKind::welch ? "Welch" : "Periodogram"},
      {"accumulation", std::to_string(static_cast<unsigned>(analysis.settings.spectrum.accumulation.mode))},
      {"smoothing", std::to_string(static_cast<unsigned>(analysis.settings.spectrum.smoothing.kind))},
      {"prefilterApplied", analysis.settings.prefilter.enabled ? "true" : "false"},
      {"measurementSource",
       analysis.settings.spectrum.measurement_source == dsp::MeasurementSource::raw ? "raw" : "smoothed"},
      {"computeMilliseconds", std::to_string(analysis.compute_duration.count())},
      {"viewRequest", analysis.view_request.scope + ":" + std::to_string(analysis.view_request.generation)},
      {"cacheKey", analysis.cache_key}};
  std::ostringstream data_json;
  data_json << "{\"peak\":" << std::setprecision(17) << peak << ",\"mean\":" << mean
            << ",\"bins\":" << measurement_values.size() << ",\"measurementSource\":"
            << json_string(analysis.settings.spectrum.measurement_source == dsp::MeasurementSource::raw ? "raw"
                                                                                                        : "smoothed")
            << ",\"parameterHash\":" << json_string(analysis.settings_hash.stable_text())
            << ",\"parameters\":" << json_string(normalized_settings.value()) << '}';
  auto payload =
      core::make_artifact_json("signal.measurement.psd/1.0", descriptor.provenance, descriptor.units, data_json.str());
  if (!payload) {
    return payload.error();
  }
  auto committed = artifact_store_->commit(descriptor, payload.value());
  if (!committed) {
    return committed.error();
  }
  if (const auto status = append_result_to_workspace(committed.value()); !status) {
    return status;
  }
  return committed.value();
}

core::Result<core::ArtifactRecord> ApplicationController::commit_sample_export(const ImportedSignal& imported,
                                                                               data::SourceFormat format,
                                                                               std::uint64_t maximum_samples) {
  std::lock_guard state_lock{analysis_state_mutex_};
  if (!artifact_store_ || maximum_samples == 0U) {
    return failure(core::ErrorReason::unavailable, "当前没有可提交的采样结果");
  }
  const auto slice = bounded_slice(imported, maximum_samples);
  auto range = data::SampleRange::from_count(imported.loaded->range().begin(), slice.size());
  if (!range) {
    return range.error();
  }
  data::SignalBuffer exported_buffer;
  if (slice.kind() == data::SignalKind::complex) {
    const auto values = slice.complex_values();
    exported_buffer = data::SignalBuffer::from_complex({values.begin(), values.end()});
  } else {
    const auto values = slice.real_values();
    exported_buffer = data::SignalBuffer::from_real({values.begin(), values.end()});
  }
  data::RawReadResult browsed{std::move(exported_buffer), range.value(),
                              slice.size() * imported.descriptor.frame_bytes().value()};
  auto selection = data::select_samples(browsed, range.value(), *imported.source);
  if (!selection) {
    return selection.error();
  }
  selection.value().source_format = format;
  const auto temporary =
      state_directory_ / (artifact_id("sample-export") + (format == data::SourceFormat::wav ? ".wav" : ".raw"));
  const auto maximum_bytes = slice.size() * imported.descriptor.frame_bytes().value() + 44U;
  auto exported = data::export_selection(selection.value(), temporary, maximum_bytes);
  if (!exported) {
    return exported.error();
  }
  auto payload = read_file_bounded(temporary, maximum_bytes);
  std::error_code ignored;
  std::filesystem::remove(temporary, ignored);
  if (!payload) {
    return payload.error();
  }
  auto descriptor_json = data::serialize_sidecar(exported.value().exported_descriptor);
  if (!descriptor_json) {
    return descriptor_json.error();
  }
  core::ArtifactDescriptor descriptor;
  descriptor.id = artifact_id(format == data::SourceFormat::wav ? "WAV" : "RAW");
  descriptor.kind = format == data::SourceFormat::wav ? core::ArtifactKind::audio : core::ArtifactKind::sampled_data;
  descriptor.format = format == data::SourceFormat::wav ? core::ArtifactFormat::wav : core::ArtifactFormat::raw;
  descriptor.provenance = {
      workspace_.project_id, imported.fingerprint.version_id, {}, {}, {}, {}, "signal.data.export-selection", "1.0.0",
      "export-default-v1"};
  descriptor.units = {{"sample", imported.descriptor.amplitude_mode}};
  descriptor.metadata = {{"signalDescriptor", descriptor_json.value()},
                         {"sourceRangeBegin", std::to_string(range.value().begin())},
                         {"sourceRangeEnd", std::to_string(range.value().end())}};
  if (format == data::SourceFormat::wav) {
    descriptor.metadata.emplace("bitDepth", std::to_string(imported.descriptor.scalar_bytes().value() * 8U));
    descriptor.metadata.emplace("sampleRateHz", std::to_string(imported.descriptor.sample_rate_hz));
    descriptor.metadata.emplace("normalizationPolicy", "保持确认描述符比例；不自动归一化");
  }
  auto committed = artifact_store_->commit(descriptor, payload.value());
  if (!committed) {
    return committed.error();
  }
  if (const auto status = append_result_to_workspace(committed.value()); !status) {
    return status;
  }
  return committed.value();
}

core::Result<std::vector<core::ArtifactRecord>>
ApplicationController::results(const core::ArtifactFilter& filter) const {
  if (!artifact_store_) {
    return std::vector<core::ArtifactRecord>{};
  }
  return artifact_store_->query(filter);
}

core::Result<std::filesystem::path>
ApplicationController::export_result(const core::ArtifactRecord& result,
                                     const std::filesystem::path& destination) const {
  if (!artifact_store_) {
    return failure(core::ErrorReason::unavailable, "当前没有结果存储");
  }
  return artifact_store_->export_package(result, destination);
}

const core::Workspace& ApplicationController::workspace() const noexcept {
  return workspace_;
}

const std::filesystem::path& ApplicationController::project_path() const noexcept {
  return project_path_;
}

std::optional<ImportedSignal> ApplicationController::current_signal() const {
  std::lock_guard lock{analysis_state_mutex_};
  return current_signal_;
}

std::shared_ptr<const AnalysisBundle> ApplicationController::current_analysis() const {
  std::lock_guard lock{analysis_state_mutex_};
  return current_analysis_;
}

dsp::AnalysisSettingsSnapshot ApplicationController::analysis_settings() const {
  std::lock_guard lock{analysis_state_mutex_};
  return analysis_settings_;
}

AnalysisDisplaySettings ApplicationController::analysis_display_settings() const {
  std::lock_guard lock{analysis_state_mutex_};
  return analysis_display_settings_;
}

core::Status ApplicationController::set_analysis_settings(dsp::AnalysisSettingsSnapshot settings) {
  std::lock_guard lock{analysis_state_mutex_};
  if (current_signal_ && current_signal_->loaded) {
    if (const auto status = dsp::validate_analysis_settings(settings, current_signal_->loaded->samples().size(),
                                                            current_signal_->descriptor);
        !status) {
      return status;
    }
  } else if (const auto serialized = dsp::serialize_analysis_settings(settings); !serialized) {
    return serialized.error();
  }
  analysis_settings_ = std::move(settings);
  persist_analysis_extensions();
  return core::Status::success();
}

core::Status ApplicationController::set_analysis_display_settings(AnalysisDisplaySettings settings) {
  if (settings.schema.rfind("signal.analysis-display/1.", 0U) != 0U) {
    return failure(core::ErrorReason::unavailable, "不支持的分析显示参数主版本", settings.schema);
  }
  if (const auto status = visualization::validate_display_mapping(settings.mapping); !status) {
    return status;
  }
  if (settings.interpolation != "nearest" && settings.interpolation != "linear") {
    return failure(core::ErrorReason::invalid_argument, "STFT 插值只允许 nearest 或 linear");
  }
  {
    std::lock_guard lock{analysis_state_mutex_};
    analysis_display_settings_ = std::move(settings);
    persist_analysis_extensions();
  }
  return core::Status::success();
}

core::Status ApplicationController::save_user_analysis_preset(std::string name,
                                                              const dsp::AnalysisSettingsSnapshot& settings) {
  if (name.empty() || name.size() > 128U) {
    return failure(core::ErrorReason::invalid_argument, "用户预设名称长度必须位于 1 到 128 字节");
  }
  if (const auto serialized = dsp::serialize_analysis_settings(settings); !serialized) {
    return serialized.error();
  }
  std::lock_guard lock{analysis_state_mutex_};
  user_analysis_presets_.insert_or_assign(std::move(name), settings);
  persist_analysis_extensions();
  return core::Status::success();
}

core::Status ApplicationController::delete_user_analysis_preset(std::string_view name) {
  std::lock_guard lock{analysis_state_mutex_};
  if (user_analysis_presets_.erase(std::string{name}) == 0U) {
    return failure(core::ErrorReason::invalid_argument, "待删除的用户预设不存在", std::string{name});
  }
  persist_analysis_extensions();
  return core::Status::success();
}

core::Status ApplicationController::set_active_analysis_preset(std::string preset_id,
                                                               const dsp::AnalysisSettingsSnapshot& settings,
                                                               std::string scope) {
  if (scope != "project-view" && scope != "project-channel") {
    return failure(core::ErrorReason::invalid_argument, "分析预设作用域必须是 project-view 或 project-channel");
  }
  auto hash = dsp::hash_analysis_settings(settings);
  if (!hash) {
    return hash.error();
  }
  std::lock_guard lock{analysis_state_mutex_};
  analysis_scope_ = std::move(scope);
  active_analysis_preset_ = std::move(preset_id);
  active_analysis_preset_hash_ = hash.value().stable_text();
  persist_analysis_extensions();
  return core::Status::success();
}

std::map<std::string, dsp::AnalysisSettingsSnapshot, std::less<>> ApplicationController::user_analysis_presets() const {
  std::lock_guard lock{analysis_state_mutex_};
  return user_analysis_presets_;
}

std::vector<AnalysisPreset> ApplicationController::built_in_analysis_presets(const ImportedSignal& imported,
                                                                             bool prefer_cuda,
                                                                             std::uint64_t viewport_samples) const {
  const auto make = [&](std::string id, std::string name, std::string scenario) {
    auto settings = adaptive_settings(imported, id, prefer_cuda, viewport_samples);
    auto cost = dsp::estimate_analysis_cost(settings, imported.loaded ? imported.loaded->samples().size() : 0U,
                                            imported.descriptor.sample_rate_hz, 768ULL * 1024ULL * 1024ULL,
                                            384ULL * 1024ULL * 1024ULL);
    const auto bin_spacing = imported.descriptor.sample_rate_hz /
                             static_cast<double>(std::max<std::uint64_t>(2U, settings.spectrum.fft_length));
    const auto hop_seconds = static_cast<double>(settings.spectrogram.hop_length) / imported.descriptor.sample_rate_hz;
    const auto variance = settings.spectrum.estimator.kind == dsp::PsdEstimatorKind::periodogram ? "高"
                          : settings.spectrum.estimator.welch_segment_count >= 16U               ? "低"
                                                                                                 : "中";
    const auto smoothed =
        settings.spectrum.smoothing.kind != dsp::SpectrumSmoothingKind::none ||
        settings.spectrogram.smoothing.frequency_mode != dsp::SpectrogramFrequencySmoothingKind::none ||
        settings.spectrogram.smoothing.time_mode != dsp::SpectrogramTimeSmoothingKind::none;
    const auto normalized_bin_spacing = bin_spacing / imported.descriptor.sample_rate_hz;
    const auto narrow_peak_risk = normalized_bin_spacing <= 1.0 / 16'384.0  ? "低"
                                  : normalized_bin_spacing <= 1.0 / 4'096.0 ? "中"
                                                                            : "高";
    const auto frame_seconds =
        static_cast<double>(settings.spectrogram.frame_length) / imported.descriptor.sample_rate_hz;
    const auto short_burst_risk = frame_seconds <= 0.002 ? "低" : frame_seconds <= 0.02 ? "中" : "高";
    auto description = "场景：" + scenario + "；频率分辨率 Δf=" + std::to_string(bin_spacing) +
                       " Hz；时间步进 Δt=" + std::to_string(hop_seconds) + " s；噪声方差=" + variance +
                       "；平滑=" + (smoothed ? "开启" : "关闭") + "；窄峰风险=" + narrow_peak_risk +
                       "；短突发风险=" + short_burst_risk;
    if (cost) {
      const auto compute_cost = cost.value().estimated_operations < 5.0e7   ? "低"
                                : cost.value().estimated_operations < 5.0e8 ? "中"
                                                                            : "高";
      description += "；计算代价=" + std::string{compute_cost} + "（主机内存 " +
                     std::to_string(cost.value().host_memory_bytes / (1024U * 1024U)) + " MiB）";
    }
    description += prefer_cuda ? "；CUDA 优先。" : "；CPU 路径。";
    return AnalysisPreset{std::move(id), std::move(name), std::move(description), std::move(settings)};
  };
  return {
      make("quick-preview", "快速预览", "快速浏览与交互调参"),
      make("balanced-analysis", "平衡分析", "通用频谱、PSD 与 STFT 联合分析"),
      make("high-resolution", "高分辨率", "需要更细频率栅格的稳态信号"),
      make("low-noise-psd", "低噪声 PSD", "降低随机噪声方差的功率谱估计"),
      make("burst-signal", "突发信号", "短时突发和快速时变信号"),
      make("narrowband-fine", "窄带精细分析", "稳态窄带与邻近谱线分离"),
  };
}

std::vector<task::TaskStatus> ApplicationController::task_history() const {
  return task_runtime_.history();
}

workbench::WorkbenchContent ApplicationController::workbench_content() const {
  workbench::WorkbenchContent content;
  content.project_name = workspace_.project_id;
  content.status_text = workspace_.project_id.empty() ? "● 就绪 · 未打开项目" : "● 就绪 · 项目已打开";
  content.resource_text = "离线 · CPU/CUDA 自动降级";
  if (current_signal_) {
    const auto& imported = *current_signal_;
    content.source_summary = imported.source_path.filename().string() + " · " +
                             scalar_type_label(imported.descriptor.scalar_type) + " · " +
                             std::to_string(imported.descriptor.sample_rate_hz / 1.0e6) + " MS/s";
    content.navigation = {{imported.source_path.filename().string(),
                           std::to_string(imported.loaded->samples().size()) + " 样本", 0U, true},
                          {"概览与导航", {}, 1U, false},
                          {"选区与标记", "0", 1U, false},
                          {"CH-01 基础分析", {}, 1U, false},
                          {"结果", std::to_string(workspace_.results.size()), 1U, false}};
    content.inspector = {
        {"数据源版本", imported.fingerprint.version_id.substr(0U, 16U)},
        {"实际文件字节", std::to_string(imported.fingerprint.size_bytes)},
        {"已读样本范围", "[" + std::to_string(imported.loaded->range().begin()) + ", " +
                             std::to_string(imported.loaded->range().end()) + ")"},
        {"读取状态", imported.partial_read ? "部分读取可用" : "初始范围完整"},
        {"采样率", std::to_string(imported.descriptor.sample_rate_hz) + " Hz"},
    };
  } else {
    content.source_summary = "未打开数据源";
    content.navigation = {{"未打开项目或数据源", {}, 0U, true}};
  }
  for (const auto& task_status : task_runtime_.history()) {
    content.tasks.push_back({task_status.task_type, std::string{task::to_string(task_status.state)},
                             std::to_string(static_cast<unsigned>(task_status.progress * 100.0)) + "%",
                             task_status.resources.gpu_units > 0U ? "GPU/CPU" : "CPU",
                             task_status.provenance.source_object.id});
  }
  const auto current_parameter_hash = dsp::hash_analysis_settings(analysis_settings_);
  for (const auto& result : workspace_.results) {
    const auto parameter = result.attributes.find("parameterVersion");
    const auto current = current_signal_ && current_parameter_hash &&
                         result.data_source_version_id == current_signal_->fingerprint.version_id &&
                         parameter != result.attributes.end() &&
                         parameter->second == current_parameter_hash.value().stable_text();
    content.results.push_back(
        {result.id, current ? "● 当前" : "! 已过期", result.data_source_version_id, current ? "可定位" : "查看来源"});
  }
  return content;
}

core::Result<std::vector<std::filesystem::path>> ApplicationController::recent_projects() const {
  return recent_projects_.load();
}

task::TaskRuntime& ApplicationController::task_runtime() noexcept {
  return task_runtime_;
}

void ApplicationController::persist_analysis_extensions() {
  if (auto serialized = dsp::serialize_analysis_settings(analysis_settings_); serialized) {
    workspace_.extensions["signal.analysis-settings"] = json_string(serialized.value());
  }
  workspace_.extensions["signal.analysis-display"] =
      json_string(serialize_display_settings(analysis_display_settings_));
  constexpr std::string_view preset_prefix{"signal.analysis-user-preset."};
  for (auto iterator = workspace_.extensions.begin(); iterator != workspace_.extensions.end();) {
    if (iterator->first.rfind(preset_prefix, 0U) == 0U) {
      iterator = workspace_.extensions.erase(iterator);
    } else {
      ++iterator;
    }
  }
  for (const auto& [name, settings] : user_analysis_presets_) {
    if (auto serialized = dsp::serialize_analysis_settings(settings); serialized) {
      workspace_.extensions[std::string{preset_prefix} + name] = json_string(serialized.value());
    }
  }
  workspace_.extensions["signal.analysis-scope"] = json_string(analysis_scope_);
  workspace_.extensions["signal.analysis-active-preset"] = json_string(active_analysis_preset_);
  workspace_.extensions["signal.analysis-active-preset-hash"] = json_string(active_analysis_preset_hash_);
}

core::Result<ApplicationController::RestoredAnalysisExtensions>
ApplicationController::restore_analysis_extensions(const core::Workspace& workspace) const {
  RestoredAnalysisExtensions restored;
  constexpr std::string_view preset_prefix{"signal.analysis-user-preset."};
  if (const auto found = workspace.extensions.find("signal.analysis-settings"); found != workspace.extensions.end()) {
    auto decoded = decode_extension_string(found->second);
    if (!decoded) {
      return decoded.error().with_context("读取工程分析参数");
    }
    auto parsed = dsp::parse_analysis_settings(decoded.value());
    if (!parsed) {
      return parsed.error().with_context("读取工程分析参数");
    }
    restored.settings = std::move(parsed.value());
  }
  if (const auto found = workspace.extensions.find("signal.analysis-display"); found != workspace.extensions.end()) {
    auto decoded = decode_extension_string(found->second);
    if (decoded) {
      auto parsed = parse_display_settings(decoded.value());
      if (parsed) {
        restored.display = std::move(parsed.value());
      }
    }
  }
  for (const auto& [key, value] : workspace.extensions) {
    if (key.rfind(preset_prefix, 0U) != 0U) {
      continue;
    }
    auto decoded = decode_extension_string(value);
    if (!decoded) {
      continue;
    }
    auto parsed = dsp::parse_analysis_settings(decoded.value());
    if (!parsed) {
      continue;
    }
    restored.user_presets.emplace(key.substr(preset_prefix.size()), std::move(parsed.value()));
  }
  const auto decode_optional = [&](std::string_view key, std::string& destination) {
    if (const auto found = workspace.extensions.find(std::string{key}); found != workspace.extensions.end()) {
      if (auto decoded = decode_extension_string(found->second); decoded) {
        destination = std::move(decoded.value());
      }
    }
  };
  decode_optional("signal.analysis-scope", restored.scope);
  decode_optional("signal.analysis-active-preset", restored.active_preset);
  decode_optional("signal.analysis-active-preset-hash", restored.active_preset_hash);
  if (restored.scope != "project-view" && restored.scope != "project-channel") {
    restored.scope = "project-view";
  }
  return restored;
}

core::Status ApplicationController::persist_source_link(const ImportedSignal& imported, std::string_view source_id) {
  if (project_path_.empty() || source_id.empty()) {
    return failure(core::ErrorReason::invalid_argument, "项目或数据源 ID 无效");
  }
  const auto resources = project_path_.parent_path() / (project_path_.stem().string() + ".assets") / "resources";
  return core::AtomicFileStore{}.write(resources / (std::string{source_id} + ".source-link"),
                                       as_bytes(source_link_text(imported)));
}

core::Result<std::filesystem::path>
ApplicationController::resolve_source_link(const core::WorkspaceDataSource& source) const {
  auto relative = core::resolve_relative_resource(project_path_, utf8_path(source.relative_uri));
  if (!relative) {
    return relative.error();
  }
  return parse_source_link(relative.value(), source.version_id);
}

core::Status ApplicationController::append_result_to_workspace(const core::ArtifactRecord& record) {
  workspace_.results.push_back(
      {record.descriptor.id,
       std::string{core::artifact_kind_name(record.descriptor.kind)},
       record.descriptor.provenance.data_source_version_id,
       {{"artifactPackage", path_utf8(record.package_path.lexically_relative(project_path_.parent_path()))},
        {"sha256", record.payload_digest.hex()},
        {"parameterVersion", record.descriptor.provenance.parameter_version},
        {"channelVersion", record.descriptor.provenance.channel_version}},
       {}});
  persist_analysis_extensions();
  const auto status = workspace_store_.save(project_path_, workspace_);
  if (!status) {
    workspace_.results.pop_back();
  }
  return status;
}

core::Status run_headless_self_test(const std::filesystem::path& scratch_directory) {
  std::error_code error;
  std::filesystem::create_directories(scratch_directory, error);
  if (error) {
    return failure(core::ErrorReason::unavailable, "自检目录不可创建", error.message());
  }
  const auto session = scratch_directory / ("run-" + task::TaskId::generate().value);
  std::filesystem::create_directories(session, error);
  if (error) {
    return failure(core::ErrorReason::unavailable, "自检会话目录不可创建", error.message());
  }
  const auto source = session / "selftest_cf1MHz_sr1MSps.sc16";
  std::vector<std::byte> samples(4096U * 4U);
  for (std::size_t index = 0; index < 4096U; ++index) {
    const auto i = static_cast<std::int16_t>(std::llround(std::sin(static_cast<double>(index) * 0.05) * 20'000.0));
    const auto q = static_cast<std::int16_t>(std::llround(std::cos(static_cast<double>(index) * 0.05) * 20'000.0));
    std::memcpy(samples.data() + index * 4U, &i, sizeof(i));
    std::memcpy(samples.data() + index * 4U + 2U, &q, sizeof(q));
  }
  if (const auto status = core::AtomicFileStore{}.write(source, samples); !status) {
    return status;
  }
  ApplicationController controller{session / "state"};
  const auto project = session / "selftest.signal-workspace";
  if (const auto status = controller.create_project(project, "selftest-project"); !status) {
    return status;
  }
  auto descriptor = make_confirmed_descriptor(source, parse_filename_hints(source), true);
  if (!descriptor) {
    return descriptor.error();
  }
  auto import = controller.start_import({source, descriptor.value(), data::SourceFormat::raw, samples.size(), 4096U});
  if (!import) {
    return import.error();
  }
  auto imported = controller.finalize_import(import.value());
  if (!imported) {
    return imported.error();
  }
  auto analysis = controller.analyze(imported.value());
  if (!analysis) {
    return analysis.error();
  }
  auto result = controller.commit_measurement(analysis.value(), "selftest-selection", "CH-01");
  if (!result) {
    return result.error();
  }
  if (auto records = controller.results(); !records || records.value().size() != 1U) {
    return failure(core::ErrorReason::internal_failure, "自检结果闭环失败");
  }
  return core::Status::success();
}

} // namespace signal::studio
