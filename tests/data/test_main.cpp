#include "signal_studio/data/index.hpp"
#include "signal_studio/data/io.hpp"
#include "signal_studio/data/loading.hpp"
#include "signal_studio/data/preview.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/task_runtime/task_runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace signal::data;

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string{message});
}

void require(const signal::core::Status& condition, std::string_view message) {
  require(static_cast<bool>(condition), message);
}

template <typename T> void require(const signal::core::Result<T>& condition, std::string_view message) {
  require(static_cast<bool>(condition), message);
}

std::filesystem::path fixture_root() {
  const auto root = std::filesystem::temp_directory_path() / "signal-studio-ms01-data-tests";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  return root;
}

void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(output), "无法写入测试夹具");
}

template <typename T> void append_little(std::vector<std::byte>& output, T value) {
  std::uint64_t bits{};
  if constexpr (std::is_floating_point_v<T> && sizeof(T) == 4U) {
    bits = std::bit_cast<std::uint32_t>(value);
  } else if constexpr (std::is_floating_point_v<T> && sizeof(T) == 8U) {
    bits = std::bit_cast<std::uint64_t>(value);
  } else {
    bits = static_cast<std::uint64_t>(value);
  }
  for (std::size_t index = 0; index < sizeof(T); ++index)
    output.push_back(static_cast<std::byte>((bits >> (index * 8U)) & 0xffU));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
  append_little(output, value);
}
void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
  append_little(output, value);
}
void append_text(std::vector<std::byte>& output, std::string_view text) {
  for (const char value : text)
    output.push_back(static_cast<std::byte>(value));
}

SampleRange range(std::uint64_t begin, std::uint64_t end) {
  auto result = SampleRange::make(begin, end);
  require(static_cast<bool>(result), "测试范围无效");
  return result.value();
}

SignalDescriptor real_descriptor(ScalarType scalar, Endianness endian, std::uint64_t count) {
  SignalDescriptor descriptor;
  descriptor.signal_kind = SignalKind::real;
  descriptor.scalar_type = scalar;
  descriptor.component_layout = ComponentLayout::real;
  descriptor.component_order = ComponentOrder::not_applicable;
  descriptor.endianness = endian;
  descriptor.sample_rate_hz = 1000.0;
  descriptor.requested_sample_range = range(0U, count);
  return descriptor;
}

SignalDescriptor sc16_descriptor(std::uint64_t count, ComponentOrder order = ComponentOrder::iq,
                                 ComponentLayout layout = ComponentLayout::interleaved) {
  SignalDescriptor descriptor;
  descriptor.signal_kind = SignalKind::complex;
  descriptor.scalar_type = ScalarType::int16;
  descriptor.component_layout = layout;
  descriptor.component_order = order;
  descriptor.endianness = Endianness::little;
  descriptor.sample_rate_hz = 50'000'000.0;
  descriptor.center_frequency_hz = 1'245'000'000.0;
  descriptor.requested_sample_range = range(0U, count);
  return descriptor;
}

std::filesystem::path make_sc16(const std::filesystem::path& root, std::string_view name,
                                std::span<const ComplexSample> values, ComponentOrder order = ComponentOrder::iq,
                                ComponentLayout layout = ComponentLayout::interleaved) {
  std::vector<std::byte> bytes;
  const auto encode = [&](double value) { append_little(bytes, static_cast<std::int16_t>(value)); };
  if (layout == ComponentLayout::interleaved) {
    for (const auto value : values) {
      if (order == ComponentOrder::iq) {
        encode(value.real);
        encode(value.imag);
      } else {
        encode(value.imag);
        encode(value.real);
      }
    }
  } else {
    for (const auto value : values)
      encode(order == ComponentOrder::iq ? value.real : value.imag);
    for (const auto value : values)
      encode(order == ComponentOrder::iq ? value.imag : value.real);
  }
  const auto path = root / name;
  write_bytes(path, bytes);
  return path;
}

std::filesystem::path make_wav(const std::filesystem::path& root) {
  std::vector<std::byte> payload;
  for (const auto value : std::array<ComplexSample, 2>{{{1.0, -2.0}, {3.0, -4.0}}}) {
    append_little(payload, static_cast<std::int16_t>(value.real));
    append_little(payload, static_cast<std::int16_t>(value.imag));
  }
  std::vector<std::byte> wav;
  append_text(wav, "RIFF");
  append_u32(wav, static_cast<std::uint32_t>(36U + payload.size()));
  append_text(wav, "WAVEfmt ");
  append_u32(wav, 16U);
  append_u16(wav, 1U);
  append_u16(wav, 2U);
  append_u32(wav, 48'000U);
  append_u32(wav, 48'000U * 4U);
  append_u16(wav, 4U);
  append_u16(wav, 16U);
  append_text(wav, "data");
  append_u32(wav, static_cast<std::uint32_t>(payload.size()));
  wav.insert(wav.end(), payload.begin(), payload.end());
  const auto path = root / "stereo.wav";
  write_bytes(path, wav);
  return path;
}

CacheKey cache_key(std::uint64_t time_begin = 0U, std::uint64_t time_end = 8U,
                   TileKind kind = TileKind::spectrum_summary) {
  CacheKey key;
  key.tile_kind = kind;
  key.source_fingerprint = "path-size-time-sampled-hash";
  key.data_source_version_id = "source-v1";
  key.loaded_range = range(0U, std::max<std::uint64_t>(16U, time_end));
  key.descriptor_digest = "descriptor-sha256";
  key.algorithm_version = "summary-1";
  key.dependency_version = "cpu-reference-1";
  key.parameter_digest = "parameters-sha256";
  key.time_viewport = range(time_begin, time_end);
  key.frequency_begin_hz = -1000;
  key.frequency_end_hz = 1000;
  key.pixel_width = 4U;
  key.pixel_height = 2U;
  key.quality = "exact";
  return key;
}

class CountingProducer final : public ITileProducer {
public:
  signal::core::Result<Tile> produce(const TileRequest& request, const CancellationToken& cancellation) override {
    if (cancellation.cancelled()) {
      return signal::core::Status::failure({signal::core::ErrorDomain::data, signal::core::ErrorReason::cancelled},
                                           "生产已取消");
    }
    ++calls;
    Tile tile;
    tile.kind = request.kind;
    tile.time_range = request.key.time_viewport;
    tile.frequency_begin_hz = request.key.frequency_begin_hz;
    tile.frequency_end_hz = request.key.frequency_end_hz;
    tile.width = request.key.pixel_width;
    tile.height = request.key.pixel_height;
    tile.values.assign(static_cast<std::size_t>(tile.width) * tile.height, static_cast<float>(calls));
    return tile;
  }
  int calls{};
};

void test_ranges_buffers() {
  require(!SampleRange::make(9U, 8U), "反向范围必须失败");
  require(!SampleRange::from_count(std::numeric_limits<std::uint64_t>::max(), 1U), "范围溢出必须失败");
  const auto real = SignalBuffer::from_real({1.0, 2.0, 3.0});
  auto slice = real.view().slice(1U, 2U);
  require(slice && slice.value().real_values()[0] == 2.0, "实数零拷贝切片错误");
  const auto complex = SignalBuffer::from_complex({{1.0, -1.0}});
  require(complex.view().complex_values()[0].imag == -1.0, "复数容器错误");
}

void test_data_closed_loop(const std::filesystem::path& root) {
  const auto real_path = root / "closed-loop-real.raw";
  std::vector<std::byte> real_bytes;
  for (const auto value : std::array<std::int16_t, 4>{1, -2, 3, -4})
    append_little(real_bytes, value);
  write_bytes(real_path, real_bytes);
  auto real_source =
      FileDataSource::open_raw(real_path, real_descriptor(ScalarType::int16, Endianness::little, 4U), "real-source-v1");
  require(real_source, "实信号 IDataSource 导入失败");
  IDataSource& real_contract = *real_source.value();
  auto browsed_real = real_contract.read({range(0U, 4U), 8U, {}});
  auto selected_real = browsed_real ? select_samples(browsed_real.value(), range(1U, 3U), real_contract)
                                    : signal::core::Result<SignalSelection>{browsed_real.error()};
  require(selected_real && selected_real.value().samples.real_values()[0] == -2.0, "实信号浏览或选区失败");
  const auto real_export_path = root / "closed-loop-real-export.raw";
  auto real_export = export_selection(selected_real.value(), real_export_path, 4U);
  require(real_export && real_export.value().bytes_written == 4U &&
              real_export.value().exported_descriptor.scalar_type == ScalarType::int16 &&
              real_export.value().exported_descriptor.signal_kind == SignalKind::real &&
              real_export.value().exported_descriptor.endianness == Endianness::little,
          "实信号有界导出格式或描述符不一致");
  auto exported_real_source =
      FileDataSource::open_raw(real_export_path, real_export.value().exported_descriptor, "real-export-v1");
  auto exported_real = exported_real_source.value()->read({range(0U, 2U), 4U, {}});
  require(exported_real && exported_real.value().samples.view().real_values()[1] == 3.0,
          "实信号导出后无法按同一描述符重新导入");

  const std::array complex_values{ComplexSample{1.0, -2.0}, ComplexSample{3.0, -4.0}, ComplexSample{5.0, -6.0}};
  const auto complex_path =
      make_sc16(root, "closed-loop-complex.raw", complex_values, ComponentOrder::qi, ComponentLayout::planar);
  auto complex_source = FileDataSource::open_raw(
      complex_path, sc16_descriptor(3U, ComponentOrder::qi, ComponentLayout::planar), "complex-source-v1");
  require(complex_source, "复信号 IDataSource 导入失败");
  auto browsed_complex = complex_source.value()->read({range(0U, 3U), 12U, {}});
  auto selected_complex = browsed_complex
                              ? select_samples(browsed_complex.value(), range(1U, 3U), *complex_source.value())
                              : signal::core::Result<SignalSelection>{browsed_complex.error()};
  require(selected_complex && selected_complex.value().samples.complex_values()[1] == complex_values[2],
          "复信号浏览或选区失败");
  const auto complex_export_path = root / "closed-loop-complex-export.raw";
  auto complex_export = export_selection(selected_complex.value(), complex_export_path, 8U);
  require(complex_export && complex_export.value().bytes_written == 8U &&
              complex_export.value().exported_descriptor.component_layout == ComponentLayout::planar &&
              complex_export.value().exported_descriptor.component_order == ComponentOrder::qi,
          "复信号有界导出格式或描述符不一致");
  auto exported_complex_source =
      FileDataSource::open_raw(complex_export_path, complex_export.value().exported_descriptor, "complex-export-v1");
  auto exported_complex = exported_complex_source.value()->read({range(0U, 2U), 8U, {}});
  require(exported_complex && exported_complex.value().samples.view().complex_values()[0] == complex_values[1],
          "复信号导出后无法按同一描述符重新导入");

  std::atomic_bool cancelled{true};
  require(!complex_source.value()->read({range(0U, 1U), 4U, [&] { return cancelled.load(); }}),
          "IDataSource::read 必须遵守取消请求");
}

void test_all_scalar_types(const std::filesystem::path& root) {
  struct ScalarCase final {
    ScalarType type;
    Endianness endian;
    std::vector<std::byte> bytes;
    double expected;
  };
  std::vector<ScalarCase> cases;
  cases.push_back({ScalarType::int8, Endianness::not_applicable, {std::byte{0xfe}}, -2.0});
  cases.push_back({ScalarType::uint8, Endianness::not_applicable, {std::byte{0xfe}}, 254.0});
  cases.push_back({ScalarType::int16, Endianness::little, {std::byte{0xfe}, std::byte{0xff}}, -2.0});
  cases.push_back({ScalarType::uint16, Endianness::little, {std::byte{0x34}, std::byte{0x12}}, 4660.0});
  cases.push_back(
      {ScalarType::int24_packed, Endianness::little, {std::byte{0xfe}, std::byte{0xff}, std::byte{0xff}}, -2.0});
  cases.push_back({ScalarType::int32,
                   Endianness::little,
                   {std::byte{0xfe}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}},
                   -2.0});
  std::vector<std::byte> f32;
  append_little(f32, 1.25F);
  cases.push_back({ScalarType::float32, Endianness::little, f32, 1.25});
  std::vector<std::byte> f64;
  append_little(f64, -2.5);
  cases.push_back({ScalarType::float64, Endianness::little, f64, -2.5});
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const auto path = root / ("scalar-" + std::to_string(index) + ".raw");
    write_bytes(path, cases[index].bytes);
    const auto descriptor = real_descriptor(cases[index].type, cases[index].endian, 1U);
    auto result = read_raw_samples(path, descriptor, range(0U, 1U), 8U);
    require(result && std::abs(result.value().samples.view().real_values()[0] - cases[index].expected) < 1e-6,
            "标量类型解码错误");
  }
  const auto big_path = root / "big-int24.raw";
  const std::array big_bytes{std::byte{0xff}, std::byte{0xff}, std::byte{0xfe}};
  write_bytes(big_path, big_bytes);
  auto big_descriptor = real_descriptor(ScalarType::int24_packed, Endianness::big, 1U);
  auto big_result = read_raw_samples(big_path, big_descriptor, range(0U, 1U), 3U);
  require(big_result && big_result.value().samples.view().real_values()[0] == -2.0, "大端 Int24 解码错误");
}

void test_iq_layouts(const std::filesystem::path& root) {
  const std::array values{ComplexSample{1.0, -2.0}, ComplexSample{3.0, -4.0}};
  for (const auto layout : {ComponentLayout::interleaved, ComponentLayout::planar}) {
    for (const auto order : {ComponentOrder::iq, ComponentOrder::qi}) {
      const auto path =
          make_sc16(root, std::to_string(static_cast<int>(layout)) + std::to_string(static_cast<int>(order)) + ".raw",
                    values, order, layout);
      auto result = read_raw_samples(path, sc16_descriptor(values.size(), order, layout), range(0U, 2U), 8U);
      require(result && result.value().samples.view().complex_values()[1] == values[1], "IQ/QI 或布局解码错误");
    }
  }
}

void test_descriptor_validation(const std::filesystem::path& root) {
  auto one_byte = real_descriptor(ScalarType::int8, Endianness::little, 1U);
  require(!one_byte.validate(), "单字节类型不得接受字节序");
  auto multi_byte = real_descriptor(ScalarType::int16, Endianness::not_applicable, 1U);
  require(!multi_byte.validate(), "多字节类型必须要求字节序");
  const auto truncated = root / "truncated.raw";
  const std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}};
  write_bytes(truncated, bytes);
  require(!calculate_data_facts(3U, sc16_descriptor(1U), 3U), "残帧必须拒绝");
  auto complete = sc16_descriptor(1U);
  complete.amplitude_mode = "scaled";
  complete.scale_factor = 0.5;
  complete.additive_offset = 1.0;
  complete.start_time_seconds = 2.0;
  require(complete.validate(), "完整导入描述符必须通过");
}

void test_frequency_semantics() {
  auto descriptor = real_descriptor(ScalarType::float32, Endianness::little, 1U);
  require(display_frequency(125.0, descriptor, FrequencyReference::baseband).value() == 125.0, "相对频率错误");
  require(!display_frequency(125.0, descriptor, FrequencyReference::absolute), "缺少中心频率时不得显示绝对频率");
  descriptor.signal_kind = SignalKind::complex;
  descriptor.component_layout = ComponentLayout::interleaved;
  descriptor.component_order = ComponentOrder::iq;
  descriptor.center_frequency_hz = 1000.0;
  require(display_frequency(125.0, descriptor, FrequencyReference::absolute).value() == 1125.0,
          "绝对频率映射必须保持同一 bin");
}

void test_wav(const std::filesystem::path& root) {
  const auto path = make_wav(root);
  require(!read_wav_descriptor(path, false), "立体声 WAV 未确认 IQ 时必须拒绝");
  auto wav = read_wav_descriptor(path, true, ComponentOrder::iq);
  require(wav && wav.value().descriptor.signal_kind == SignalKind::complex &&
              wav.value().descriptor.requested_sample_range.size() == 2U,
          "WAV 描述符错误");
  auto samples = read_raw_samples(path, wav.value().descriptor, range(0U, 2U), 8U);
  require(samples && samples.value().samples.view().complex_values()[0].imag == -2.0 / 32768.0, "WAV IQ 数据错误");
}

void test_adapters() {
  FormatAdapterRegistry registry;
  const auto factory = [](const std::filesystem::path&) -> signal::core::Result<SignalDescriptor> {
    auto descriptor = real_descriptor(ScalarType::float32, Endianness::little, 1U);
    descriptor.provenance["scalarType"] = {FieldOrigin::adapter, true};
    return descriptor;
  };
  require(register_standard_descriptor_adapters(registry, factory, factory, factory), "标准描述符适配器注册失败");
  require(registry.adapter_ids().size() == 3U, "适配器注册数量错误");
  auto npz = registry.describe("input.npz");
  require(npz && npz.value().descriptor_only && npz.value().adapter_id == "numpy-descriptor", "NPY/NPZ 描述符适配错误");
}

void test_preview_quality(const std::filesystem::path& root) {
  std::vector<std::byte> bytes;
  append_little(bytes, 0.0);
  append_little(bytes, 2.0);
  append_little(bytes, std::numeric_limits<double>::quiet_NaN());
  append_little(bytes, std::numeric_limits<double>::infinity());
  const auto path = root / "quality.raw";
  write_bytes(path, bytes);
  auto descriptor = real_descriptor(ScalarType::float64, Endianness::little, 4U);
  PreviewOptions options;
  options.maximum_samples = 4U;
  options.maximum_read_bytes = bytes.size();
  options.spectrum_bins = 2U;
  CancellationToken token;
  auto preview = create_bounded_preview(path, descriptor, options, token);
  require(preview && preview.value().scope_label == "bounded-preview" &&
              std::find(preview.value().warnings.begin(), preview.value().warnings.end(),
                        QualityWarning::nan_present) != preview.value().warnings.end() &&
              std::find(preview.value().warnings.begin(), preview.value().warnings.end(),
                        QualityWarning::infinity_present) != preview.value().warnings.end() &&
              std::find(preview.value().warnings.begin(), preview.value().warnings.end(), QualityWarning::clipping) !=
                  preview.value().warnings.end(),
          "预览统计或质量告警错误");
  PreviewCoordinator coordinator;
  auto old = coordinator.begin_request();
  static_cast<void>(coordinator.begin_request());
  require(old.cancelled(), "新预览必须取消旧预览");
  token.cancel();
  require(!create_bounded_preview(path, descriptor, options, token), "取消预览不得发布结果");
}

void test_sidecar() {
  auto descriptor = sc16_descriptor(4U);
  descriptor.amplitude_mode = "escaped-\"mode\nline";
  descriptor.provenance["sampleRateHz"] = {FieldOrigin::sidecar, false};
  descriptor.provenance["componentOrder"] = {FieldOrigin::user, true};
  auto json = serialize_sidecar(descriptor);
  require(json, "侧车序列化失败");
  auto parsed = parse_sidecar(json.value());
  require(parsed && parsed.value().provenance.at("componentOrder").confirmed &&
              !parsed.value().provenance.at("sampleRateHz").confirmed &&
              parsed.value().amplitude_mode == descriptor.amplitude_mode,
          "版本化侧车来源或确认状态丢失");
  auto incompatible = json.value();
  incompatible.replace(incompatible.find("signal.raw-descriptor/1.0"), 25U, "signal.raw-descriptor/2.0");
  require(!parse_sidecar(incompatible), "侧车主版本不兼容必须拒绝");
  require(!parse_sidecar(json.value() + " trailing"), "侧车尾随垃圾必须拒绝");
  auto duplicate = json.value();
  duplicate.insert(1U, "\"schema\":\"duplicate\",");
  require(!parse_sidecar(duplicate), "侧车重复键必须拒绝");
  require(!parse_sidecar("{\"wrapper\":" + json.value() + "}"), "侧车错误层级必须拒绝");
  auto wrong_provenance = json.value();
  const auto provenance = wrong_provenance.find("\"provenance\":{");
  wrong_provenance.replace(provenance, std::string{"\"provenance\":{"}.size(), "\"provenance\":[]");
  require(!parse_sidecar(wrong_provenance), "侧车 provenance 错误类型必须拒绝");
}

void test_facts_plan(const std::filesystem::path& root) {
  std::vector<ComplexSample> values(100U, {1.0, 2.0});
  const auto path = make_sc16(root, "facts.raw", values);
  auto descriptor = sc16_descriptor(values.size());
  auto facts = calculate_data_facts(400U, descriptor, 9U);
  require(facts && facts.value().available_frames == 100U && facts.value().initial_target_bytes == 8U &&
              std::abs(facts.value().duration_seconds - 0.000002) < 1e-12,
          "文件事实或初始化目标错误");
  auto plan = make_initial_load_plan(path, descriptor, 99U, 1000U, 64U);
  require(plan && plan.value().target_bytes == 4U && plan.value().requested_range == range(99U, 100U),
          "初始化目标未按实际剩余完整帧截断");
}

class RecordingTaskObserver final : public signal::task::ITaskObserver {
public:
  void on_event(const signal::task::TaskEvent& event) noexcept override {
    std::lock_guard lock{mutex_};
    events_.push_back(event);
  }
  [[nodiscard]] std::vector<signal::task::TaskEvent> snapshot() const {
    std::lock_guard lock{mutex_};
    return events_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<signal::task::TaskEvent> events_;
};

void test_loader(const std::filesystem::path& root) {
  using namespace std::chrono_literals;
  require(signal::task::evaluate_scheduling(signal::task::WorkClass::io, 51ms).must_schedule,
          "超过 50 ms 的加载 IO 必须由 TaskRuntime 调度");
  static std::uint64_t invocation{};
  const auto suffix = std::to_string(++invocation);
  std::vector<ComplexSample> values(16U, {1.0, 2.0});
  const auto path = make_sc16(root, "loader-" + suffix + ".raw", values);
  auto descriptor = sc16_descriptor(values.size());
  auto plan = make_initial_load_plan(path, descriptor, 0U, 64U, 4U);
  auto loader_result = IncrementalLoader::create("source-v1-" + suffix, path, descriptor, plan.value());
  require(loader_result, "加载器创建失败");
  auto loader = std::shared_ptr<IncrementalLoader>{std::move(loader_result.value())};

  signal::task::RuntimeConfig config;
  config.worker_count = 1U;
  config.budget = {1U, 1U, 0U, 1U};
  config.history_file = root / ("loader-history-" + suffix + ".log");
  signal::task::TaskRuntime runtime{config};
  auto observer = std::make_shared<RecordingTaskObserver>();
  const auto subscription = runtime.add_observer(observer);
  std::atomic_bool first_chunk{};
  std::atomic_bool permit_after_first{};
  signal::task::TaskSpec spec;
  spec.task_id = {"data-load-complete-" + suffix};
  spec.task_type = "data.load";
  spec.priority = signal::task::TaskPriority::foreground;
  spec.resources = {1U, 1U, 0U, 1U};
  spec.idempotency_key = "data-load-complete-" + suffix;
  spec.provenance = {{"project"}, {"source-v1-" + suffix}, {"data-source", "source"}};
  auto handle = runtime.submit(spec, [loader, &first_chunk, &permit_after_first](signal::task::TaskContext& context) {
    if (!loader->start()) {
      return signal::task::TaskExecutionResult::failed({"DATA.START", "加载启动失败", "IncrementalLoader::start failed",
                                                        "修改参数后重试", true, "edit_parameters",
                                                        "task://" + context.task_id().value + "/log"});
    }
    while (loader->snapshot().progress_state == LoadProgressState::reading) {
      if (!context.checkpoint()) {
        static_cast<void>(loader->cancel());
        return signal::task::TaskExecutionResult::completed();
      }
      const auto status = loader->process_next();
      if (!status) {
        return signal::task::TaskExecutionResult::failed(
            {status.code().stable_text(), std::string{status.message()}, "IncrementalLoader::process_next failed",
             "retry|edit_parameters|cancel|view_log", true, "retry", "task://" + context.task_id().value + "/log"});
      }
      const auto snapshot = loader->snapshot();
      static_cast<void>(context.report_progress(snapshot.progress, "frame-aligned source loading"));
      if (!first_chunk.exchange(true)) {
        while (!permit_after_first.load()) {
          if (!context.checkpoint()) {
            static_cast<void>(loader->cancel());
            return signal::task::TaskExecutionResult::completed();
          }
          std::this_thread::yield();
        }
      }
    }
    return signal::task::TaskExecutionResult::completed();
  });
  require(handle, "TaskRuntime 加载任务提交失败");
  for (int attempt = 0; attempt < 200 && !first_chunk.load(); ++attempt)
    std::this_thread::sleep_for(1ms);
  require(first_chunk.load() && handle.value().pause(), "TaskRuntime 未在完整帧边界暂停");
  for (int attempt = 0; attempt < 200; ++attempt) {
    auto status = handle.value().status();
    if (status && status.value().state == signal::task::TaskState::paused)
      break;
    std::this_thread::sleep_for(1ms);
  }
  const auto paused_status = handle.value().status();
  const auto paused_boundary = loader->snapshot().next_sample;
  require(paused_status && paused_status.value().state == signal::task::TaskState::paused,
          "唯一 TaskStatus 未显示暂停");
  permit_after_first.store(true);
  std::this_thread::sleep_for(5ms);
  require(loader->snapshot().next_sample == paused_boundary, "暂停期间加载边界发生变化");
  require(handle.value().resume(), "TaskRuntime 继续失败");
  const auto completed = handle.value().wait();
  require(completed && completed.value().state == signal::task::TaskState::completed &&
              loader->snapshot().progress_state == LoadProgressState::complete &&
              loader->snapshot().source_state == DataSourceState::complete,
          "TaskRuntime 驱动加载未完成");
  const auto events = observer->snapshot();
  require(!events.empty() && std::all_of(events.begin(), events.end(),
                                         [&](const auto& event) { return event.status.task_id == spec.task_id; }),
          "进度、任务中心和观察日志必须共享唯一 TaskId/TaskStatus 源");

  auto cancel_loader_result = IncrementalLoader::create("source-cancel-" + suffix, path, descriptor, plan.value());
  auto cancel_loader = std::shared_ptr<IncrementalLoader>{std::move(cancel_loader_result.value())};
  std::atomic_bool cancel_first{};
  signal::task::TaskSpec cancel_spec = spec;
  cancel_spec.task_id = {"data-load-cancel-" + suffix};
  cancel_spec.idempotency_key = "data-load-cancel-" + suffix;
  cancel_spec.provenance.data_source_version_id = {"source-cancel-" + suffix};
  auto cancel_handle = runtime.submit(cancel_spec, [cancel_loader, &cancel_first](signal::task::TaskContext& context) {
    static_cast<void>(cancel_loader->start());
    while (cancel_loader->snapshot().progress_state == LoadProgressState::reading) {
      if (!context.checkpoint()) {
        static_cast<void>(cancel_loader->cancel());
        return signal::task::TaskExecutionResult::completed();
      }
      const auto status = cancel_loader->process_next();
      if (!status)
        return signal::task::TaskExecutionResult::failed(
            {status.code().stable_text(), std::string{status.message()}, "IncrementalLoader::process_next failed",
             "retry|edit_parameters|cancel|view_log", true, "retry", "task://" + context.task_id().value + "/log"});
      cancel_first.store(true);
      while (cancel_first.load()) {
        if (!context.checkpoint()) {
          static_cast<void>(cancel_loader->cancel());
          return signal::task::TaskExecutionResult::completed();
        }
        std::this_thread::yield();
      }
    }
    return signal::task::TaskExecutionResult::completed();
  });
  for (int attempt = 0; attempt < 200 && !cancel_first.load(); ++attempt)
    std::this_thread::sleep_for(1ms);
  require(cancel_first.load() && cancel_handle.value().cancel(), "TaskRuntime 取消请求失败");
  const auto cancelled = cancel_handle.value().wait();
  const auto cancelled_source = cancel_loader->snapshot();
  require(cancelled && cancelled.value().state == signal::task::TaskState::canceled &&
              cancelled_source.progress_state == LoadProgressState::cancelled &&
              cancelled_source.source_state == DataSourceState::partial_read_available &&
              cancelled_source.published_range && cancelled_source.published_range->range() == range(0U, 1U),
          "任务取消与部分数据源状态未正交或越过完整帧");

  const auto malformed = root / ("loader-malformed-" + suffix + ".raw");
  const std::array malformed_bytes{std::byte{1}, std::byte{2}, std::byte{3}};
  write_bytes(malformed, malformed_bytes);
  LoadPlan bad_plan{range(0U, 1U), 4U, 4U, 4U};
  auto bad_result = IncrementalLoader::create("source-bad-" + suffix, malformed, sc16_descriptor(1U), bad_plan);
  auto bad = std::shared_ptr<IncrementalLoader>{std::move(bad_result.value())};
  signal::task::TaskSpec bad_spec = spec;
  bad_spec.task_id = {"data-load-failure-" + suffix};
  bad_spec.idempotency_key = "data-load-failure-" + suffix;
  auto bad_handle = runtime.submit(bad_spec, [bad](signal::task::TaskContext& context) {
    static_cast<void>(bad->start());
    const auto status = bad->process_next();
    if (status)
      return signal::task::TaskExecutionResult::completed();
    return signal::task::TaskExecutionResult::failed(
        {status.code().stable_text(), std::string{status.message()}, "IncrementalLoader::process_next failed",
         "retry|edit_parameters|cancel|view_log", true, "retry", "task://" + context.task_id().value + "/log"});
  });
  const auto failed = bad_handle.value().wait();
  const auto failure = bad->snapshot();
  require(failed && failed.value().state == signal::task::TaskState::failed, "解析失败未形成失败 TaskStatus");
  require(failed.value().failure && failed.value().failure->log_link == "task://" + bad_spec.task_id.value + "/log",
          "TaskRuntime 失败状态缺少同一 TaskId 的日志入口");
  require(failed.value().failure->suggested_action == "retry|edit_parameters|cancel|view_log",
          "TaskRuntime 失败状态缺少结构化恢复动作");
  require(failure.progress_state == LoadProgressState::failed && !failure.published_range && failure.failure,
          "解析失败时发布了未验证数据或缺少 Data 失败状态");
  require(failure.failure->recovery_actions ==
              std::vector<ImportRecoveryAction>{ImportRecoveryAction::retry, ImportRecoveryAction::edit_parameters,
                                                ImportRecoveryAction::cancel, ImportRecoveryAction::view_log},
          "Data 失败状态缺少 retry/edit_parameters/cancel/view_log 动作");
  require(!failure.failure->log_uri.empty() && !failure.logs.empty(), "Data 失败状态缺少日志入口或日志记录");
  runtime.remove_observer(subscription);
  runtime.shutdown();
}

void test_pyramid_progressive() {
  const auto buffer = SignalBuffer::from_real({1.0, -2.0, 3.0, -4.0});
  auto pyramid = TimeSummaryPyramid::build(buffer.view(), range(10U, 14U));
  require(pyramid && pyramid.value().level_count() == 3U, "时域金字塔层数错误");
  auto viewport = pyramid.value().viewport(range(10U, 14U), 2U);
  require(viewport && viewport.value().size() == 2U && viewport.value()[0].minimum == -2.0 &&
              std::abs(viewport.value()[0].rms - std::sqrt(2.5)) < 1e-12,
          "时域 min/max/RMS 汇总错误");
  ProgressiveIndexStatus status{14U};
  require(!status.transition(ProgressiveIndexState::building, 0.2), "渐进索引不得跳过采样概览直接构建");
  require(!status.transition(ProgressiveIndexState::sample_overview, 0.1), "采样概览缺少短突发遗漏提示时必须拒绝");
  require(status.transition(ProgressiveIndexState::sample_overview, 0.1, "采样概览可能遗漏短突发"), "采样概览状态失败");
  require(status.transition(ProgressiveIndexState::building, 0.5), "构建中状态失败");
  require(status.transition(ProgressiveIndexState::complete, 1.0) && status.index_upper_bound() == 14U,
          "完整索引状态或部分读取上界错误");
  require(status.transition(ProgressiveIndexState::degraded, 1.0, "cache unavailable"), "降级状态失败");
  require(!validate_range_extension(range(0U, 14U), "source-v1", range(0U, 20U), "source-v1") &&
              validate_range_extension(range(0U, 14U), "source-v1", range(0U, 20U), "source-v2"),
          "扩大部分读取范围必须创建新数据源版本");

  std::vector<ComplexSample> tone;
  for (std::uint64_t index = 0; index < 8U; ++index) {
    const double phase = 2.0 * std::numbers::pi * 2.0 * static_cast<double>(index) / 8.0;
    tone.push_back({std::cos(phase), std::sin(phase)});
  }
  const auto tone_buffer = SignalBuffer::from_complex(std::move(tone));
  DirectDftTileProducer dft{tone_buffer.view(), range(0U, 8U), 8.0};
  auto spectrum_key = cache_key(0U, 8U, TileKind::spectrum_summary);
  spectrum_key.loaded_range = range(0U, 8U);
  spectrum_key.pixel_width = 3U;
  spectrum_key.pixel_height = 2U;
  spectrum_key.frequency_begin_hz = 0;
  spectrum_key.frequency_end_hz = 4;
  auto spectrum = dft.produce({TileKind::spectrum_summary, spectrum_key, false}, CancellationToken{});
  require(spectrum && spectrum.value().height == 1U && spectrum.value().values.size() == 3U &&
              spectrum.value().values[1] > 0.99F && spectrum.value().values[0] < 0.01F,
          "真实频谱概要 DFT 结果错误");
  auto stft_key = spectrum_key;
  stft_key.tile_kind = TileKind::stft;
  auto stft = dft.produce({TileKind::stft, stft_key, false}, CancellationToken{});
  require(stft && stft.value().height == 2U && stft.value().values.size() == 6U && stft.value().values[1] > 0.99F &&
              stft.value().values[4] > 0.99F,
          "真实 STFT 直接 DFT 瓦片结果错误");
}

void test_cache(const std::filesystem::path& root) {
  static std::uint64_t invocation{};
  auto key = cache_key();
  require(key.validate(), "完整缓存键必须有效");
  auto incompatible = key;
  incompatible.pixel_width += 1U;
  require(key.canonical() != incompatible.canonical(), "视口像素变化不得命中同一键");
  auto incompatible_kind = key;
  incompatible_kind.tile_kind = TileKind::stft;
  require(key.canonical() != incompatible_kind.canonical() && !TileRequest{TileKind::stft, key, false}.validate(),
          "频谱与 STFT 瓦片不得共享缓存身份");
  auto memory = MemoryTileCache::create(1'000'000U, 25U);
  require(memory && memory.value()->budget_bytes() == 250'000U, "内存缓存默认预算错误");
  require(!MemoryTileCache::create(1'000'000U, 9U) && !MemoryTileCache::create(1'000'000U, 61U),
          "内存预算必须限制在 10%-60%");

  const auto directory = root / ("tiles-" + std::to_string(++invocation));
  auto disk = std::make_unique<DiskTileStore>(directory, 1'000'000U);
  require(disk->recover(), "磁盘缓存恢复失败");
  MultiResolutionTileStore store{std::move(memory.value()), std::move(disk)};
  ProgressiveIndexStatus index_status{key.loaded_range.end()};
  require(index_status.transition(ProgressiveIndexState::sample_overview, 0.1, "采样概览可能遗漏短突发") &&
              index_status.transition(ProgressiveIndexState::building, 0.5),
          "索引诊断状态准备失败");
  store.update_index_status(index_status);
  CountingProducer producer;
  TileRequest request{TileKind::spectrum_summary, key, false};
  CancellationToken cancellation;
  auto first = store.get_or_produce(request, producer, cancellation);
  auto second = store.get_or_produce(request, producer, cancellation);
  IMultiResolutionStore& store_contract = store;
  auto contract_hit = store_contract.get(request);
  require(first && second && producer.calls == 1 && store.diagnostics().memory_hits >= 1U && contract_hit &&
              store.diagnostics().index_progress == 0.5 && store.diagnostics().coverage == 0.5 &&
              !store.diagnostics().active_level.empty() && store.diagnostics().disk_bytes > 0U &&
              store.diagnostics().misses >= 1U && !store.diagnostics().invalidation_reason.empty() &&
              !store.diagnostics().degraded,
          "缓存命中或诊断错误");
  require(store_contract.pin(request), "当前任务缓存资产 pin 失败");
  store_contract.unpin(request);
  auto changed_source_key = key;
  changed_source_key.source_fingerprint = "source-fingerprint-v2";
  changed_source_key.data_source_version_id = "source-version-v2";
  auto changed_source =
      store.get_or_produce(TileRequest{TileKind::spectrum_summary, changed_source_key, false}, producer, cancellation);
  require(changed_source && producer.calls == 2, "源版本变化后旧缓存不得继续命中");

  auto memory2 = MemoryTileCache::create(1'000'000U, 25U);
  auto disk2 = std::make_unique<DiskTileStore>(directory, 1'000'000U);
  MultiResolutionTileStore recovered{std::move(memory2.value()), std::move(disk2)};
  auto from_disk = recovered.get_or_produce(request, producer, cancellation);
  require(from_disk && producer.calls == 2 && recovered.diagnostics().disk_hits == 1U, "已提交磁盘瓦片未恢复");

  auto tile_file = std::filesystem::path{};
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() == ".tile")
      tile_file = entry.path();
  }
  require(!tile_file.empty(), "未找到磁盘瓦片");
  {
    std::fstream stream{tile_file, std::ios::binary | std::ios::in | std::ios::out};
    stream.seekp(12);
    const char corrupt = '\x7f';
    stream.write(&corrupt, 1);
  }
  auto memory3 = MemoryTileCache::create(1'000'000U, 25U);
  auto disk3 = std::make_unique<DiskTileStore>(directory, 1'000'000U);
  MultiResolutionTileStore fallback{std::move(memory3.value()), std::move(disk3)};
  auto repaired = fallback.get_or_produce(request, producer, cancellation);
  require(repaired && producer.calls == 3 && fallback.diagnostics().corruptions == 1U, "损坏缓存必须回退按需生产");

  auto temporary = tile_file;
  temporary += ".tmp";
  const std::array junk{std::byte{1}};
  write_bytes(temporary, junk);
  DiskTileStore recovery{directory, 1'000'000U};
  require(recovery.recover() && !std::filesystem::exists(temporary), "未提交瓦片必须在恢复时清理");

  auto small_memory = MemoryTileCache::create(2'000U, 10U);
  auto first_tile =
      std::make_shared<const Tile>(Tile{TileKind::spectrum_summary, key.time_viewport, key.frequency_begin_hz,
                                        key.frequency_end_hz, 4U, 2U, std::vector<float>(8U, 1.0F)});
  auto second_key = key;
  second_key.parameter_digest = "second";
  auto second_tile =
      std::make_shared<const Tile>(Tile{TileKind::spectrum_summary, key.time_viewport, key.frequency_begin_hz,
                                        key.frequency_end_hz, 4U, 2U, std::vector<float>(8U, 2.0F)});
  require(small_memory.value()->put(key, first_tile) && small_memory.value()->pin(key),
          "小缓存当前资产写入或 pin 失败");
  require(!small_memory.value()->put(second_key, second_tile) && small_memory.value()->get(key),
          "pin 的当前任务资产不得因容量压力被清理");
  small_memory.value()->unpin(key);
  require(small_memory.value()->put(second_key, second_tile) && !small_memory.value()->get(key) &&
              small_memory.value()->get(second_key) && small_memory.value()->diagnostics().evictions >= 1U,
          "解除 pin 后 LRU 淘汰或容量约束错误");
  require(index_status.transition(ProgressiveIndexState::degraded, 0.5, "disk capacity unavailable"),
          "缓存降级状态转换失败");
  store.update_index_status(index_status);
  require(store.diagnostics().degraded && store.diagnostics().invalidation_reason == "disk capacity unavailable",
          "缓存降级诊断字段不完整");
}

void test_prefetch(const std::filesystem::path& root) {
  auto memory = MemoryTileCache::create(1'000'000U, 25U);
  auto disk = std::make_unique<DiskTileStore>(root / "prefetch", 1'000'000U);
  MultiResolutionTileStore store{std::move(memory.value()), std::move(disk)};
  CountingProducer producer;
  PrefetchQueue queue;
  queue.enqueue({TileKind::stft, cache_key(0U, 8U, TileKind::stft), true});
  require(queue.pending() == 1U, "预取入队失败");
  CancellationToken interactive_cancellation;
  auto interactive =
      queue.run_interactive(store, producer, TileRequest{TileKind::stft, cache_key(16U, 24U, TileKind::stft), false},
                            interactive_cancellation);
  require(interactive && producer.calls == 1 && queue.pending() == 0U, "当前交互请求必须取消并优先于待执行预取");
  queue.enqueue({TileKind::stft, cache_key(8U, 16U, TileKind::stft), true});
  require(queue.run_next(store, producer) && producer.calls == 2, "STFT 通用瓦片生产契约失败");
}

void test_io_and_external_hook(const std::filesystem::path& root) {
  const std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const auto path = root / "mapped.raw";
  write_bytes(path, bytes);
  const auto before = std::filesystem::last_write_time(path);
  BoundedFileReader reader{path, 2U};
  require(!reader.read(0U, 3U), "分块读取必须执行显式上界");
  auto mapped = MappedFileWindow::open(path, 1U, 2U, 2U);
  require(mapped && mapped.value().bytes()[0] == std::byte{2}, "只读映射窗口错误");
  require(std::filesystem::last_write_time(path) == before, "读取不得修改源文件");

  const auto repository = std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path();
  const auto external = repository.parent_path() / "test_data";
  const auto large = external / "x310_capture_cf1245MHz_sr50MSps_20260521_144927.sc16";
  if (std::filesystem::is_regular_file(large)) {
    BoundedFileReader external_reader{large, 4096U};
    auto prefix = external_reader.read(0U, 4096U);
    auto window = MappedFileWindow::open(large, 4096U, 4096U, 4096U);
    require(prefix && prefix.value().size() == 4096U && window && window.value().bytes().size() == 4096U,
            "外部大文件有界集成读取失败");
  }
  const auto wav_path = external / "20241110-174401-662_bw_12800000_sampleTime_0.4_rollOff_0.3.wav";
  if (std::filesystem::is_regular_file(wav_path)) {
    auto wav = read_wav_descriptor(wav_path, true, ComponentOrder::iq);
    require(wav && wav.value().descriptor.sample_rate_hz == 12'800'000.0 &&
                wav.value().descriptor.requested_sample_range.size() == 5'120'000U &&
                wav.value().data_bytes == 20'480'000U,
            "外部 WAV 描述符与已批准测试数据说明不一致");
    auto preview = create_bounded_preview(wav_path, wav.value().descriptor,
                                          PreviewOptions{4096U, 16U * 1024U * 1024U, 64U}, CancellationToken{});
    require(preview && preview.value().statistics.sample_count == 4096U &&
                preview.value().spectrum.analyzed_samples <= 4096U && !preview.value().whole_file,
            "外部 WAV 有界预览集成失败");
  }
}

struct RequirementCase final {
  std::string_view id;
  std::function<void()> run;
};

} // namespace

int main(int argc, char** argv) {
  const auto root = fixture_root();
  const std::vector<RequirementCase> cases{
      {"FR-DAT-001",
       [&] {
         test_ranges_buffers();
         test_data_closed_loop(root);
       }},
      {"FR-DAT-002", [&] { test_all_scalar_types(root); }},
      {"FR-DAT-003", [&] { test_iq_layouts(root); }},
      {"FR-DAT-004", [&] { test_descriptor_validation(root); }},
      {"FR-DAT-005", [&] { test_descriptor_validation(root); }},
      {"FR-DAT-006", test_frequency_semantics},
      {"FR-DAT-007", test_frequency_semantics},
      {"FR-DAT-008", [&] { test_wav(root); }},
      {"FR-DAT-009", test_adapters},
      {"FR-DAT-010", [&] { test_preview_quality(root); }},
      {"FR-DAT-011", [&] { test_preview_quality(root); }},
      {"FR-DAT-012", test_sidecar},
      {"FR-DAT-013", [&] { test_facts_plan(root); }},
      {"FR-DAT-014", [&] { test_facts_plan(root); }},
      {"FR-DAT-015", [&] { test_loader(root); }},
      {"FR-DAT-016", [&] { test_loader(root); }},
      {"FR-DAT-017", [&] { test_loader(root); }},
      {"FR-DAT-018", [&] { test_loader(root); }},
      {"FR-DAT-019", [&] { test_loader(root); }},
      {"FR-IDX-001", test_pyramid_progressive},
      {"FR-IDX-002", test_pyramid_progressive},
      {"FR-IDX-003", [&] { test_cache(root); }},
      {"FR-IDX-004", [&] { test_cache(root); }},
      {"FR-IDX-005", [&] { test_cache(root); }},
      {"FR-IDX-006", [&] { test_cache(root); }},
      {"FR-IDX-007", [&] { test_cache(root); }},
      {"FR-IDX-008", [&] { test_cache(root); }},
      {"FR-IDX-009", test_pyramid_progressive},
      {"FR-IDX-010", [&] { test_cache(root); }},
      {"FR-IDX-011", [&] { test_prefetch(root); }},
      {"FR-IDX-012", test_pyramid_progressive},
      {"FR-DATA-101", [&] { test_io_and_external_hook(root); }},
  };
  try {
    if (argc == 3 && std::string_view{argv[1]} == "--case") {
      const std::string_view requested{argv[2]};
      const auto found = std::find_if(cases.begin(), cases.end(),
                                      [requested](const RequirementCase& test) { return test.id == requested; });
      if (found == cases.end()) {
        std::cerr << "Unknown requirement case: " << requested << '\n';
        return 2;
      }
      found->run();
      std::cout << "PASS " << found->id << '\n';
      return 0;
    }
    if (argc != 1) {
      std::cerr << "Usage: signal_studio_data_tests [--case REQUIREMENT-ID]\n";
      return 2;
    }
    for (const auto& test : cases) {
      test.run();
      std::cout << "PASS " << test.id << '\n';
    }
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  std::cout << "SignalData MS-01: " << cases.size() << " requirement mappings passed\n";
  return 0;
}
