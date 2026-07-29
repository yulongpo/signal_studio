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

} // namespace

struct ImportTask::SharedState final {
  mutable std::mutex mutex;
  std::optional<ImportedSignal> imported;
  std::optional<core::Status> error;
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
  workspace_ = std::move(created).value();
  project_path_ = project_path;
  artifact_store_ = std::make_unique<core::ArtifactStore>(project_path_.parent_path() /
                                                          (project_path_.stem().string() + ".assets") / "artifacts");
  if (const auto status = artifact_store_->recover(); !status) {
    return status;
  }
  if (const auto status = save_project(); !status) {
    return status;
  }
  return recent_projects_.record(project_path_);
}

core::Status ApplicationController::open_project(const std::filesystem::path& project_path, bool read_only) {
  auto loaded = workspace_store_.load(project_path, read_only);
  if (!loaded) {
    return loaded.error();
  }
  workspace_ = std::move(loaded).value();
  project_path_ = project_path;
  artifact_store_ = std::make_unique<core::ArtifactStore>(project_path_.parent_path() /
                                                          (project_path_.stem().string() + ".assets") / "artifacts");
  if (const auto status = artifact_store_->recover(); !status) {
    return status;
  }
  current_signal_.reset();
  current_analysis_.reset();
  if (!workspace_.data_sources.empty()) {
    const auto& source = workspace_.data_sources.front();
    auto resolved = resolve_source_link(source);
    if (!resolved) {
      return resolved.error();
    }
    const auto context_status = current_context_.switch_to({workspace_.project_id, source.id, source.version_id, 0U});
    if (!context_status) {
      return context_status;
    }
  }
  return read_only ? core::Status::success() : recent_projects_.record(project_path_);
}

core::Status ApplicationController::save_project() {
  if (project_path_.empty()) {
    return failure(core::ErrorReason::unavailable, "当前没有可保存项目");
  }
  return workspace_store_.save(project_path_, workspace_);
}

core::Status ApplicationController::close_project() {
  current_signal_.reset();
  current_analysis_.reset();
  artifact_store_.reset();
  project_path_.clear();
  return workspace_store_.close(workspace_);
}

core::Result<ImportTask> ApplicationController::start_import(ImportRequest request) {
  if (workspace_.project_id.empty()) {
    return failure(core::ErrorReason::unavailable, "导入前必须先新建或打开项目");
  }
  if (request.path.empty() || request.initial_bytes == 0U || request.chunk_bytes == 0U) {
    return failure(core::ErrorReason::invalid_argument, "导入路径或有界读取参数无效");
  }
  if (const auto status = request.descriptor.validate(); !status) {
    return status;
  }
  auto state = std::make_shared<ImportTask::SharedState>();
  task::TaskSpec spec;
  spec.task_id = task::TaskId::generate();
  spec.task_type = "signal-studio.import";
  spec.priority = task::TaskPriority::foreground;
  spec.resources = {.cpu_units = 1U, .io_units = 1U, .runtime_threads = 1U};
  spec.idempotency_key = path_utf8(request.path) + ":" +
                         std::to_string(request.descriptor.requested_sample_range.begin()) + ":" +
                         std::to_string(request.initial_bytes) + ":attempt:" + spec.task_id.value;
  spec.provenance = {{workspace_.project_id}, {"pending-import"}, {"data-source", path_utf8(request.path)}};
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
  if (const auto status = save_project(); !status) {
    return status;
  }
  return imported.value();
}

core::Result<AnalysisBundle> ApplicationController::analyze(const ImportedSignal& imported, bool prefer_cuda) {
  if (!imported.loaded || imported.loaded->samples().size() < 1024U) {
    return failure(core::ErrorReason::invalid_argument, "基础 PSD/STFT 分析至少需要 1024 个已校验样本");
  }
  const auto samples = bounded_slice(imported, 16'384U);
  std::string backend_reason;
  auto backend = dsp::make_auto_fft_backend(prefer_cuda, &backend_reason);
  if (!backend) {
    return backend.error();
  }
  const auto center_frequency = imported.descriptor.center_frequency_hz.value_or(0.0);
  const auto sidedness = imported.descriptor.signal_kind == data::SignalKind::complex
                             ? dsp::SpectrumSidedness::two_sided_shifted
                             : dsp::SpectrumSidedness::one_sided;
  const dsp::SpectrumRequest spectrum_request{imported.descriptor.sample_rate_hz, center_frequency,
                                              dsp::WindowKind::hann, sidedness};
  auto psd = dsp::calculate_psd(*backend.value(), samples, spectrum_request);
  if (!psd) {
    return psd.error();
  }
  const std::uint64_t fft_length = std::min<std::uint64_t>(1024U, samples.size());
  auto stft = dsp::calculate_stft(*backend.value(), samples,
                                  {imported.descriptor.sample_rate_hz, center_frequency, fft_length,
                                   std::max<std::uint64_t>(1U, fft_length / 4U), dsp::WindowKind::hann, sidedness});
  if (!stft) {
    return stft.error();
  }
  if (psd.value().frequency_hz.size() < 2U) {
    return failure(core::ErrorReason::internal_failure, "PSD 未产生有效频率轴");
  }
  const auto frequency_begin = static_cast<std::int64_t>(std::llround(psd.value().frequency_hz.front()));
  const auto bin_width = psd.value().frequency_hz[1] - psd.value().frequency_hz[0];
  const auto frequency_end =
      static_cast<std::int64_t>(std::llround(psd.value().frequency_hz.back() + std::max(1.0, bin_width)));
  auto frequency_range = visualization::make_frequency_range(frequency_begin, frequency_end);
  if (!frequency_range) {
    return frequency_range.error();
  }
  visualization::ViewportController controller{"signal-studio-p02"};
  const auto loaded_range = imported.loaded->range();
  if (!controller.bind_source(imported.fingerprint.version_id, loaded_range, frequency_range.value(),
                              imported.partial_read,
                              imported.loaded->samples().size() * imported.descriptor.frame_bytes().value(),
                              imported.fingerprint.size_bytes)) {
    return failure(core::ErrorReason::internal_failure, "无法绑定真实导入视口");
  }
  auto viewport_range =
      data::SampleRange::from_count(loaded_range.begin(), std::min<std::uint64_t>(loaded_range.size(), samples.size()));
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
  frame.psd_db_hz = psd.value().db_per_hz;
  frame.stft_db = stft.value().db_per_hz;
  frame.stft_rows = static_cast<std::uint32_t>(stft.value().rows);
  frame.stft_columns = static_cast<std::uint32_t>(stft.value().columns);
  frame.psd_metadata = {samples.size(), 1U,     static_cast<std::uint32_t>(samples.size()),
                        "Hann",         "单窗", psd.value().equivalent_noise_bandwidth_hz,
                        "dB/Hz"};
  frame.stft_metadata = {static_cast<std::uint32_t>(fft_length),
                         static_cast<std::uint32_t>(std::max<std::uint64_t>(1U, fft_length / 4U)),
                         static_cast<std::uint32_t>(fft_length),
                         0.75,
                         "Industrial",
                         "nearest"};
  frame.absolute_frequency = imported.descriptor.center_frequency_hz.has_value();
  frame.center_frequency_hz =
      static_cast<std::int64_t>(std::llround(imported.descriptor.center_frequency_hz.value_or(0.0)));
  frame.data_source_version_id = imported.fingerprint.version_id;
  auto inspector = workbench::make_inspector_channel_state("CH-01", "channel-v1", imported.fingerprint.version_id,
                                                           imported.descriptor);
  if (!inspector) {
    return inspector.error();
  }
  AnalysisBundle bundle{controller.snapshot(), std::move(frame), inspector.value(), psd.value().provenance.backend_id};
  current_analysis_ = bundle;
  return bundle;
}

core::Result<core::ArtifactRecord> ApplicationController::commit_measurement(const AnalysisBundle& analysis,
                                                                             std::string selection_id,
                                                                             std::string channel_id) {
  if (!artifact_store_ || workspace_.project_id.empty() || analysis.frame.psd_db_hz.empty()) {
    return failure(core::ErrorReason::unavailable, "当前没有可提交的分析结果");
  }
  const auto peak = *std::ranges::max_element(analysis.frame.psd_db_hz);
  const auto mean = std::accumulate(analysis.frame.psd_db_hz.begin(), analysis.frame.psd_db_hz.end(), 0.0) /
                    static_cast<double>(analysis.frame.psd_db_hz.size());
  core::ArtifactDescriptor descriptor;
  descriptor.id = artifact_id("RST");
  descriptor.kind = core::ArtifactKind::measurement;
  descriptor.format = core::ArtifactFormat::json;
  descriptor.provenance = {workspace_.project_id,
                           analysis.frame.data_source_version_id,
                           std::move(selection_id),
                           std::move(channel_id),
                           analysis.inspector.channel_version,
                           {},
                           "signal.dsp.psd",
                           "1.0.0",
                           "psd-default-v1"};
  descriptor.units = {{"peak", "dB/Hz"}, {"mean", "dB/Hz"}};
  descriptor.metadata = {{"backend", analysis.backend_id},
                         {"fftSize", std::to_string(analysis.frame.psd_metadata.fft_size)},
                         {"window", analysis.frame.psd_metadata.window}};
  std::ostringstream data_json;
  data_json << "{\"peak\":" << std::setprecision(17) << peak << ",\"mean\":" << mean
            << ",\"bins\":" << analysis.frame.psd_db_hz.size() << '}';
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

const std::optional<ImportedSignal>& ApplicationController::current_signal() const noexcept {
  return current_signal_;
}

const std::optional<AnalysisBundle>& ApplicationController::current_analysis() const noexcept {
  return current_analysis_;
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
  for (const auto& result : workspace_.results) {
    const auto current = current_signal_ && result.data_source_version_id == current_signal_->fingerprint.version_id;
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
  const auto status = save_project();
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
