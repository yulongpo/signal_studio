#include "application.hpp"

#include "signal_studio/core/artifact.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/workbench/inspector.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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
