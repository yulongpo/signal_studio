#pragma once

#include "signal_studio/core/artifact.hpp"
#include "signal_studio/core/services.hpp"
#include "signal_studio/data/loading.hpp"
#include "signal_studio/data/preview.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/task_runtime/task_runtime.hpp"
#include "signal_studio/visualization/visualization.hpp"
#include "signal_studio/workbench/inspector.hpp"
#include "signal_studio/workbench/workbench.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace signal::studio {

struct FilenameHints final {
  std::optional<double> sample_rate_hz;
  std::optional<double> center_frequency_hz;
  std::optional<data::ScalarType> scalar_type;
  std::optional<data::SignalKind> signal_kind;
  std::vector<std::string> evidence;
};

[[nodiscard]] FilenameHints parse_filename_hints(const std::filesystem::path& path);
[[nodiscard]] core::Result<data::SignalDescriptor>
make_confirmed_descriptor(const std::filesystem::path& path, const FilenameHints& hints, bool confirm_filename_hints);

struct ImportRequest final {
  std::filesystem::path path;
  data::SignalDescriptor descriptor;
  data::SourceFormat source_format{data::SourceFormat::raw};
  std::uint64_t initial_bytes{8U * 1024U * 1024U};
  std::uint64_t chunk_bytes{512U * 1024U};
};

struct ImportedSignal final {
  std::filesystem::path source_path;
  data::SourceFormat source_format{data::SourceFormat::raw};
  data::SignalDescriptor descriptor;
  core::SourceFingerprint fingerprint;
  data::DataFacts facts;
  std::shared_ptr<data::FileDataSource> source;
  std::shared_ptr<const data::LoadedDataRange> loaded;
  bool partial_read{};
};

struct AnalysisBundle final {
  visualization::ViewportSnapshot viewport;
  visualization::VisualizationFrame frame;
  workbench::InspectorChannelState inspector;
  std::string backend_id;
};

class ImportTask final {
public:
  ImportTask() = default;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const task::TaskHandle& handle() const noexcept;
  [[nodiscard]] core::Result<ImportedSignal> result() const;

private:
  struct SharedState;
  ImportTask(task::TaskHandle handle, std::shared_ptr<SharedState> state);
  task::TaskHandle handle_;
  std::shared_ptr<SharedState> state_;
  friend class ApplicationController;
};

class ApplicationController final {
public:
  explicit ApplicationController(std::filesystem::path state_directory);
  ~ApplicationController();
  ApplicationController(const ApplicationController&) = delete;
  ApplicationController& operator=(const ApplicationController&) = delete;

  [[nodiscard]] core::Status create_project(const std::filesystem::path& project_path, std::string project_id);
  [[nodiscard]] core::Status open_project(const std::filesystem::path& project_path, bool read_only = false);
  [[nodiscard]] core::Status save_project();
  [[nodiscard]] core::Status close_project();
  [[nodiscard]] core::Result<ImportTask> start_import(ImportRequest request);
  [[nodiscard]] core::Result<ImportedSignal> finalize_import(const ImportTask& task);
  [[nodiscard]] core::Result<AnalysisBundle> analyze(const ImportedSignal& imported, bool prefer_cuda = false);
  [[nodiscard]] core::Result<core::ArtifactRecord>
  commit_measurement(const AnalysisBundle& analysis, std::string selection_id = {}, std::string channel_id = "CH-01");
  [[nodiscard]] core::Result<core::ArtifactRecord> commit_sample_export(const ImportedSignal& imported,
                                                                        data::SourceFormat format,
                                                                        std::uint64_t maximum_samples = 16'384U);
  [[nodiscard]] core::Result<std::vector<core::ArtifactRecord>> results(const core::ArtifactFilter& filter = {}) const;
  [[nodiscard]] core::Result<std::filesystem::path> export_result(const core::ArtifactRecord& result,
                                                                  const std::filesystem::path& destination) const;

  [[nodiscard]] const core::Workspace& workspace() const noexcept;
  [[nodiscard]] const std::filesystem::path& project_path() const noexcept;
  [[nodiscard]] const std::optional<ImportedSignal>& current_signal() const noexcept;
  [[nodiscard]] const std::optional<AnalysisBundle>& current_analysis() const noexcept;
  [[nodiscard]] std::vector<task::TaskStatus> task_history() const;
  [[nodiscard]] workbench::WorkbenchContent workbench_content() const;
  [[nodiscard]] core::Result<std::vector<std::filesystem::path>> recent_projects() const;
  [[nodiscard]] task::TaskRuntime& task_runtime() noexcept;

private:
  [[nodiscard]] core::Status persist_source_link(const ImportedSignal& imported, std::string_view source_id);
  [[nodiscard]] core::Result<std::filesystem::path> resolve_source_link(const core::WorkspaceDataSource& source) const;
  [[nodiscard]] core::Status append_result_to_workspace(const core::ArtifactRecord& record);

  std::filesystem::path state_directory_;
  core::WorkspaceStore workspace_store_;
  core::RecentProjectStore recent_projects_;
  core::Workspace workspace_;
  std::filesystem::path project_path_;
  core::CurrentContextStore current_context_;
  task::TaskRuntime task_runtime_;
  std::unique_ptr<core::ArtifactStore> artifact_store_;
  std::optional<ImportedSignal> current_signal_;
  std::optional<AnalysisBundle> current_analysis_;
};

/// Qt 启动前执行的真实 Core/Data/DSP/Task/Artifact 冒烟闭环。
[[nodiscard]] core::Status run_headless_self_test(const std::filesystem::path& scratch_directory);

} // namespace signal::studio
