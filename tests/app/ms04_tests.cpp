#include "application.hpp"

#include "signal_studio/core/artifact.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/workbench/inspector.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using signal::core::ArtifactDescriptor;
using signal::core::ArtifactFormat;
using signal::core::ArtifactKind;
using signal::core::ArtifactProvenance;
using signal::core::ArtifactRecord;

template <typename T> void require(T&& condition, std::string_view message) {
  if (!static_cast<bool>(condition)) {
    throw std::runtime_error(std::string{message});
  }
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("signal-studio-ms04-" + std::to_string(stamp));
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::byte> bytes(std::string_view value) {
  std::vector<std::byte> result(value.size());
  if (!value.empty()) {
    std::memcpy(result.data(), value.data(), value.size());
  }
  return result;
}

[[nodiscard]] std::string json_string(std::string_view value) {
  std::string result{"\""};
  for (const auto character : value) {
    switch (character) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      result.push_back(character);
      break;
    }
  }
  result.push_back('"');
  return result;
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view value, char delimiter) {
  std::vector<std::string_view> result;
  while (true) {
    const auto position = value.find(delimiter);
    result.push_back(value.substr(0U, position));
    if (position == std::string_view::npos) {
      return result;
    }
    value.remove_prefix(position + 1U);
  }
}

[[nodiscard]] std::filesystem::path write_sc16(const std::filesystem::path& path, std::size_t samples = 16'384U) {
  std::vector<std::byte> payload(samples * 4U);
  for (std::size_t index = 0; index < samples; ++index) {
    const auto i = static_cast<std::int16_t>(std::llround(std::sin(static_cast<double>(index) * 0.05) * 20'000.0));
    const auto q = static_cast<std::int16_t>(std::llround(std::cos(static_cast<double>(index) * 0.05) * 20'000.0));
    std::memcpy(payload.data() + index * 4U, &i, sizeof(i));
    std::memcpy(payload.data() + index * 4U + 2U, &q, sizeof(q));
  }
  require(signal::core::AtomicFileStore{}.write(path, payload), "SC16 fixture write failed");
  return path;
}

[[nodiscard]] signal::data::SignalDescriptor complex_descriptor() {
  signal::data::SignalDescriptor descriptor;
  descriptor.signal_kind = signal::data::SignalKind::complex;
  descriptor.scalar_type = signal::data::ScalarType::int16;
  descriptor.component_layout = signal::data::ComponentLayout::interleaved;
  descriptor.component_order = signal::data::ComponentOrder::iq;
  descriptor.endianness = signal::data::Endianness::little;
  descriptor.sample_rate_hz = 1.0e6;
  descriptor.center_frequency_hz = 10.0e6;
  descriptor.scale_factor = 1.0 / 32768.0;
  descriptor.amplitude_mode = "int16_scaled";
  descriptor.requested_sample_range = signal::data::SampleRange::from_count(0U, 16'384U).value();
  descriptor.provenance = {{"source", {signal::data::FieldOrigin::user, true}},
                           {"sample_rate_hz", {signal::data::FieldOrigin::user, true}},
                           {"center_frequency_hz", {signal::data::FieldOrigin::user, true}}};
  return descriptor;
}

[[nodiscard]] signal::studio::ImportedSignal import_fixture(signal::studio::ApplicationController& controller,
                                                            const std::filesystem::path& directory,
                                                            std::size_t samples = 8'192U) {
  const auto source = write_sc16(directory / "ms45_cf10MHz_sr1MSps.sc16", samples);
  auto confirmed =
      signal::studio::make_confirmed_descriptor(source, signal::studio::parse_filename_hints(source), true);
  require(confirmed, "MS-4.5 fixture descriptor failed");
  auto task =
      controller.start_import({source, confirmed.value(), signal::data::SourceFormat::raw, samples * 4U, 16U * 1024U});
  require(task, "MS-4.5 fixture import submission failed");
  auto imported = controller.finalize_import(task.value());
  require(imported && imported.value().loaded->samples().size() == samples, "MS-4.5 fixture import failed");
  return std::move(imported).value();
}

[[nodiscard]] signal::dsp::AnalysisSettingsSnapshot
compact_analysis_settings(const signal::studio::ApplicationController& controller) {
  auto settings = controller.analysis_settings();
  settings.spectrum.analysis_range_policy = signal::dsp::AnalysisRangePolicy::first_frame;
  settings.spectrum.frame_length = 1024U;
  settings.spectrum.fft_length = 1024U;
  settings.spectrum.zero_padding_policy = signal::dsp::ZeroPaddingPolicy::forbidden;
  settings.spectrum.estimator = {signal::dsp::PsdEstimatorKind::periodogram, 0.0, 1U};
  settings.spectrum.accumulation = {};
  settings.spectrum.smoothing = {};
  settings.spectrogram.frame_length = 256U;
  settings.spectrogram.fft_length = 256U;
  settings.spectrogram.hop_length = 128U;
  settings.spectrogram.padding_policy = signal::dsp::ZeroPaddingPolicy::forbidden;
  settings.spectrogram.smoothing = {};
  return settings;
}

[[nodiscard]] double maximum(std::span<const double> values) {
  require(!values.empty(), "Expected non-empty numerical result");
  return *std::ranges::max_element(values);
}

[[nodiscard]] ArtifactProvenance provenance(std::string version = "source-v1") {
  return {"project-1", std::move(version), "SEL-1", "CH-01",        "channel-v1",
          "task-1",    "signal.test",      "1.0.0", "parameters-v1"};
}

[[nodiscard]] ArtifactDescriptor descriptor(std::string id, ArtifactFormat format,
                                            ArtifactKind kind = ArtifactKind::measurement) {
  ArtifactDescriptor value;
  value.id = std::move(id);
  value.kind = kind;
  value.format = format;
  value.provenance = provenance();
  value.units = {{"power", "dB/Hz"}};
  return value;
}

[[nodiscard]] std::vector<std::byte> valid_json(const ArtifactProvenance& source) {
  auto payload =
      signal::core::make_artifact_json("signal.measurement/1.0", source, {{"power", "dB/Hz"}}, R"({"peak":-42.5})");
  require(payload, "JSON payload creation failed");
  return payload.value();
}

[[nodiscard]] ArtifactRecord commit_json(signal::core::ArtifactStore& store, std::string id = "result-json") {
  auto description = descriptor(std::move(id), ArtifactFormat::json);
  auto record = store.commit(description, valid_json(description.provenance));
  require(record, "JSON artifact commit failed");
  return record.value();
}

void test_inspector_views() {
  auto state =
      signal::workbench::make_inspector_channel_state("CH-01", "channel-v1", "source-v1", complex_descriptor());
  require(state && state.value().views.size() == 8U, "Inspector view catalog incomplete");
  require(std::ranges::all_of(state.value().views,
                              [](const auto& view) {
                                return view.applicable
                                           ? !view.range.empty() && !view.unit.empty() && !view.preprocessing.empty()
                                           : !view.reason.empty();
                              }),
          "Inspector applicability does not expose range/unit/preprocessing or reason");
}

void test_constellation() {
  auto state =
      signal::workbench::make_inspector_channel_state("CH-01", "channel-v1", "source-v1", complex_descriptor());
  require(state && state.value().constellation.maximum_points == 100'000U && state.value().constellation.reference_grid,
          "Constellation defaults are not bounded and explicit");
  auto real = complex_descriptor();
  real.signal_kind = signal::data::SignalKind::real;
  real.component_layout = signal::data::ComponentLayout::real;
  real.component_order = signal::data::ComponentOrder::not_applicable;
  real.requested_sample_range = signal::data::SampleRange::from_count(0U, 32'768U).value();
  auto real_state = signal::workbench::make_inspector_channel_state("R", "v1", "source-v1", real);
  require(real_state, "Real Inspector construction failed");
  const auto constellation = std::ranges::find_if(real_state.value().views, [](const auto& view) {
    return view.kind == signal::workbench::InspectorViewKind::constellation;
  });
  require(constellation != real_state.value().views.end() && !constellation->applicable,
          "Real signal constellation was not explicitly rejected");
}

void test_eye_rules() {
  auto unavailable = signal::workbench::make_inspector_channel_state("CH-01", "v1", "source-v1", complex_descriptor());
  require(unavailable, "Inspector construction failed");
  const auto eye = std::ranges::find_if(unavailable.value().views, [](const auto& view) {
    return view.kind == signal::workbench::InspectorViewKind::eye_diagram;
  });
  require(eye != unavailable.value().views.end() && !eye->applicable && eye->reason.find("符号率") != std::string::npos,
          "Eye diagram did not reject missing timing evidence");
  auto available = signal::workbench::make_inspector_channel_state("CH-01", "v1", "source-v1", complex_descriptor(),
                                                                   100'000.0, "clock-recovery-v1");
  require(available, "Explicit eye diagram configuration failed");
  const auto configured_eye = std::ranges::find_if(available.value().views, [](const auto& view) {
    return view.kind == signal::workbench::InspectorViewKind::eye_diagram;
  });
  require(configured_eye != available.value().views.end() && configured_eye->applicable,
          "Eye diagram rejected explicit symbol rate and synchronization source");
}

void test_histograms() {
  auto state = signal::workbench::make_inspector_channel_state("CH-01", "v1", "source-v1", complex_descriptor());
  require(state, "Inspector construction failed");
  const auto amplitude = std::ranges::find_if(state.value().views, [](const auto& view) {
    return view.kind == signal::workbench::InspectorViewKind::amplitude_histogram;
  });
  const auto phase = std::ranges::find_if(state.value().views, [](const auto& view) {
    return view.kind == signal::workbench::InspectorViewKind::phase_histogram;
  });
  require(amplitude->applicable && phase->applicable && phase->unit == "rad",
          "Amplitude/phase histogram contract incomplete");
}

void test_instantaneous_frequency() {
  auto state = signal::workbench::make_inspector_channel_state("CH-01", "v1", "source-v1", complex_descriptor());
  require(state, "Inspector construction failed");
  const auto view = std::ranges::find_if(state.value().views, [](const auto& item) {
    return item.kind == signal::workbench::InspectorViewKind::instantaneous_frequency;
  });
  require(view != state.value().views.end() && view->applicable && view->unit == "Hz" &&
              view->preprocessing.find("相位") != std::string::npos,
          "Instantaneous-frequency contract lacks unit/preprocessing");
}

void test_inspector_state_versions() {
  auto state =
      signal::workbench::make_inspector_channel_state("CH-01", "channel-v1", "source-v1", complex_descriptor());
  require(state, "Inspector construction failed");
  state.value().results.push_back({"R-1", "channel-v1", "parameters-v1"});
  require(
      signal::workbench::inspector_result_is_current(state.value().results.front(), "channel-v1", "parameters-v1") &&
          !signal::workbench::inspector_result_is_current(state.value().results.front(), "channel-v2", "parameters-v1"),
      "Inspector stale-result version check failed");
  signal::workbench::InspectorStateStore store;
  require(store.upsert(state.value()), "Inspector state upsert failed");
  auto serialized = store.serialize();
  require(serialized, "Inspector state serialization failed");
  auto parsed = signal::workbench::InspectorStateStore::parse(serialized.value());
  require(parsed && parsed.value().find("CH-01") == state.value(), "Inspector state round-trip failed");
}

void test_layout_degradation() {
  signal::workbench::InspectorLayoutTemplate layout{
      "communications",
      {signal::workbench::InspectorViewKind::constellation, signal::workbench::InspectorViewKind::eye_diagram},
      {"clock-recovery", "demodulator"}};
  const std::array<std::string, 1> plugins{"clock-recovery"};
  auto restored = signal::workbench::restore_inspector_layout(layout, plugins);
  require(restored && restored.value().degraded &&
              restored.value().missing_plugins == std::vector<std::string>{"demodulator"},
          "Inspector layout did not degrade explicitly for missing plugin");
}

void test_csv_json() {
  const auto source = provenance();
  const std::array<std::string, 2> columns{"frequency_hz", "power_db_hz"};
  const std::array<std::vector<std::string>, 1> rows{std::vector<std::string>{"1000000", "-42.5"}};
  auto csv = signal::core::make_artifact_csv("signal.spectrum/1.0", source, {{"power", "dB/Hz"}}, columns, rows);
  auto json = signal::core::make_artifact_json("signal.spectrum/1.0", source, {{"power", "dB/Hz"}},
                                               R"({"frequencyHz":1000000,"power":-42.5})");
  require(csv && json, "CSV/JSON encoding failed");
  require(std::string{reinterpret_cast<const char*>(csv.value().data()), csv.value().size()}.find(
              "# provenance.dataSourceVersionId") != std::string::npos,
          "CSV provenance header missing");
  require(std::string{reinterpret_cast<const char*>(json.value().data()), json.value().size()}.find("\"units\"") !=
              std::string::npos,
          "JSON units missing");
}

void test_provenance() {
  TemporaryDirectory temporary;
  signal::core::ArtifactStore store{temporary.path() / "artifacts"};
  const auto record = commit_json(store);
  require(record.descriptor.provenance.project_id == "project-1" &&
              record.descriptor.provenance.data_source_version_id == "source-v1" &&
              record.descriptor.provenance.channel_version == "channel-v1" &&
              record.descriptor.provenance.parameter_version == "parameters-v1",
          "Artifact provenance was not retained");
  require(store.verify(record), "Committed artifact integrity failed");
}

void test_png() {
  TemporaryDirectory temporary;
  signal::core::ArtifactStore store{temporary.path() / "artifacts"};
  auto description = descriptor("plot-png", ArtifactFormat::png, ArtifactKind::spectrum);
  const std::array<std::byte, 12> png{std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47},
                                      std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A},
                                      std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0}};
  require(store.commit(description, png), "PNG artifact signature was rejected");
  auto invalid = description;
  invalid.id = "bad-png";
  require(!store.commit(invalid, bytes("not png")), "Invalid PNG was accepted");
}

void test_raw() {
  TemporaryDirectory temporary;
  signal::core::ArtifactStore store{temporary.path() / "artifacts"};
  auto description = descriptor("samples-raw", ArtifactFormat::raw, ArtifactKind::sampled_data);
  auto sidecar = signal::data::serialize_sidecar(complex_descriptor());
  require(sidecar, "RAW sidecar encoding failed");
  description.metadata["signalDescriptor"] = sidecar.value();
  require(store.commit(description, bytes("raw-payload")), "Re-importable RAW artifact failed");
  auto invalid = descriptor("raw-without-sidecar", ArtifactFormat::raw, ArtifactKind::sampled_data);
  require(!store.commit(invalid, bytes("raw-payload")), "RAW artifact without SignalDescriptor was accepted");
}

void test_wav() {
  TemporaryDirectory temporary;
  signal::core::ArtifactStore store{temporary.path() / "artifacts"};
  auto description = descriptor("audio-wav", ArtifactFormat::wav, ArtifactKind::audio);
  description.metadata = {{"signalDescriptor", "{}"},
                          {"bitDepth", "16"},
                          {"sampleRateHz", "1000000"},
                          {"normalizationPolicy", "保持确认比例"}};
  std::array<std::byte, 12> wav{};
  std::memcpy(wav.data(), "RIFF", 4U);
  std::memcpy(wav.data() + 8U, "WAVE", 4U);
  require(store.commit(description, wav), "WAV artifact metadata/signature failed");
}

void test_plugin_format() {
  TemporaryDirectory temporary;
  signal::core::ArtifactStore store{temporary.path() / "artifacts"};
  auto description = descriptor("plugin-result", ArtifactFormat::plugin_defined, ArtifactKind::plugin_defined);
  description.plugin_format = "vendor.signal-result/2.0";
  require(store.commit(description, bytes("plugin-payload")), "Plugin-defined artifact failed");
}

void test_atomic_no_overwrite() {
  TemporaryDirectory temporary;
  signal::core::ArtifactStore store{temporary.path() / "artifacts"};
  auto record = commit_json(store);
  auto duplicate = store.commit(record.descriptor, valid_json(record.descriptor.provenance));
  require(!duplicate, "Artifact store silently overwrote an existing ID");
  require(store.verify(record), "Original artifact changed after duplicate commit");
  std::filesystem::create_directories(store.root() / ".staging-orphan");
  require(store.recover() && !std::filesystem::exists(store.root() / ".staging-orphan"),
          "Artifact staging recovery failed");
}

void test_batch_export() {
  TemporaryDirectory temporary;
  signal::core::ArtifactStore store{temporary.path() / "artifacts"};
  std::array<ArtifactRecord, 2> records{commit_json(store, "result-a"), commit_json(store, "result-b")};
  auto exported =
      store.export_batch(records, {"measurement-batch", {ArtifactFormat::json}, true}, temporary.path() / "batch");
  require(exported && std::filesystem::exists(exported.value() / "batch-manifest.json") &&
              std::filesystem::exists(exported.value() / "result-a" / "manifest.json"),
          "Batch export manifest/package incomplete");
}

void test_filter_and_current() {
  TemporaryDirectory temporary;
  signal::core::ArtifactStore store{temporary.path() / "artifacts"};
  const auto current = commit_json(store, "current");
  auto old_description = descriptor("old", ArtifactFormat::json);
  old_description.provenance = provenance("source-v0");
  auto old = store.commit(old_description, valid_json(old_description.provenance));
  require(old, "Old-version artifact commit failed");
  signal::core::ArtifactFilter filter;
  filter.data_source_version_id = "source-v1";
  auto queried = store.query(filter);
  require(queried && queried.value().size() == 1U &&
              signal::core::artifact_is_current(current, "source-v1", "channel-v1", "parameters-v1") &&
              !signal::core::artifact_is_current(old.value(), "source-v1", "channel-v1", "parameters-v1"),
          "Artifact filters/current-stale distinction failed");
}

void test_application_flow() {
  TemporaryDirectory temporary;
  const auto source = write_sc16(temporary.path() / "capture_cf10MHz_sr1MSps.sc16");
  auto before = signal::core::hash_file(source);
  require(before, "Source hash failed");
  signal::studio::ApplicationController controller{temporary.path() / "state"};
  const auto project = temporary.path() / "flow.signal-workspace";
  require(controller.create_project(project, "flow"), "Project creation failed");
  const auto hints = signal::studio::parse_filename_hints(source);
  require(hints.sample_rate_hz == 1.0e6 && hints.center_frequency_hz == 10.0e6, "Filename hint parsing failed");
  require(!signal::studio::make_confirmed_descriptor(source, hints, false), "Unconfirmed filename hints were accepted");
  auto confirmed = signal::studio::make_confirmed_descriptor(source, hints, true);
  require(confirmed, "Confirmed descriptor construction failed");
  auto import =
      controller.start_import({source, confirmed.value(), signal::data::SourceFormat::raw, 64U * 1024U, 4U * 1024U});
  require(import, "Import task submission failed");
  auto imported = controller.finalize_import(import.value());
  require(imported && imported.value().loaded->samples().size() == 16'384U,
          "Bounded import did not publish expected samples");
  auto analysis = controller.analyze(imported.value());
  require(analysis && !analysis.value().frame.psd_db_hz.empty() && !analysis.value().frame.stft_db.empty(),
          "Real PSD/STFT analysis failed");
  auto result = controller.commit_measurement(analysis.value(), "SEL-1", "CH-01");
  require(result && controller.results().value().size() == 1U, "Measurement artifact closure failed");
  require(controller.save_project() && controller.close_project() && controller.open_project(project),
          "Project save/close/reopen failed");
  auto after = signal::core::hash_file(source);
  require(after && after.value() == before.value(), "Import modified source recording");
}

void test_ms45_parameter_effects() {
  TemporaryDirectory temporary;
  signal::studio::ApplicationController controller{temporary.path() / "state"};
  require(controller.create_project(temporary.path() / "parameters.signal-workspace", "ms45-parameters"),
          "MS-4.5 parameter project creation failed");
  const auto imported = import_fixture(controller, temporary.path());
  auto baseline_settings = compact_analysis_settings(controller);
  const auto baseline_request = controller.task_runtime().issue_view_request("ms45-parameter-effects");
  auto baseline = controller.analyze(imported, baseline_settings, false, nullptr, baseline_request, "task-baseline");
  require(baseline, "MS-4.5 baseline analysis failed");

  auto changed_settings = baseline_settings;
  changed_settings.spectrum.fft_length = 2048U;
  changed_settings.spectrum.zero_padding_policy = signal::dsp::ZeroPaddingPolicy::enabled;
  changed_settings.spectrum.window = {signal::dsp::WindowKind::blackman_harris, 0.0};
  changed_settings.spectrogram.fft_length = 512U;
  changed_settings.spectrogram.padding_policy = signal::dsp::ZeroPaddingPolicy::enabled;
  changed_settings.spectrogram.window = {signal::dsp::WindowKind::hamming, 0.0};
  const auto changed_request = controller.task_runtime().issue_view_request("ms45-parameter-effects");
  auto changed = controller.analyze(imported, changed_settings, false, nullptr, changed_request, "task-changed");
  require(changed, "MS-4.5 changed-parameter analysis failed");

  require(baseline.value().spectrum.fft_length == 1024U && changed.value().spectrum.fft_length == 2048U &&
              baseline.value().spectrum.values.size() == 1024U && changed.value().spectrum.values.size() == 2048U,
          "FFT length did not change the spectrum bin count");
  require(std::abs(baseline.value().spectrum.bin_spacing_hz - 976.5625) < 1.0e-9 &&
              std::abs(changed.value().spectrum.bin_spacing_hz - 488.28125) < 1.0e-9,
          "FFT length did not change the frequency-bin spacing");
  require(baseline.value().stft.columns == 256U && changed.value().stft.columns == 512U &&
              baseline.value().stft.db_per_hz.size() != changed.value().stft.db_per_hz.size(),
          "STFT FFT parameter did not change the time-frequency result dimensions");
  require(std::abs(maximum(baseline.value().psd.db_per_hz) - maximum(changed.value().psd.db_per_hz)) > 1.0e-4,
          "Window and FFT parameters did not change the computed PSD values");
  require(baseline.value().settings_hash != changed.value().settings_hash &&
              baseline.value().cache_key != changed.value().cache_key,
          "Numerically distinct parameter snapshots produced identical identities");
  const auto presets = controller.built_in_analysis_presets(imported);
  require(presets.size() == 6U &&
              std::ranges::all_of(presets,
                                  [](const signal::studio::AnalysisPreset& preset) {
                                    return preset.description.find("场景：") != std::string::npos &&
                                           preset.description.find("频率分辨率") != std::string::npos &&
                                           preset.description.find("时间步进") != std::string::npos &&
                                           preset.description.find("噪声方差=") != std::string::npos &&
                                           preset.description.find("计算代价=") != std::string::npos &&
                                           preset.description.find("平滑=") != std::string::npos &&
                                           preset.description.find("窄峰风险=") != std::string::npos &&
                                           preset.description.find("短突发风险=") != std::string::npos;
                                  }),
          "内置预设未逐项说明场景、分辨率、方差、代价、平滑和风险");
}

void test_ms45_cache_key_and_hit() {
  TemporaryDirectory temporary;
  signal::studio::ApplicationController controller{temporary.path() / "state"};
  require(controller.create_project(temporary.path() / "cache.signal-workspace", "ms45-cache"),
          "MS-4.5 cache project creation failed");
  const auto imported = import_fixture(controller, temporary.path());
  const auto settings = compact_analysis_settings(controller);
  const auto first_request = controller.task_runtime().issue_view_request("ms45-cache");
  auto first = controller.analyze(imported, settings, false, nullptr, first_request, "task-cache-first");
  require(first && !first.value().cache_hit, "First MS-4.5 analysis unexpectedly reported a cache hit");
  const auto second_request = controller.task_runtime().issue_view_request("ms45-cache");
  auto second = controller.analyze(imported, settings, false, nullptr, second_request, "task-cache-second");
  require(second && second.value().cache_hit && second.value().cache_key == first.value().cache_key,
          "Identical MS-4.5 settings did not hit the same cache entry");

  const auto fields = split(first.value().cache_key, '|');
  require(fields.size() == 15U && fields[0] == "signal.analysis-cache/1.2" && fields[1] == "ms45-cache" &&
              !fields[2].empty() && fields[3] == imported.fingerprint.version_id &&
              fields[4] == std::to_string(imported.loaded->range().begin()) + ":" +
                               std::to_string(imported.loaded->range().end()) &&
              fields[6] == settings.algorithm_version && fields[7] == "cpu-only" && !fields[8].empty() &&
              !fields[9].empty() && fields[10] == first.value().settings_hash.stable_text() &&
              fields[13] == "views:11" && fields[14] == "raw+smoothed",
          "Analysis cache key omits source/range/algorithm/backend/parameter/output identity");
  auto sidecar = signal::data::serialize_sidecar(imported.descriptor);
  require(sidecar, "Cache-key descriptor serialization failed");
  auto descriptor_digest =
      signal::core::hash_bytes(std::as_bytes(std::span<const char>{sidecar.value().data(), sidecar.value().size()}));
  require(descriptor_digest && fields[5] == descriptor_digest.value().hex(),
          "Analysis cache key does not contain the real SignalDescriptor digest");

  require(controller.commit_analysis(second.value(), second_request),
          "Cache baseline could not become the current analysis for minimal invalidation");
  auto smoothing_only = settings;
  smoothing_only.spectrum.smoothing = {signal::dsp::SpectrumSmoothingKind::gaussian, 5U, 1.0, 0U};
  const auto smoothing_request = controller.task_runtime().issue_view_request("ms45-cache");
  auto smoothed =
      controller.analyze(imported, smoothing_only, false, nullptr, smoothing_request, "task-smoothing-only");
  require(smoothed && !smoothed.value().cache_hit &&
              smoothed.value().invalidation == signal::dsp::AnalysisInvalidation::spectrum_smoothing &&
              smoothed.value().spectrum_transform_reused && smoothed.value().spectrogram_transform_reused &&
              smoothed.value().psd.raw_linear_values == second.value().psd.raw_linear_values &&
              smoothed.value().stft.raw_linear_values == second.value().stft.raw_linear_values &&
              smoothed.value().stft.values == second.value().stft.values &&
              smoothed.value().psd.values != second.value().psd.values,
          "Smoothing-only change did not reuse raw spectrum/STFT results with minimal invalidation");

  const auto spectrum_only_request = controller.task_runtime().issue_view_request("ms45-cache-views");
  auto spectrum_only = controller.analyze(imported, settings, false, nullptr, spectrum_only_request,
                                          "task-spectrum-only", {true, false});
  const auto spectrogram_only_request = controller.task_runtime().issue_view_request("ms45-cache-views");
  auto spectrogram_only = controller.analyze(imported, settings, false, nullptr, spectrogram_only_request,
                                             "task-spectrogram-only", {false, true});
  require(spectrum_only && spectrogram_only && !spectrum_only.value().psd.values.empty() &&
              spectrum_only.value().stft.values.empty() && !spectrum_only.value().frame.psd_db_hz.empty() &&
              spectrum_only.value().frame.stft_db.empty() && spectrogram_only.value().psd.values.empty() &&
              !spectrogram_only.value().stft.values.empty() && spectrogram_only.value().frame.psd_db_hz.empty() &&
              !spectrogram_only.value().frame.stft_db.empty() &&
              spectrum_only.value().cache_key != spectrogram_only.value().cache_key &&
              split(spectrum_only.value().cache_key, '|')[13] == "views:10" &&
              split(spectrogram_only.value().cache_key, '|')[13] == "views:01",
          "隐藏图表仍执行计算，或视图集合未进入缓存身份");

  auto hidden_stft_invalid = settings;
  hidden_stft_invalid.spectrogram.frame_length = imported.loaded->samples().size() * 2U;
  hidden_stft_invalid.spectrogram.fft_length = hidden_stft_invalid.spectrogram.frame_length;
  const auto hidden_stft_request = controller.task_runtime().issue_view_request("ms45-cache-hidden-invalid");
  auto visible_spectrum = controller.analyze(imported, hidden_stft_invalid, false, nullptr, hidden_stft_request,
                                             "task-hidden-stft-invalid", {true, false});
  require(visible_spectrum && visible_spectrum.value().cost.spectrogram_rows == 0U &&
              visible_spectrum.value().cost.spectrogram_columns == 0U,
          "隐藏 STFT 的非法参数仍阻止 PSD-only 分析或占用资源预算");

  auto hidden_spectrum_invalid = settings;
  hidden_spectrum_invalid.spectrum.frame_length = imported.loaded->samples().size() * 2U;
  hidden_spectrum_invalid.spectrum.fft_length = hidden_spectrum_invalid.spectrum.frame_length;
  const auto hidden_spectrum_request = controller.task_runtime().issue_view_request("ms45-cache-hidden-invalid");
  auto visible_stft = controller.analyze(imported, hidden_spectrum_invalid, false, nullptr, hidden_spectrum_request,
                                         "task-hidden-spectrum-invalid", {false, true});
  require(visible_stft && visible_stft.value().cost.spectrum_segment_count == 0U &&
              visible_stft.value().cost.spectrum_output_bins == 0U,
          "隐藏 PSD 的非法参数仍阻止 STFT-only 分析或占用资源预算");
}

void test_ms45_latest_view_commit() {
  TemporaryDirectory temporary;
  signal::studio::ApplicationController controller{temporary.path() / "state"};
  require(controller.create_project(temporary.path() / "latest.signal-workspace", "ms45-latest"),
          "MS-4.5 latest-result project creation failed");
  const auto imported = import_fixture(controller, temporary.path(), 4096U);
  auto settings = compact_analysis_settings(controller);
  const auto stale_request = controller.task_runtime().issue_view_request("signal-studio.analysis");
  auto latest_settings = settings;
  latest_settings.spectrum.smoothing = {signal::dsp::SpectrumSmoothingKind::gaussian, 5U, 1.0, 0U};
  const auto current_request = controller.task_runtime().issue_view_request("signal-studio.analysis");

  auto stale_future = std::async(std::launch::async, [&] {
    return controller.analyze(imported, settings, false, nullptr, stale_request, "task-stale");
  });
  auto current_future = std::async(std::launch::async, [&] {
    return controller.analyze(imported, latest_settings, false, nullptr, current_request, "task-current");
  });
  auto stale = stale_future.get();
  auto current = current_future.get();
  require(stale && current, "并发参数分析未全部完成");
  require(!controller.commit_analysis(stale.value(), stale_request) && !controller.current_analysis(),
          "Superseded ViewRequestId was allowed to commit an old result");
  require(controller.commit_analysis(current.value(), current_request),
          "Latest ViewRequestId could not commit the current result");
  require(controller.current_analysis() && controller.current_analysis()->task_id == "task-current" &&
              controller.current_analysis()->view_request == current_request &&
              controller.current_analysis()->settings_hash == current.value().settings_hash,
          "Latest-result commit did not publish the matching task/request provenance");
}

void test_ms45_project_switch_invalidates_analysis() {
  TemporaryDirectory temporary;
  signal::studio::ApplicationController controller{temporary.path() / "state"};
  const auto project_a = temporary.path() / "project-a.signal-workspace";
  const auto project_b = temporary.path() / "project-b.signal-workspace";
  const auto project_c = temporary.path() / "project-c.signal-workspace";
  const auto stale_import_project = temporary.path() / "stale-import.signal-workspace";
  std::filesystem::create_directories(temporary.path() / "source-a");
  std::filesystem::create_directories(temporary.path() / "source-b");
  std::filesystem::create_directories(temporary.path() / "stale-source");
  require(controller.create_project(stale_import_project, "stale-import"), "MS-4.5 stale-import project create failed");
  const auto stale_source = write_sc16(temporary.path() / "stale-source" / "stale_cf10MHz_sr1MSps.sc16", 262'144U);
  auto stale_descriptor =
      signal::studio::make_confirmed_descriptor(stale_source, signal::studio::parse_filename_hints(stale_source), true);
  require(stale_descriptor, "MS-4.5 stale-import descriptor failed");
  auto stale_import = controller.start_import(
      {stale_source, stale_descriptor.value(), signal::data::SourceFormat::raw, 262'144U * 4U, 4U * 1024U});
  require(stale_import, "MS-4.5 stale-import submission failed");
  require(controller.create_project(project_a, "project-a"), "MS-4.5 project A create failed");
  auto stale_result = controller.finalize_import(stale_import.value());
  require(!stale_result && controller.workspace().project_id == "project-a" &&
              controller.workspace().data_sources.empty() && !controller.current_signal(),
          "旧工程导入在工程切换后污染了新工程");
  auto imported_a = import_fixture(controller, temporary.path() / "source-a", 32'768U);
  auto settings = compact_analysis_settings(controller);
  const auto request_a = controller.task_runtime().issue_view_request("signal-studio.analysis");
  auto analysis_a = controller.analyze(imported_a, settings, false, nullptr, request_a, "task-project-a");
  require(analysis_a && analysis_a.value().project_id == "project-a", "Project A analysis provenance missing");

  require(controller.create_project(project_b, "project-b"), "MS-4.5 project B create failed");
  require(!controller.commit_analysis(analysis_a.value(), request_a),
          "Project A result committed after switching to project B");
  require(!controller.commit_measurement(analysis_a.value(), "old-selection", "old-channel"),
          "Project A artifact committed after switching to project B");
  require(controller.workspace().project_id == "project-b" && !controller.current_signal() &&
              !controller.current_analysis(),
          "Project switch retained the previous source or analysis");

  auto imported_b = import_fixture(controller, temporary.path() / "source-b", 262'144U);
  auto slow_settings = settings;
  slow_settings.spectrum.analysis_range_policy = signal::dsp::AnalysisRangePolicy::all_complete_frames;
  slow_settings.spectrum.estimator = {signal::dsp::PsdEstimatorKind::welch, 0.5, 0U};
  slow_settings.spectrum.accumulation = {signal::dsp::SpectrumAccumulationMode::linear_average, 0U, 1.0, 0U};
  const auto request_b = controller.task_runtime().issue_view_request("signal-studio.analysis");
  auto running = std::async(std::launch::async, [&controller, imported_b, slow_settings, request_b] {
    return controller.analyze(imported_b, slow_settings, false, nullptr, request_b, "task-project-b");
  });
  require(running.wait_for(std::chrono::milliseconds{5}) == std::future_status::timeout,
          "并发工程切换测试未能确认分析任务已经运行");
  const auto switched_at = std::chrono::steady_clock::now();
  require(controller.create_project(project_c, "project-c"), "MS-4.5 project C create failed");
  const auto switch_duration = std::chrono::steady_clock::now() - switched_at;
  require(switch_duration < std::chrono::milliseconds{500}, "工程切换在运行中分析期间阻塞了调用线程");
  auto completed = running.get();
  require(!completed, "工程切换后旧工程分析仍返回了可缓存结果");
  require(controller.workspace().project_id == "project-c" && !controller.current_signal() &&
              !controller.current_analysis(),
          "Concurrent project switch published stale source or analysis state");
}

void test_ms45_project_settings_persistence_and_migration() {
  TemporaryDirectory temporary;
  const auto project = temporary.path() / "settings.signal-workspace";
  signal::core::WorkspaceStore workspace_store;
  signal::dsp::AnalysisSettingsSnapshot expected_settings;
  signal::studio::AnalysisDisplaySettings expected_display;
  {
    signal::studio::ApplicationController controller{temporary.path() / "state-write"};
    require(controller.create_project(project, "ms45-settings"), "MS-4.5 settings project creation failed");
    static_cast<void>(import_fixture(controller, temporary.path()));
    expected_settings = compact_analysis_settings(controller);
    expected_settings.spectrum.window = {signal::dsp::WindowKind::kaiser, 8.0};
    expected_settings.spectrum.smoothing = {signal::dsp::SpectrumSmoothingKind::gaussian, 7U, 1.25, 0U};
    expected_display.mapping.range_mode = signal::visualization::RangeMode::manual;
    expected_display.mapping.minimum = -110.0;
    expected_display.mapping.maximum = -10.0;
    expected_display.mapping.reference_level = -10.0;
    expected_display.mapping.dynamic_range = 100.0;
    expected_display.mapping.color_map = "Viridis";
    expected_display.interpolation = "linear";
    expected_display.frequency_axis_mode = "baseband";
    require(controller.set_analysis_settings(expected_settings) &&
                controller.set_analysis_display_settings(expected_display) &&
                controller.save_user_analysis_preset("lab-preset", expected_settings) &&
                controller.set_active_analysis_preset("user:lab-preset", expected_settings, "project-channel") &&
                controller.save_project(),
            "MS-4.5 settings/display/preset persistence write failed");
  }
  {
    signal::studio::ApplicationController restored{temporary.path() / "state-read"};
    require(restored.open_project(project), "MS-4.5 settings project reopen failed");
    auto expected_serialized = signal::dsp::serialize_analysis_settings(expected_settings);
    auto actual_serialized = signal::dsp::serialize_analysis_settings(restored.analysis_settings());
    const auto presets = restored.user_analysis_presets();
    const auto& extensions = restored.workspace().extensions;
    require(expected_serialized && actual_serialized && expected_serialized.value() == actual_serialized.value() &&
                restored.analysis_display_settings() == expected_display && presets.contains("lab-preset") &&
                extensions.at("signal.analysis-scope") == "\"project-channel\"" &&
                extensions.at("signal.analysis-active-preset") == "\"user:lab-preset\"" &&
                extensions.at("signal.analysis-active-preset-hash").find("sha256:") != std::string::npos,
            "Project analysis/display settings or user preset did not restore");
    auto preset_serialized = signal::dsp::serialize_analysis_settings(presets.at("lab-preset"));
    require(preset_serialized && preset_serialized.value() == expected_serialized.value(),
            "Restored user preset differs from the saved parameter snapshot");
  }

  const auto corrupt_project = temporary.path() / "corrupt-optional.signal-workspace";
  auto corrupt_workspace = workspace_store.load(project);
  require(corrupt_workspace, "Corrupt optional-extension fixture load failed");
  corrupt_workspace.value().extensions["signal.analysis-display"] = json_string("not-a-display-schema");
  corrupt_workspace.value().extensions["signal.analysis-user-preset.bad"] = json_string("not-analysis-settings");
  require(workspace_store.save(corrupt_project, corrupt_workspace.value()),
          "Corrupt optional-extension fixture save failed");
  {
    signal::studio::ApplicationController recovered{temporary.path() / "state-corrupt"};
    require(recovered.open_project(corrupt_project) &&
                recovered.analysis_settings().spectrum.window == expected_settings.spectrum.window &&
                recovered.analysis_display_settings() == signal::studio::AnalysisDisplaySettings{} &&
                recovered.user_analysis_presets().contains("lab-preset") &&
                !recovered.user_analysis_presets().contains("bad"),
            "损坏的可选显示/预设扩展未按字段回退，或破坏了有效分析参数");
  }

  const auto legacy_project = temporary.path() / "legacy.signal-workspace";
  auto legacy = workspace_store.create("ms45-legacy");
  require(legacy && legacy.value().extensions.empty() && workspace_store.save(legacy_project, legacy.value()),
          "Legacy project fixture creation failed");
  {
    signal::studio::ApplicationController migrated{temporary.path() / "state-legacy"};
    require(migrated.open_project(legacy_project) &&
                migrated.analysis_settings().schema == "signal.analysis-settings/1.0" &&
                migrated.analysis_display_settings().schema == "signal.analysis-display/1.0" &&
                migrated.user_analysis_presets().empty() && migrated.save_project(),
            "Legacy project without analysis fields did not migrate to defaults");
  }
  auto migrated_workspace = workspace_store.load(legacy_project);
  require(migrated_workspace && migrated_workspace.value().extensions.contains("signal.analysis-settings") &&
              migrated_workspace.value().extensions.contains("signal.analysis-display") &&
              migrated_workspace.value().extensions.contains("signal.analysis-scope"),
          "Legacy project migration did not persist the new analysis extension fields");

  const auto future_project = temporary.path() / "future.signal-workspace";
  auto future = workspace_store.create("ms45-future");
  auto future_settings = signal::dsp::serialize_analysis_settings(expected_settings);
  require(future && future_settings, "Future-version project fixture setup failed");
  const auto schema_position = future_settings.value().find("signal.analysis-settings/1.0");
  require(schema_position != std::string::npos, "Serialized analysis schema marker missing");
  future_settings.value().replace(schema_position, std::string_view{"signal.analysis-settings/1.0"}.size(),
                                  "signal.analysis-settings/2.0");
  future.value().extensions["signal.analysis-settings"] = json_string(future_settings.value());
  require(workspace_store.save(future_project, future.value()), "Future-version project fixture save failed");
  signal::studio::ApplicationController rejected{temporary.path() / "state-future"};
  require(rejected.open_project(project), "事务回滚测试无法先打开有效工程");
  const auto original_project_id = rejected.workspace().project_id;
  const auto original_path = rejected.project_path();
  const auto original_settings = rejected.analysis_settings();
  const auto original_settings_text = signal::dsp::serialize_analysis_settings(original_settings);
  const auto opened = rejected.open_project(future_project);
  const auto retained_settings_text = signal::dsp::serialize_analysis_settings(rejected.analysis_settings());
  require(!opened && opened.code().reason == signal::core::ErrorReason::unavailable &&
              rejected.workspace().project_id == original_project_id && rejected.project_path() == original_path &&
              original_settings_text && retained_settings_text &&
              original_settings_text.value() == retained_settings_text.value(),
          "Unsupported future analysis version was not rejected transactionally");
}

void test_ms45_artifact_parameter_hash_and_provenance() {
  TemporaryDirectory temporary;
  signal::studio::ApplicationController controller{temporary.path() / "state"};
  require(controller.create_project(temporary.path() / "artifact.signal-workspace", "ms45-artifact"),
          "MS-4.5 artifact project creation failed");
  const auto imported = import_fixture(controller, temporary.path());
  auto settings = compact_analysis_settings(controller);
  settings.spectrum.window = {signal::dsp::WindowKind::tukey, 0.3};
  const auto request = controller.task_runtime().issue_view_request("signal-studio.analysis");
  auto analysis = controller.analyze(imported, settings, false, nullptr, request, "task-ms45-artifact");
  require(analysis && controller.commit_analysis(analysis.value(), request), "MS-4.5 artifact analysis failed");
  auto artifact = controller.commit_measurement(analysis.value(), "SEL-MS45", "CH-MS45");
  require(artifact, "MS-4.5 measurement artifact commit failed");

  auto serialized = signal::dsp::serialize_analysis_settings(settings);
  require(serialized, "Artifact settings serialization failed");
  auto expected_digest = signal::core::hash_bytes(
      std::as_bytes(std::span<const char>{serialized.value().data(), serialized.value().size()}));
  require(expected_digest && analysis.value().settings_hash.algorithm == "sha256" &&
              analysis.value().settings_hash.hex.size() == 64U &&
              std::ranges::all_of(analysis.value().settings_hash.hex,
                                  [](unsigned char character) {
                                    return std::isxdigit(character) != 0 && (character < 'A' || character > 'F');
                                  }) &&
              analysis.value().settings_hash.hex == expected_digest.value().hex(),
          "Artifact parameter identity is not the real lowercase SHA-256 of the normalized snapshot");
  const auto& source = artifact.value().descriptor.provenance;
  require(source.project_id == "ms45-artifact" && source.data_source_version_id == imported.fingerprint.version_id &&
              source.selection_id == "SEL-MS45" && source.channel_id == "CH-MS45" &&
              source.channel_version == analysis.value().inspector.channel_version &&
              source.task_id == "task-ms45-artifact" && source.algorithm_id == "signal.dsp.psd" &&
              source.algorithm_version == settings.algorithm_version &&
              source.parameter_version == analysis.value().settings_hash.stable_text(),
          "Artifact provenance omits or substitutes the real analysis source identity");
  const auto& metadata = artifact.value().descriptor.metadata;
  const std::array<std::string_view, 19> required_metadata{
      "backend",        "device",       "parameterSchema",   "parameterSnapshot", "parameterHash", "sourceRangeBegin",
      "sourceRangeEnd", "sampleRateHz", "centerFrequencyHz", "fftSize",           "frameLength",   "window",
      "enbwHz",         "rbwHz",        "estimator",         "accumulation",      "smoothing",     "prefilterApplied",
      "cacheKey"};
  require(std::ranges::all_of(required_metadata,
                              [&metadata](std::string_view key) { return metadata.contains(std::string{key}); }) &&
              metadata.at("parameterHash") == analysis.value().settings_hash.stable_text() &&
              metadata.at("parameterSnapshot") == serialized.value() &&
              metadata.at("cacheKey") == analysis.value().cache_key,
          "Artifact metadata is missing the normalized parameters, source range, DSP identity, or cache trace");
  std::ifstream payload{artifact.value().payload_path, std::ios::binary};
  const std::string payload_text{std::istreambuf_iterator<char>{payload}, std::istreambuf_iterator<char>{}};
  require(payload_text.find(analysis.value().settings_hash.stable_text()) != std::string::npos &&
              payload_text.find("\"provenance\"") != std::string::npos,
          "Committed artifact payload does not retain parameter-hash and provenance evidence");
}

void test_ms45_parameter_switch_stability(std::chrono::seconds duration) {
  TemporaryDirectory temporary;
  signal::studio::ApplicationController controller{temporary.path() / "state"};
  require(controller.create_project(temporary.path() / "stability.signal-workspace", "ms45-stability"),
          "MS-4.5 stability project creation failed");
  const auto imported = import_fixture(controller, temporary.path());

  std::array<signal::dsp::AnalysisSettingsSnapshot, 4U> variants;
  variants[0] = compact_analysis_settings(controller);

  variants[1] = variants[0];
  variants[1].spectrum.analysis_range_policy = signal::dsp::AnalysisRangePolicy::all_complete_frames;
  variants[1].spectrum.frame_length = 512U;
  variants[1].spectrum.fft_length = 1024U;
  variants[1].spectrum.zero_padding_policy = signal::dsp::ZeroPaddingPolicy::enabled;
  variants[1].spectrum.window = {signal::dsp::WindowKind::blackman_harris, 0.0};
  variants[1].spectrum.output_quantity = signal::dsp::SpectrumOutputQuantity::psd_dbfs_per_hz;
  variants[1].spectrum.normalization = signal::dsp::SpectrumNormalization::window_power;
  variants[1].spectrum.estimator = {signal::dsp::PsdEstimatorKind::welch, 0.5, 8U};
  variants[1].spectrum.accumulation = {signal::dsp::SpectrumAccumulationMode::linear_average, 8U, 1.0, 0U};
  variants[1].spectrum.smoothing = {signal::dsp::SpectrumSmoothingKind::gaussian, 7U, 1.25, 0U};
  variants[1].spectrogram.frame_length = 256U;
  variants[1].spectrogram.fft_length = 512U;
  variants[1].spectrogram.hop_length = 64U;
  variants[1].spectrogram.window = {signal::dsp::WindowKind::kaiser, 7.5};
  variants[1].spectrogram.padding_policy = signal::dsp::ZeroPaddingPolicy::enabled;
  variants[1].spectrogram.smoothing = {signal::dsp::SpectrogramFrequencySmoothingKind::gaussian, 5U, 1.0,
                                       signal::dsp::SpectrogramTimeSmoothingKind::exponential, 0.35};

  variants[2] = variants[0];
  variants[2].spectrum.analysis_range_policy = signal::dsp::AnalysisRangePolicy::all_complete_frames;
  variants[2].spectrum.frame_length = 1024U;
  variants[2].spectrum.fft_length = 2048U;
  variants[2].spectrum.zero_padding_policy = signal::dsp::ZeroPaddingPolicy::enabled;
  variants[2].spectrum.window = {signal::dsp::WindowKind::tukey, 0.3};
  variants[2].spectrum.accumulation = {signal::dsp::SpectrumAccumulationMode::maximum_hold, 8U, 1.0, 1U};
  variants[2].spectrum.smoothing = {signal::dsp::SpectrumSmoothingKind::savitzky_golay, 7U, 0.0, 3U};
  variants[2].spectrogram.frame_length = 512U;
  variants[2].spectrogram.fft_length = 512U;
  variants[2].spectrogram.hop_length = 256U;
  variants[2].spectrogram.window = {signal::dsp::WindowKind::tukey, 0.25};

  variants[3] = variants[0];
  variants[3].spectrum.analysis_range_policy = signal::dsp::AnalysisRangePolicy::all_complete_frames;
  variants[3].spectrum.frame_length = 256U;
  variants[3].spectrum.fft_length = 256U;
  variants[3].spectrum.window = {signal::dsp::WindowKind::hamming, 0.0};
  variants[3].spectrum.estimator = {signal::dsp::PsdEstimatorKind::welch, 0.75, 12U};
  variants[3].spectrum.accumulation = {signal::dsp::SpectrumAccumulationMode::exponential_average, 12U, 0.2, 0U};
  variants[3].spectrum.smoothing = {signal::dsp::SpectrumSmoothingKind::moving_average, 5U, 0.0, 0U};
  variants[3].spectrogram.frame_length = 128U;
  variants[3].spectrogram.fft_length = 256U;
  variants[3].spectrogram.hop_length = 32U;
  variants[3].spectrogram.window = {signal::dsp::WindowKind::flat_top, 0.0};
  variants[3].spectrogram.padding_policy = signal::dsp::ZeroPaddingPolicy::enabled;
  variants[3].spectrogram.smoothing = {signal::dsp::SpectrogramFrequencySmoothingKind::none, 0U, 0.0,
                                       signal::dsp::SpectrogramTimeSmoothingKind::exponential, 0.5};

  signal::visualization::ViewportController viewport{"ms45-stability"};
  const auto frequency_range = signal::visualization::make_frequency_range(9'500'000, 10'500'000);
  require(frequency_range &&
              viewport.bind_source(imported.fingerprint.version_id, imported.loaded->range(), frequency_range.value()),
          "MS-4.5 stability viewport setup failed");

  const auto deadline = std::chrono::steady_clock::now() + duration;
  std::uint64_t iterations{};
  std::uint64_t committed{};
  std::uint64_t stale_rejected{};
  std::uint64_t cache_hits{};
  do {
    const auto variant_index = static_cast<std::size_t>(iterations % variants.size());
    require(controller.set_analysis_settings(variants[variant_index]), "MS-4.5 stability parameter switch failed");

    auto display = controller.analysis_display_settings();
    display.mapping.range_mode = signal::visualization::RangeMode::manual;
    display.mapping.minimum = variant_index % 2U == 0U ? -120.0 : -100.0;
    display.mapping.maximum = variant_index % 2U == 0U ? -20.0 : -10.0;
    display.mapping.reference_level = display.mapping.maximum;
    display.mapping.dynamic_range = display.mapping.maximum - display.mapping.minimum;
    display.mapping.color_map = variant_index % 2U == 0U ? "Industrial" : "Viridis";
    display.interpolation = variant_index % 2U == 0U ? "nearest" : "linear";
    display.frequency_axis_mode = variant_index % 2U == 0U ? "absolute-if-available" : "baseband";
    require(controller.set_analysis_display_settings(display), "MS-4.5 stability display switch failed");

    const auto span = variant_index % 2U == 0U ? 2048U : 4096U;
    const auto maximum_begin = imported.loaded->range().size() - span;
    const auto begin = maximum_begin == 0U ? 0U : (iterations * 257U) % maximum_begin;
    require(viewport.resize_time(begin, span), "MS-4.5 stability time zoom failed");
    const auto view_frequency =
        variant_index % 2U == 0U ? signal::visualization::make_frequency_range(9'750'000, 10'250'000) : frequency_range;
    require(view_frequency && viewport.set_frequency(view_frequency.value()),
            "MS-4.5 stability frequency view switch failed");

    const auto request = controller.task_runtime().issue_view_request("ms45-parameter-stability");
    auto analysis = controller.analyze(imported, variants[variant_index], false, nullptr, request,
                                       "ms45-stability-" + std::to_string(iterations));
    require(analysis, "MS-4.5 stability analysis failed");
    cache_hits += analysis.value().cache_hit ? 1U : 0U;

    if (iterations % 17U == 0U) {
      const auto current = controller.task_runtime().issue_view_request("ms45-parameter-stability");
      require(!controller.commit_analysis(std::move(analysis).value(), request),
              "MS-4.5 stability accepted a stale result");
      ++stale_rejected;
      analysis = controller.analyze(imported, variants[variant_index], false, nullptr, current,
                                    "ms45-stability-current-" + std::to_string(iterations));
      require(analysis && controller.commit_analysis(std::move(analysis).value(), current),
              "MS-4.5 stability latest result commit failed");
    } else {
      require(controller.commit_analysis(std::move(analysis).value(), request),
              "MS-4.5 stability current result commit failed");
    }
    ++committed;
    ++iterations;
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  } while (std::chrono::steady_clock::now() < deadline);

  require(iterations >= 4U && committed == iterations && stale_rejected > 0U && cache_hits > 0U &&
              static_cast<bool>(controller.current_analysis()),
          "MS-4.5 stability did not exercise bounded cache/latest-result/view switching");
  std::cout << "ms45_stability_iterations=" << iterations << " duration_seconds=" << duration.count()
            << " cache_hits=" << cache_hits << " stale_rejected=" << stale_rejected << '\n';
}

void test_headless_self_test() {
  TemporaryDirectory temporary;
  require(signal::studio::run_headless_self_test(temporary.path()), "Headless application self-test failed");
}

void test_cancel_retry() {
  TemporaryDirectory temporary;
  auto source = std::filesystem::path{SIGNAL_STUDIO_MS04_EXTERNAL_DATA_DIR} /
                "x310_capture_cf1425MHz_sr50MSps_20260521_144220.sc16";
  constexpr std::uint64_t first_read_bytes = 128U * 1024U * 1024U;
  std::uint64_t first_chunk_bytes = 64U * 1024U;
  if (!std::filesystem::exists(source)) {
    source = temporary.path() / "x310_capture_cf1425MHz_sr50MSps_20260521_144220.sc16";
    constexpr std::uint64_t ci_fixture_bytes = 256U * 1024U * 1024U;
    std::ofstream fixture{source, std::ios::binary | std::ios::trunc};
    require(fixture && fixture.seekp(static_cast<std::streamoff>(ci_fixture_bytes - 1U)) && fixture.put('\0'),
            "Cancel/retry CI fixture creation failed");
    fixture.close();
    first_chunk_bytes = 4U * 1024U;
  }
  signal::studio::ApplicationController controller{temporary.path() / "state"};
  require(controller.create_project(temporary.path() / "cancel.signal-workspace", "cancel"),
          "Cancel/retry project creation failed");
  auto description =
      signal::studio::make_confirmed_descriptor(source, signal::studio::parse_filename_hints(source), true);
  require(description, "Cancel/retry descriptor failed");
  auto first = controller.start_import(
      {source, description.value(), signal::data::SourceFormat::raw, first_read_bytes, first_chunk_bytes});
  require(first, "Cancelable import submission failed");
  bool made_progress{};
  for (std::size_t attempt = 0; attempt < 200U; ++attempt) {
    auto status = first.value().handle().status();
    require(status, "Cancelable import status failed");
    if (status.value().progress > 0.0) {
      made_progress = true;
      break;
    }
    if (signal::task::is_terminal(status.value().state)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
  }
  require(made_progress, "Cancelable import completed before exposing progress");
  require(first.value().handle().cancel(), "Import cancel request failed");
  auto partial = controller.finalize_import(first.value());
  require(partial && partial.value().partial_read && partial.value().loaded->samples().size() > 0U,
          "Cancel did not retain a verified read-only prefix");
  auto retry = controller.start_import(
      {source, description.value(), signal::data::SourceFormat::raw, 1U * 1024U * 1024U, 128U * 1024U});
  require(retry, "Retry import submission failed");
  auto completed = controller.finalize_import(retry.value());
  require(completed && !completed.value().partial_read, "Retry import did not complete a fresh attempt");
}

void test_error_recovery() {
  TemporaryDirectory temporary;
  const auto empty = temporary.path() / "empty.sc16";
  require(signal::core::AtomicFileStore{}.write(empty, {}), "Empty fixture write failed");
  require(!signal::studio::make_confirmed_descriptor(empty, signal::studio::parse_filename_hints(empty), true),
          "Empty source was accepted");

  signal::studio::ApplicationController controller{temporary.path() / "state"};
  require(controller.create_project(temporary.path() / "errors.signal-workspace", "errors"),
          "Error-recovery project creation failed");
  auto description = complex_descriptor();
  auto missing = controller.start_import(
      {temporary.path() / "missing.raw", description, signal::data::SourceFormat::raw, 4096U, 4096U});
  require(missing, "Missing-path task should be accepted then fail with task evidence");
  require(!controller.finalize_import(missing.value()), "Missing source unexpectedly imported");

  const auto blocking_file = temporary.path() / "artifact-root";
  require(signal::core::AtomicFileStore{}.write(blocking_file, bytes("not-a-directory")),
          "Blocking file fixture failed");
  signal::core::ArtifactStore unwritable{blocking_file};
  auto result_description = descriptor("permission-equivalent", ArtifactFormat::json);
  require(!unwritable.commit(result_description, valid_json(result_description.provenance)),
          "Non-directory artifact root unexpectedly accepted a commit");

  const auto corrupt_project = temporary.path() / "corrupt.signal-workspace";
  require(signal::core::AtomicFileStore{}.write(corrupt_project, bytes("{broken")), "Corrupt project fixture failed");
  require(!controller.open_project(corrupt_project), "Corrupted project unexpectedly opened");
}

void test_rel_soak(std::chrono::seconds duration) {
  TemporaryDirectory temporary;
  const auto source = write_sc16(temporary.path() / "soak_cf10MHz_sr1MSps.sc16", 4096U);
  const auto deadline = std::chrono::steady_clock::now() + duration;
  std::uint64_t iterations{};
  do {
    const auto cycle = temporary.path() / "active-cycle";
    {
      signal::studio::ApplicationController controller{cycle / "state"};
      const auto project = cycle / "soak.signal-workspace";
      require(controller.create_project(project, "soak"), "Soak project creation failed");
      auto descriptor =
          signal::studio::make_confirmed_descriptor(source, signal::studio::parse_filename_hints(source), true);
      require(descriptor, "Soak descriptor failed");
      auto task = controller.start_import(
          {source, descriptor.value(), signal::data::SourceFormat::raw, 16U * 1024U, 4U * 1024U});
      require(task, "Soak import submission failed");
      const auto terminal = task.value().handle().wait();
      require(terminal, "Soak import wait failed");
      auto imported = controller.finalize_import(task.value());
      if (!imported) {
        throw std::runtime_error("Soak import failed: " + std::string{imported.error().message()} + " / " +
                                 std::string{imported.error().diagnostic()} +
                                 " state=" + std::string{signal::task::to_string(terminal.value().state)} +
                                 " text=" + terminal.value().status_text);
      }
      auto analysis = controller.analyze(imported.value());
      require(analysis, "Soak analysis failed");
      require(controller.commit_measurement(analysis.value()), "Soak artifact commit failed");
      require(controller.close_project(), "Soak project close failed");
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(cycle, cleanup_error);
    require(!cleanup_error, "Soak cycle cleanup failed");
    ++iterations;
    std::this_thread::sleep_for(std::chrono::milliseconds{250});
  } while (std::chrono::steady_clock::now() < deadline);
  require(iterations > 0U, "Soak executed no mixed-operation iterations");
  std::cout << "soak_iterations=" << iterations << " duration_seconds=" << duration.count() << '\n';
}

void test_rel_open_close() {
  TemporaryDirectory temporary;
  signal::studio::ApplicationController controller{temporary.path() / "state"};
  const auto project = temporary.path() / "cycle.signal-workspace";
  require(controller.create_project(project, "cycle") && controller.close_project(), "Initial project cycle failed");
  for (std::size_t index = 0; index < 100U; ++index) {
    require(controller.open_project(project) && controller.close_project(), "100-cycle project reopen failed");
  }
}

void test_rel_failure_source_safety() {
  TemporaryDirectory temporary;
  const auto source = write_sc16(temporary.path() / "safe_cf10MHz_sr1MSps.sc16", 4096U);
  auto before = signal::core::hash_file(source);
  require(before, "Source hash failed");
  signal::core::ArtifactStore store{temporary.path() / "artifacts"};
  std::filesystem::create_directories(store.root() / ".staging-corrupt");
  require(store.recover(), "Corrupt staging recovery failed");
  auto invalid = descriptor("invalid-result", ArtifactFormat::raw, ArtifactKind::sampled_data);
  require(!store.commit(invalid, bytes("payload")), "Fault-injected invalid result unexpectedly committed");
  auto after = signal::core::hash_file(source);
  require(after && after.value() == before.value(), "Failure recovery changed the external source");
}

void test_rel_checksum() {
  TemporaryDirectory temporary;
  signal::core::ArtifactStore store{temporary.path() / "artifacts"};
  auto record = commit_json(store);
  std::ofstream output(record.payload_path, std::ios::binary | std::ios::trunc);
  output << "tampered";
  output.close();
  require(!store.verify(record), "Artifact checksum did not detect corruption");
}

void test_external_recordings() {
  const auto root = std::filesystem::path{SIGNAL_STUDIO_MS04_EXTERNAL_DATA_DIR};
  const std::array<std::filesystem::path, 3> files{root /
                                                       "20241110-174401-662_bw_12800000_sampleTime_0.4_rollOff_0.3.wav",
                                                   root / "x310_capture_cf1245MHz_sr50MSps_20260521_144927.sc16",
                                                   root / "x310_capture_cf1425MHz_sr50MSps_20260521_144220.sc16"};
  for (const auto& file : files) {
    require(std::filesystem::exists(file), "Approved external recording missing");
    auto fingerprint = signal::core::fingerprint_source(file);
    require(fingerprint && fingerprint.value().size_bytes > 0U, "External recording fingerprint failed");
    if (file.extension() == ".wav") {
      auto wav = signal::data::read_wav_descriptor(file, true, signal::data::ComponentOrder::iq);
      require(wav, "Approved WAV descriptor failed");
      auto source = signal::data::FileDataSource::open_wav(file, fingerprint.value().version_id, true,
                                                           signal::data::ComponentOrder::iq);
      require(source, "Approved WAV open failed");
      const auto count = std::min<std::uint64_t>(4096U, wav.value().descriptor.requested_sample_range.size());
      auto range = signal::data::SampleRange::from_count(0U, count);
      auto read = source.value()->read({range.value(), count * wav.value().descriptor.frame_bytes().value(), {}});
      require(read && read.value().samples.view().size() == count, "Approved WAV bounded read failed");
    } else {
      auto descriptor =
          signal::studio::make_confirmed_descriptor(file, signal::studio::parse_filename_hints(file), true);
      require(descriptor, "Approved SC16 descriptor failed");
      auto read = signal::data::read_raw_samples(file, descriptor.value(),
                                                 signal::data::SampleRange::from_count(0U, 4096U).value(), 4096U * 4U);
      require(read && read.value().samples.view().size() == 4096U, "Approved SC16 bounded read failed");
    }
  }
}

[[nodiscard]] std::string argument(int argc, char* argv[], std::string_view name, std::string fallback = {}) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::string_view{argv[index]} == name) {
      return argv[index + 1];
    }
  }
  return fallback;
}

} // namespace

int main(int argc, char* argv[]) {
  try {
    const auto test_case = argument(argc, argv, "--case");
    const auto soak_seconds = std::chrono::seconds{std::stoll(argument(argc, argv, "--soak-seconds", "2"))};
    const std::map<std::string, std::function<void()>, std::less<>> tests{
        {"FR-INS-001", test_inspector_views},
        {"FR-INS-002", test_constellation},
        {"FR-INS-003", test_eye_rules},
        {"FR-INS-004", test_histograms},
        {"FR-INS-005", test_instantaneous_frequency},
        {"FR-INS-006", test_inspector_state_versions},
        {"FR-INS-007", test_layout_degradation},
        {"FR-EXP-001", test_csv_json},
        {"FR-EXP-002", test_provenance},
        {"FR-EXP-003", test_png},
        {"FR-EXP-004", test_raw},
        {"FR-EXP-005", test_wav},
        {"FR-EXP-006", test_plugin_format},
        {"FR-EXP-007", test_atomic_no_overwrite},
        {"FR-EXP-008", test_batch_export},
        {"FR-EXP-009", test_filter_and_current},
        {"NFR-REL-001", [soak_seconds] { test_rel_soak(soak_seconds); }},
        {"NFR-REL-002", test_rel_open_close},
        {"NFR-REL-003", test_rel_failure_source_safety},
        {"NFR-REL-004", test_rel_checksum},
        {"APP-PROJECT-IMPORT-ANALYSIS", test_application_flow},
        {"APP-MS45-PARAMETER-EFFECTS", test_ms45_parameter_effects},
        {"APP-MS45-CACHE-KEY-HIT", test_ms45_cache_key_and_hit},
        {"APP-MS45-LATEST-VIEW-COMMIT", test_ms45_latest_view_commit},
        {"APP-MS45-PROJECT-SWITCH-INVALIDATION", test_ms45_project_switch_invalidates_analysis},
        {"APP-MS45-PROJECT-PERSISTENCE-MIGRATION", test_ms45_project_settings_persistence_and_migration},
        {"APP-MS45-ARTIFACT-PROVENANCE", test_ms45_artifact_parameter_hash_and_provenance},
        {"APP-MS45-PARAMETER-STABILITY", [soak_seconds] { test_ms45_parameter_switch_stability(soak_seconds); }},
        {"APP-CANCEL-RETRY", test_cancel_retry},
        {"APP-ERROR-RECOVERY", test_error_recovery},
        {"APP-HEADLESS-SELF-TEST", test_headless_self_test},
        {"EXTERNAL-ALL-RECORDINGS", test_external_recordings}};
    const auto found = tests.find(test_case);
    if (found == tests.end()) {
      std::cerr << "Unknown --case: " << test_case << '\n';
      return 2;
    }
    found->second();
    std::cout << "PASS " << test_case << '\n';
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "FAIL: " << exception.what() << '\n';
    return 1;
  }
}
