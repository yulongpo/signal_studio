#include "signal_studio/visualization/visualization.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace signal::visualization {
namespace {

[[nodiscard]] core::Status failure(core::ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return core::Status::failure({core::ErrorDomain::visualization, reason}, std::move(message), std::move(diagnostic));
}

[[nodiscard]] std::string trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

[[nodiscard]] std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    if (character >= 'A' && character <= 'Z') {
      return static_cast<char>(character - 'A' + 'a');
    }
    return static_cast<char>(character);
  });
  return value;
}

[[nodiscard]] bool valid_token(std::string_view value) {
  return !value.empty() && value.find_first_of("\t\r\n") == std::string_view::npos;
}

[[nodiscard]] std::uint64_t divisor_for_digits(std::size_t digits) {
  std::uint64_t value = 1;
  for (std::size_t index = 0; index < digits; ++index) {
    if (value > std::numeric_limits<std::uint64_t>::max() / 10U) {
      return 0;
    }
    value *= 10U;
  }
  return value;
}

[[nodiscard]] core::Result<ChartKind> parse_chart_kind(std::string_view text) {
  std::uint32_t value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value > static_cast<std::uint32_t>(ChartKind::eye_diagram)) {
    return failure(core::ErrorReason::invalid_argument, "图表类型无效");
  }
  return static_cast<ChartKind>(value);
}

} // namespace

std::int64_t FrequencyRange::bandwidth_hz() const noexcept {
  if (begin_hz >= end_hz) {
    return 0;
  }
  const auto span = static_cast<std::uint64_t>(end_hz) - static_cast<std::uint64_t>(begin_hz);
  return span > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
             ? std::numeric_limits<std::int64_t>::max()
             : static_cast<std::int64_t>(span);
}

bool FrequencyRange::contains(std::int64_t frequency_hz) const noexcept {
  return frequency_hz >= begin_hz && frequency_hz <= end_hz;
}

bool FrequencyRange::contains(const FrequencyRange& other) const noexcept {
  return other.begin_hz >= begin_hz && other.end_hz <= end_hz;
}

core::Result<FrequencyRange> make_frequency_range(std::int64_t begin_hz, std::int64_t end_hz) {
  if (begin_hz >= end_hz) {
    return failure(core::ErrorReason::invalid_argument, "频率范围必须为非空升序区间");
  }
  const auto span = static_cast<std::uint64_t>(end_hz) - static_cast<std::uint64_t>(begin_hz);
  if (span > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return failure(core::ErrorReason::invalid_argument, "频率跨度不能超过有符号 64 位整数 Hz");
  }
  return FrequencyRange{begin_hz, end_hz};
}

core::Result<std::int64_t> parse_frequency_hz(std::string_view text) {
  auto normalized = lower(trim(text));
  if (normalized.empty()) {
    return failure(core::ErrorReason::invalid_argument, "频率输入不能为空");
  }

  std::uint64_t scale = 1;
  for (const auto& [suffix, candidate_scale] : std::initializer_list<std::pair<std::string_view, std::uint64_t>>{
           {"ghz", 1'000'000'000ULL}, {"mhz", 1'000'000ULL}, {"khz", 1'000ULL}, {"hz", 1ULL}}) {
    if (normalized.ends_with(suffix)) {
      scale = candidate_scale;
      normalized.resize(normalized.size() - suffix.size());
      normalized = trim(normalized);
      break;
    }
  }

  bool negative = false;
  if (!normalized.empty() && (normalized.front() == '+' || normalized.front() == '-')) {
    negative = normalized.front() == '-';
    normalized.erase(normalized.begin());
  }
  if (normalized.empty()) {
    return failure(core::ErrorReason::invalid_argument, "频率数值无效");
  }

  const auto point = normalized.find('.');
  if (point != std::string::npos && normalized.find('.', point + 1) != std::string::npos) {
    return failure(core::ErrorReason::invalid_argument, "频率只能包含一个小数点");
  }
  const auto whole_text =
      point == std::string::npos ? std::string_view(normalized) : std::string_view(normalized).substr(0, point);
  const auto fraction_text =
      point == std::string::npos ? std::string_view{} : std::string_view(normalized).substr(point + 1);
  if (whole_text.empty() ||
      (!fraction_text.empty() && fraction_text.find_first_not_of("0123456789") != std::string_view::npos) ||
      whole_text.find_first_not_of("0123456789") != std::string_view::npos) {
    return failure(core::ErrorReason::invalid_argument, "频率必须是十进制数值");
  }

  std::uint64_t whole{};
  auto parsed = std::from_chars(whole_text.data(), whole_text.data() + whole_text.size(), whole);
  constexpr auto positive_limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  const auto magnitude_limit = negative ? positive_limit + 1U : positive_limit;
  if (parsed.ec != std::errc{} || parsed.ptr != whole_text.data() + whole_text.size() ||
      whole > magnitude_limit / scale) {
    return failure(core::ErrorReason::invalid_argument, "频率整数部分超出范围");
  }
  std::uint64_t magnitude = whole * scale;

  if (!fraction_text.empty()) {
    std::uint64_t fraction{};
    parsed = std::from_chars(fraction_text.data(), fraction_text.data() + fraction_text.size(), fraction);
    const auto divisor = divisor_for_digits(fraction_text.size());
    const auto scale_would_overflow = fraction > std::numeric_limits<std::uint64_t>::max() / scale;
    const auto scaled_fraction = scale_would_overflow ? 0U : scale * fraction;
    if (parsed.ec != std::errc{} || parsed.ptr != fraction_text.data() + fraction_text.size() || divisor == 0 ||
        scale_would_overflow || scaled_fraction % divisor != 0) {
      return failure(core::ErrorReason::invalid_argument, "频率小数不能精确表示为整数 Hz");
    }
    const auto fractional_hz = scaled_fraction / divisor;
    if (fractional_hz > magnitude_limit - magnitude) {
      return failure(core::ErrorReason::invalid_argument, "频率超出 64 位整数范围");
    }
    magnitude += fractional_hz;
  }
  if (magnitude > magnitude_limit) {
    return failure(core::ErrorReason::invalid_argument, "频率超出 64 位整数范围");
  }
  if (negative && magnitude == positive_limit + 1U) {
    return std::numeric_limits<std::int64_t>::min();
  }
  const auto signed_value = static_cast<std::int64_t>(magnitude);
  return negative ? -signed_value : signed_value;
}

std::string format_frequency_hz(std::int64_t frequency_hz) {
  const auto magnitude = frequency_hz < 0 ? static_cast<std::uint64_t>(-(frequency_hz + 1)) + 1U
                                          : static_cast<std::uint64_t>(frequency_hz);
  std::uint64_t divisor = 1;
  std::string_view unit = "Hz";
  if (magnitude >= 1'000'000'000ULL) {
    divisor = 1'000'000'000ULL;
    unit = "GHz";
  } else if (magnitude >= 1'000'000ULL) {
    divisor = 1'000'000ULL;
    unit = "MHz";
  } else if (magnitude >= 1'000ULL) {
    divisor = 1'000ULL;
    unit = "kHz";
  }
  std::ostringstream output;
  if (frequency_hz < 0) {
    output << '-';
  }
  output << magnitude / divisor;
  const auto remainder = magnitude % divisor;
  if (remainder != 0) {
    const auto digits = static_cast<int>(std::log10(static_cast<double>(divisor)));
    output << '.' << std::setw(digits) << std::setfill('0') << remainder;
    auto value = output.str();
    while (!value.empty() && value.back() == '0') {
      value.pop_back();
    }
    return value + " " + std::string(unit);
  }
  output << ' ' << unit;
  return output.str();
}

ViewportController::ViewportController(std::string scope) : scope_(std::move(scope)) {
  if (scope_.empty()) {
    scope_ = "visualization";
  }
}

core::Result<task::ViewRequestId>
ViewportController::bind_source(std::string data_source_version_id, data::SampleRange loaded_range,
                                FrequencyRange effective_frequency_range, bool partial_read,
                                std::uint64_t loaded_bytes, // NOLINT(bugprone-easily-swappable-parameters)
                                std::uint64_t physical_bytes) {
  if (data_source_version_id.empty() || loaded_range.empty()) {
    return failure(core::ErrorReason::invalid_argument, "非空数据源版本和实际读入范围是进入分析的前提");
  }
  const auto validated_frequency =
      make_frequency_range(effective_frequency_range.begin_hz, effective_frequency_range.end_hz);
  if (!validated_frequency) {
    return validated_frequency.error();
  }
  snapshot_.data_source_version_id = std::move(data_source_version_id);
  snapshot_.loaded_range = loaded_range;
  snapshot_.effective_frequency_range = effective_frequency_range;
  snapshot_.frequency_viewport = effective_frequency_range;
  snapshot_.partial_read = partial_read;
  snapshot_.loaded_bytes = loaded_bytes;
  snapshot_.physical_bytes = physical_bytes;
  const auto recent = recent_.find(snapshot_.data_source_version_id);
  if (recent == recent_.end()) {
    snapshot_.time_viewport = loaded_range;
  } else {
    const auto begin = std::clamp(recent->second.begin(), loaded_range.begin(), loaded_range.end() - 1U);
    const auto end = std::clamp(recent->second.end(), begin + 1U, loaded_range.end());
    snapshot_.time_viewport = data::SampleRange::make(begin, end).value();
  }
  snapshot_.request_id = issue_request();
  return snapshot_.request_id;
}

core::Result<task::ViewRequestId> ViewportController::set_time(data::SampleRange viewport) {
  if (snapshot_.loaded_range.empty() || viewport.empty() || !snapshot_.loaded_range.contains(viewport)) {
    return failure(core::ErrorReason::invalid_argument, "时间视窗必须是 LoadedDataRange 内的非空半开区间");
  }
  snapshot_.time_viewport = viewport;
  snapshot_.request_id = issue_request();
  save_recent();
  return snapshot_.request_id;
}

core::Result<task::ViewRequestId> ViewportController::pan_time(std::int64_t samples) {
  if (snapshot_.time_viewport.empty()) {
    return failure(core::ErrorReason::invalid_argument, "尚未绑定时间视窗");
  }
  const auto span = snapshot_.time_viewport.size();
  const auto loaded_begin = snapshot_.loaded_range.begin();
  const auto latest_begin = snapshot_.loaded_range.end() - span;
  std::uint64_t begin = snapshot_.time_viewport.begin();
  if (samples < 0) {
    const auto delta = static_cast<std::uint64_t>(-(samples + 1)) + 1U;
    begin = delta > begin - loaded_begin ? loaded_begin : begin - delta;
  } else {
    const auto delta = static_cast<std::uint64_t>(samples);
    begin = delta > latest_begin - begin ? latest_begin : begin + delta;
  }
  return set_time(data::SampleRange::make(begin, begin + span).value());
}

core::Result<task::ViewRequestId> ViewportController::resize_time(std::uint64_t begin, std::uint64_t span) {
  if (span == 0 || begin > std::numeric_limits<std::uint64_t>::max() - span) {
    return failure(core::ErrorReason::invalid_argument, "时间跨度必须为非零且不能溢出");
  }
  const auto range = data::SampleRange::make(begin, begin + span);
  if (!range) {
    return range.error();
  }
  return set_time(range.value());
}

core::Result<task::ViewRequestId> ViewportController::set_frequency(FrequencyRange viewport) {
  const auto validated = make_frequency_range(viewport.begin_hz, viewport.end_hz);
  if (!validated || !snapshot_.effective_frequency_range.contains(viewport)) {
    return failure(core::ErrorReason::invalid_argument, "频率视口必须位于数据有效全频范围内");
  }
  snapshot_.frequency_viewport = viewport;
  snapshot_.request_id = issue_request();
  return snapshot_.request_id;
}

core::Result<task::ViewRequestId> ViewportController::reset_frequency() {
  if (snapshot_.effective_frequency_range.bandwidth_hz() <= 0) {
    return failure(core::ErrorReason::invalid_argument, "尚未绑定有效频率范围");
  }
  snapshot_.frequency_viewport = snapshot_.effective_frequency_range;
  snapshot_.request_id = issue_request();
  return snapshot_.request_id;
}

core::Result<task::ViewRequestId> ViewportController::restore_recent(std::string_view data_source_version_id) {
  if (data_source_version_id != snapshot_.data_source_version_id) {
    return failure(core::ErrorReason::invalid_argument, "最近视窗必须按数据源版本恢复");
  }
  const auto entry = recent_.find(data_source_version_id);
  if (entry == recent_.end()) {
    return set_time(snapshot_.loaded_range);
  }
  const auto begin =
      std::clamp(entry->second.begin(), snapshot_.loaded_range.begin(), snapshot_.loaded_range.end() - 1U);
  const auto end = std::clamp(entry->second.end(), begin + 1U, snapshot_.loaded_range.end());
  return set_time(data::SampleRange::make(begin, end).value());
}

void ViewportController::save_recent() {
  if (!snapshot_.data_source_version_id.empty() && !snapshot_.time_viewport.empty()) {
    recent_.insert_or_assign(snapshot_.data_source_version_id, snapshot_.time_viewport);
  }
}

const ViewportSnapshot& ViewportController::snapshot() const noexcept {
  return snapshot_;
}

std::string ViewportController::partial_read_summary() const {
  if (!snapshot_.partial_read) {
    return "已读取完整数据范围";
  }
  std::ostringstream output;
  output << "部分读取：样本 [" << snapshot_.loaded_range.begin() << ',' << snapshot_.loaded_range.end() << ")，已读 "
         << snapshot_.loaded_bytes << " / " << snapshot_.physical_bytes << " 字节；未读尾部不可导航";
  return output.str();
}

task::ViewRequestId ViewportController::issue_request() {
  ++generation_;
  return {scope_, generation_};
}

core::Status AtomicFrameCoordinator::begin(ViewportSnapshot expected) {
  if (expected.request_id.scope.empty() || expected.request_id.generation == 0 || expected.loaded_range.empty() ||
      expected.time_viewport.empty() || !expected.loaded_range.contains(expected.time_viewport) ||
      !expected.effective_frequency_range.contains(expected.frequency_viewport)) {
    return failure(core::ErrorReason::invalid_argument, "视图请求快照不完整");
  }
  expected_ = std::move(expected);
  frame_.reset();
  return core::Status::success();
}

core::Status AtomicFrameCoordinator::commit(VisualizationFrame frame) {
  if (!expected_) {
    return failure(core::ErrorReason::invalid_argument, "提交前必须开始视图请求");
  }
  if (frame.request_id != expected_->request_id) {
    return failure(core::ErrorReason::cancelled, "过期 ViewRequestId 不得提交");
  }
  if (frame.time_range != expected_->time_viewport || frame.frequency_range != expected_->frequency_viewport ||
      frame.data_source_version_id != expected_->data_source_version_id) {
    return failure(core::ErrorReason::invalid_argument, "时域、PSD 与 STFT 必须原子提交同一时间和频率视窗");
  }
  if (frame.time_primary.empty() || frame.psd_db_hz.empty() || frame.stft_rows == 0 || frame.stft_columns == 0 ||
      (!frame.time_secondary.empty() && frame.time_secondary.size() != frame.time_primary.size()) ||
      frame.psd_metadata.effective_samples != frame.time_range.size() ||
      frame.stft_db.size() != static_cast<std::size_t>(frame.stft_rows) * frame.stft_columns) {
    return failure(core::ErrorReason::invalid_argument, "三图帧数据不完整");
  }
  frame_ = std::move(frame);
  return core::Status::success();
}

std::optional<VisualizationFrame> AtomicFrameCoordinator::frame() const {
  return frame_;
}

std::optional<ViewportSnapshot> AtomicFrameCoordinator::expected() const {
  return expected_;
}

core::Status LayerModel::upsert(Layer layer) {
  if (!valid_token(layer.id) || !valid_token(layer.label) || !valid_token(layer.source) ||
      !std::isfinite(layer.opacity) || layer.opacity < 0.0 || layer.opacity > 1.0) {
    return failure(core::ErrorReason::invalid_argument, "图层字段或透明度无效");
  }
  layers_.insert_or_assign(layer.id, std::move(layer));
  return core::Status::success();
}

core::Status LayerModel::remove(std::string_view id) {
  if (layers_.erase(std::string(id)) == 0) {
    return failure(core::ErrorReason::invalid_argument, "图层不存在");
  }
  return core::Status::success();
}

std::vector<Layer> LayerModel::ordered() const {
  std::vector<Layer> result;
  result.reserve(layers_.size());
  for (const auto& [id, layer] : layers_) {
    static_cast<void>(id);
    result.push_back(layer);
  }
  std::ranges::sort(result, [](const Layer& left, const Layer& right) {
    if (left.order != right.order) {
      return left.order < right.order;
    }
    return left.id < right.id;
  });
  return result;
}

core::Result<std::string> LayerModel::serialize() const {
  std::ostringstream output;
  for (const auto& layer : ordered()) {
    output << layer.id << '\t' << layer.label << '\t' << layer.source << '\t' << (layer.visible ? 1 : 0) << '\t'
           << layer.opacity << '\t' << layer.order << '\n';
  }
  return output.str();
}

core::Status LayerModel::restore(std::string_view serialized) {
  std::istringstream input{std::string(serialized)};
  LayerModel restored;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream row(line);
    Layer layer;
    std::string visible;
    std::string opacity;
    std::string order;
    if (!std::getline(row, layer.id, '\t') || !std::getline(row, layer.label, '\t') ||
        !std::getline(row, layer.source, '\t') || !std::getline(row, visible, '\t') ||
        !std::getline(row, opacity, '\t') || !std::getline(row, order, '\t')) {
      return failure(core::ErrorReason::invalid_argument, "图层持久化内容损坏");
    }
    try {
      layer.visible = std::stoi(visible) != 0;
      layer.opacity = std::stod(opacity);
      layer.order = std::stoi(order);
    } catch (const std::exception&) {
      return failure(core::ErrorReason::invalid_argument, "图层持久化数值无效");
    }
    const auto status = restored.upsert(std::move(layer));
    if (!status) {
      return status;
    }
  }
  layers_ = std::move(restored.layers_);
  return core::Status::success();
}

core::Status validate_display_mapping(const DisplayMapping& mapping) {
  static constexpr std::array<std::string_view, 5U> supported_color_maps{"Industrial", "Viridis", "Turbo", "Inferno",
                                                                         "Grayscale"};
  if (!std::isfinite(mapping.minimum) || !std::isfinite(mapping.maximum) || !std::isfinite(mapping.reference_level) ||
      !std::isfinite(mapping.dynamic_range) || mapping.minimum >= mapping.maximum || mapping.dynamic_range <= 0.0 ||
      std::ranges::find(supported_color_maps, mapping.color_map) == supported_color_maps.end()) {
    return failure(core::ErrorReason::invalid_argument, "显示映射范围或配色无效");
  }
  return core::Status::success();
}

OverlayModel::OverlayModel(data::SampleRange loaded_range, FrequencyRange effective_frequency_range)
    : loaded_range_(loaded_range), effective_frequency_range_(effective_frequency_range) {}

core::Result<Selection> OverlayModel::create(Selection selection) {
  auto status = validate_selection(selection);
  if (!status) {
    return status;
  }
  if (selection.id.empty()) {
    selection.id = "SEL-" + std::to_string(next_selection_id_++);
  }
  if (selection.name.empty()) {
    selection.name = "选区 " + selection.id;
  }
  if (selections_.contains(selection.id)) {
    return failure(core::ErrorReason::invalid_argument, "Selection ID 必须稳定且唯一");
  }
  selections_.emplace(selection.id, selection);
  return selection;
}

core::Result<Selection> OverlayModel::copy(std::string_view id) {
  const auto source = find(id);
  if (!source) {
    return failure(core::ErrorReason::invalid_argument, "待复制 Selection 不存在");
  }
  auto copied = *source;
  copied.id.clear();
  copied.name += " 副本";
  copied.dependent_results = 0;
  return create(std::move(copied));
}

core::Status OverlayModel::update(Selection selection) {
  const auto existing = selections_.find(selection.id);
  if (existing == selections_.end()) {
    return failure(core::ErrorReason::invalid_argument, "Selection 不存在");
  }
  if (existing->second.locked && (selection.time_range != existing->second.time_range ||
                                  selection.frequency_range != existing->second.frequency_range)) {
    return failure(core::ErrorReason::invalid_argument, "锁定 Selection 不允许修改边界");
  }
  auto status = validate_selection(selection);
  if (!status) {
    return status;
  }
  selections_.insert_or_assign(selection.id, std::move(selection));
  return core::Status::success();
}

core::Status OverlayModel::remove(std::string_view id) {
  const auto existing = selections_.find(id);
  if (existing == selections_.end()) {
    return failure(core::ErrorReason::invalid_argument, "Selection 不存在");
  }
  if (existing->second.dependent_results != 0) {
    return failure(core::ErrorReason::invalid_argument, "Selection 存在依赖结果，必须先解除依赖");
  }
  selections_.erase(existing);
  return core::Status::success();
}

std::optional<Selection> OverlayModel::find(std::string_view id) const {
  const auto existing = selections_.find(id);
  if (existing == selections_.end()) {
    return std::nullopt;
  }
  return existing->second;
}

std::vector<Selection> OverlayModel::selections() const {
  std::vector<Selection> result;
  result.reserve(selections_.size());
  for (const auto& [id, selection] : selections_) {
    static_cast<void>(id);
    result.push_back(selection);
  }
  return result;
}

core::Result<Measurement> OverlayModel::add_measurement(Measurement measurement) {
  const auto selection = find(measurement.selection_id);
  if (!selection || measurement.data_source_version_id.empty() || measurement.algorithm.empty() ||
      measurement.unit.empty() || measurement.timestamp_utc.empty() || !std::isfinite(measurement.value) ||
      measurement.time_range != selection->time_range || measurement.frequency_range != selection->frequency_range) {
    return failure(core::ErrorReason::invalid_argument, "测量必须完整记录 Selection、来源、算法、单位和时间戳");
  }
  if (measurement.id.empty()) {
    measurement.id = "MEAS-" + std::to_string(next_measurement_id_++);
  } else if (std::ranges::any_of(measurements_, [&measurement](const Measurement& existing) {
               return existing.id == measurement.id;
             })) {
    return failure(core::ErrorReason::invalid_argument, "Measurement ID 必须稳定且唯一");
  }
  measurements_.push_back(measurement);
  auto updated = *selection;
  ++updated.dependent_results;
  selections_.insert_or_assign(updated.id, std::move(updated));
  return measurement;
}

std::vector<Measurement> OverlayModel::measurements() const {
  return measurements_;
}

core::Result<ChannelEstimate> OverlayModel::estimate_channel(std::string_view selection_id,
                                                             std::uint64_t output_sample_rate_hz,
                                                             bool complex_output) const {
  const auto selection = find(selection_id);
  if (!selection || !selection->frequency_range || output_sample_rate_hz == 0) {
    return failure(core::ErrorReason::invalid_argument, "创建通道需要频率 Selection 和非零输出采样率");
  }
  const auto midpoint = selection->frequency_range->begin_hz + selection->frequency_range->bandwidth_hz() / 2;
  const auto bytes_per_frame = complex_output ? 8ULL : 4ULL;
  const auto duration_samples = selection->time_range.size();
  if (duration_samples > std::numeric_limits<std::uint64_t>::max() / bytes_per_frame) {
    return failure(core::ErrorReason::invalid_argument, "预计数据量溢出");
  }
  return ChannelEstimate{midpoint, static_cast<std::uint64_t>(selection->frequency_range->bandwidth_hz()),
                         output_sample_rate_hz, complex_output, duration_samples * bytes_per_frame};
}

void OverlayModel::mark_dependencies_stale(std::string_view data_source_version_id) {
  for (auto& [id, selection] : selections_) {
    static_cast<void>(id);
    selection.stale = true;
  }
  for (auto& measurement : measurements_) {
    if (measurement.data_source_version_id == data_source_version_id) {
      measurement.stale = true;
    }
  }
}

core::Status OverlayModel::validate_selection(const Selection& selection) const {
  if (selection.time_range.empty() || !loaded_range_.contains(selection.time_range)) {
    return failure(core::ErrorReason::invalid_argument, "Selection 时间边界必须位于 LoadedDataRange");
  }
  if ((selection.kind == SelectionKind::frequency || selection.kind == SelectionKind::time_frequency) &&
      (!selection.frequency_range ||
       !make_frequency_range(selection.frequency_range->begin_hz, selection.frequency_range->end_hz) ||
       !effective_frequency_range_.contains(*selection.frequency_range))) {
    return failure(core::ErrorReason::invalid_argument, "频率 Selection 必须位于有效频率范围");
  }
  if (selection.kind == SelectionKind::time && selection.frequency_range) {
    return failure(core::ErrorReason::invalid_argument, "纯时间 Selection 不应携带频率范围");
  }
  return core::Status::success();
}

std::vector<BatchItemResult> run_batch(const std::vector<Selection>& selections,
                                       const std::function<core::Status(const Selection&)>& operation) {
  std::vector<BatchItemResult> result;
  result.reserve(selections.size());
  for (const auto& selection : selections) {
    if (!operation) {
      result.push_back({selection.id, false, "批量操作未配置"});
      continue;
    }
    const auto status = operation(selection);
    result.push_back({selection.id, status.ok(), status.ok() ? "完成" : std::string(status.message())});
  }
  return result;
}

std::uint64_t PrefetchController::begin(std::uint64_t previous_samples, std::uint64_t following_samples) {
  ++generation_;
  active_ = previous_samples != 0 || following_samples != 0;
  return generation_;
}

void PrefetchController::cancel_for_interaction() noexcept {
  ++generation_;
  active_ = false;
}

void PrefetchController::cancel_for_resource_pressure() noexcept {
  ++generation_;
  active_ = false;
}

bool PrefetchController::active() const noexcept {
  return active_;
}

std::uint64_t PrefetchController::generation() const noexcept {
  return generation_;
}

VisibilityController::VisibilityController() {
  for (std::uint32_t value = static_cast<std::uint32_t>(ChartKind::time_waveform);
       value <= static_cast<std::uint32_t>(ChartKind::eye_diagram); ++value) {
    states_.emplace(static_cast<ChartKind>(value), ChartActivity{});
  }
}

core::Status VisibilityController::set_visible(ChartKind chart, bool visible) {
  const auto existing = states_.find(chart);
  if (existing == states_.end()) {
    return failure(core::ErrorReason::invalid_argument, "未知图表类型");
  }
  existing->second.visible = visible;
  existing->second.observer_connected = visible;
  if (!visible) {
    existing->second.pending_preparations = 0;
  }
  return core::Status::success();
}

core::Status VisibilityController::note_preparation(ChartKind chart) {
  const auto existing = states_.find(chart);
  if (existing == states_.end() || !existing->second.visible) {
    return failure(core::ErrorReason::cancelled, "隐藏图表不得提交专属准备任务");
  }
  ++existing->second.pending_preparations;
  return core::Status::success();
}

core::Status VisibilityController::note_paint(ChartKind chart) {
  const auto existing = states_.find(chart);
  if (existing == states_.end() || !existing->second.visible) {
    return failure(core::ErrorReason::cancelled, "隐藏图表不得继续绘制");
  }
  ++existing->second.paint_epoch;
  if (existing->second.pending_preparations != 0) {
    --existing->second.pending_preparations;
  }
  return core::Status::success();
}

ChartActivity VisibilityController::state(ChartKind chart) const {
  const auto existing = states_.find(chart);
  return existing == states_.end() ? ChartActivity{} : existing->second;
}

core::Status ChartLayoutModel::set(std::vector<ChartLayoutItem> items) {
  if (items.empty()) {
    return failure(core::ErrorReason::invalid_argument, "图表布局不能为空");
  }
  std::map<ChartKind, bool> unique;
  for (const auto& item : items) {
    if (item.logical_height < 96 || unique.contains(item.chart)) {
      return failure(core::ErrorReason::invalid_argument, "每个可见图必须唯一且保持至少 96 逻辑像素");
    }
    unique.emplace(item.chart, true);
  }
  items_ = std::move(items);
  return core::Status::success();
}

const std::vector<ChartLayoutItem>& ChartLayoutModel::items() const noexcept {
  return items_;
}

core::Result<std::string> ChartLayoutModel::serialize() const {
  std::ostringstream output;
  for (const auto& item : items_) {
    output << static_cast<std::uint32_t>(item.chart) << ':' << item.logical_height << '\n';
  }
  return output.str();
}

core::Status ChartLayoutModel::restore(std::string_view serialized) {
  std::istringstream input{std::string(serialized)};
  std::vector<ChartLayoutItem> restored;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    const auto separator = line.find(':');
    if (separator == std::string::npos) {
      return failure(core::ErrorReason::invalid_argument, "图表布局内容损坏");
    }
    const auto chart = parse_chart_kind(std::string_view(line).substr(0, separator));
    std::uint32_t height{};
    const auto height_text = std::string_view(line).substr(separator + 1);
    const auto parsed = std::from_chars(height_text.data(), height_text.data() + height_text.size(), height);
    if (!chart || parsed.ec != std::errc{} || parsed.ptr != height_text.data() + height_text.size()) {
      return failure(core::ErrorReason::invalid_argument, "图表布局数值无效");
    }
    restored.push_back({chart.value(), height});
  }
  return set(std::move(restored));
}

std::vector<ComponentDescriptor> component_catalog() {
  return {
      {"TimeWaveformView", "实信号及 I/Q、幅度、相位时域视图", true},
      {"TimeNavigator", "LoadedDataRange 内的时间视窗导航", true},
      {"SpectrumView", "单边、镜像双边或 fftshift 频谱", true},
      {"PSDView", "当前时间视窗 PSD 与统计摘要", true},
      {"WaterfallView", "共享频率视口的瀑布图", true},
      {"SpectrogramView", "横轴频率、纵轴向下递增时间的时频图", true},
      {"ConstellationView", "可复用星座图", true},
      {"EyeDiagramView", "可复用眼图", true},
      {"FrequencyAxis", "64 位整数 Hz 频率轴", true},
      {"TimeAxis", "64 位样本索引时间轴", true},
      {"FrequencyViewport", "PSD 与 STFT 共享频率视口", true},
      {"TimeViewport", "同一 ViewRequestId 的时间视口", true},
      {"MeasurementOverlay", "游标和来源可追溯测量", true},
      {"SelectionOverlay", "时间、频率和时频 Selection", true},
      {"LayerModel", "可见性、透明度、顺序和来源", true},
      {"ChartScreenshot", "可选择组成项的图谱截图", true},
  };
}

} // namespace signal::visualization
