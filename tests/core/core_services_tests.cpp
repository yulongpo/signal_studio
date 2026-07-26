#include "signal_studio/core/result.hpp"
#include "signal_studio/core/services.hpp"

#include <array>
#include <chrono>
#include <cstddef>
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
using namespace signal::core;

template <typename T> void require(T&& condition, std::string_view message) {
  if (!static_cast<bool>(condition))
    throw std::runtime_error(std::string{message});
}

std::vector<std::byte> bytes(std::string_view text) {
  std::vector<std::byte> result;
  result.reserve(text.size());
  for (const char character : text)
    result.push_back(static_cast<std::byte>(character));
  return result;
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("signal-studio-ms01-" + std::to_string(stamp));
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() {
    std::filesystem::remove_all(path_);
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_plain(const std::filesystem::path& path, std::string_view text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  require(static_cast<bool>(output), "test fixture write failed");
}

std::string read_plain(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void test_result_and_services() {
  Result<int> value{42};
  require(value && value.value() == 42, "Result value contract failed");
  Result<void> empty;
  require(empty, "Result<void> success contract failed");
  Result<int> error{Status::failure({ErrorDomain::core, ErrorReason::invalid_argument}, "invalid")};
  require(!error && error.error().code().reason == ErrorReason::invalid_argument, "Result error contract failed");

  const auto digest = hash_bytes(bytes("abc"));
  require(digest && digest.value().hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 known-answer test failed");

  MemoryLogger logger;
  logger.write({std::chrono::system_clock::now(), LogLevel::info, "core", "ready", {{"id", "42"}}});
  require(logger.snapshot().size() == 1U, "memory logger failed");

  Configuration configuration;
  require(configuration.set("cache.memory-percent", "25"), "configuration set failed");
  const auto percent = configuration.get_uint64("cache.memory-percent", 10U, 60U);
  require(percent && percent.value() == 25U, "configuration numeric parse failed");
  require(!configuration.set("bad key", "1"), "configuration accepted invalid key");
  require(!configuration.get_uint64("cache.memory-percent", 30U, 60U), "configuration accepted out-of-range value");

  Seconds seconds{1.5};
  Hertz hertz{1.5};
  require(seconds.value == hertz.value, "typed quantities lost their values");
  static_assert(!std::is_same_v<Seconds, Hertz>);
}

void test_paths_and_atomic_files() {
  TemporaryDirectory directory;
  const auto project = directory.path() / "project.signal-workspace";
  const auto source = directory.path() / "data" / "capture.iq";
  std::filesystem::create_directories(source.parent_path());
  write_plain(source, "source-data");
  require(validate_relative_resource_path("data/capture.iq"), "valid relative path rejected");
  require(!validate_relative_resource_path("../capture.iq"), "path traversal accepted");
  require(!validate_relative_resource_path("C:/capture.iq"), "device path accepted");
  const auto resolved = resolve_relative_resource(project, "data/capture.iq");
  require(resolved && std::filesystem::equivalent(resolved.value(), source), "relative resource resolution failed");

  AtomicFileStore store;
  require(store.write(project, bytes("old")), "atomic write failed");
  write_plain(project.string() + ".tmp", "interrupted-new");
  require(store.recover(project), "atomic recovery failed");
  require(read_plain(project) == "old", "recovery replaced the prior valid file");
  require(!std::filesystem::exists(project.string() + ".tmp"), "stale temporary file was not cleaned");
  const auto bounded = store.read(project, 3U);
  require(bounded && bounded.value().size() == 3U, "bounded read failed");
  require(!store.read(project, 2U), "bounded read accepted oversized file");
}

void test_source_fingerprint() {
  TemporaryDirectory directory;
  const auto source = directory.path() / "capture.iq";
  write_plain(source, "0123456789abcdefghijklmnopqrstuvwxyz");
  const auto original_bytes = read_plain(source);
  const auto first = fingerprint_source(source, 8U);
  const auto second = fingerprint_source(source, 8U);
  require(first && second && first.value() == second.value(), "stable source fingerprint changed");
  require(read_plain(source) == original_bytes, "fingerprinting modified the source");
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  write_plain(source, "0123456789abcdefghijklmnopqrstuvwxyZ");
  const auto changed = fingerprint_source(source, 8U);
  require(changed && changed.value().version_id != first.value().version_id,
          "content change did not invalidate DataSourceVersionId");
}

Workspace sample_workspace() {
  WorkspaceStore store;
  auto created = store.create("workspace-001");
  require(created, "workspace create failed");
  Workspace workspace = std::move(created).value();
  workspace.data_sources.push_back({"source-001", "version-001", "data/capture.iq",
                                    R"({"sampleRate":12800000,"signalType":"complex"})", 0U, 4096U, true});
  workspace.objects.push_back(
      {"selection-001", "selection", "version-001", {{"begin", "100"}, {"end", "200"}}, {"channel-001"}});
  workspace.objects.push_back({"marker-001", "marker", "version-001", {{"sample", "150"}}, {"selection-001"}});
  workspace.objects.push_back({"channel-001", "analysis-channel", "version-001", {{"name", "主通道"}}, {"chain-001"}});
  workspace.objects.push_back(
      {"chain-001", "processing-chain", "version-001", {{"algorithm", "preview"}}, {"result-001"}});
  workspace.tasks.push_back({"task-001", "analysis-task", "version-001", {{"state", "completed"}}, {"result-001"}});
  workspace.results.push_back(
      {"result-001", "spectrum-result", "version-001", {{"artifact", "results/spectrum.bin"}}, {"task-001"}});
  workspace.extensions.emplace("org.signalplatform.signal-studio", R"({"layout":{"bottom":240}})");
  return workspace;
}

void test_workspace_lifecycle() {
  TemporaryDirectory directory;
  std::filesystem::create_directories(directory.path() / "data");
  write_plain(directory.path() / "data" / "capture.iq", "immutable-source");
  const auto source_before = read_plain(directory.path() / "data" / "capture.iq");
  const auto project = directory.path() / "project.signal-workspace";
  WorkspaceStore store;
  Workspace workspace = sample_workspace();
  require(store.save(project, workspace), "workspace save failed");
  auto loaded = store.load(project);
  require(loaded, "workspace load failed");
  require(loaded.value() == workspace, "workspace graph did not round-trip");
  require(read_plain(directory.path() / "data" / "capture.iq") == source_before, "workspace save modified source data");

  const auto save_as = directory.path() / "project-copy.signal-workspace";
  require(store.save(save_as, workspace), "workspace save-as failed");
  auto copied = store.load(save_as);
  require(copied && copied.value() == workspace, "workspace save-as graph did not round-trip");

  auto dangling = workspace;
  dangling.objects.front().relations.push_back("missing-object");
  require(!store.save(directory.path() / "dangling.signal-workspace", dangling),
          "workspace accepted a dangling graph relation");
  auto wrong_version = workspace;
  wrong_version.objects.front().data_source_version_id = "missing-version";
  require(!store.save(directory.path() / "wrong-version.signal-workspace", wrong_version),
          "workspace accepted an object outside the data-source graph");

  require(store.autosave(project, workspace), "workspace autosave failed");
  auto recovered = store.recover_autosave(project);
  require(recovered && recovered.value() == workspace, "workspace autosave recovery failed");

  auto read_only = store.load(project, true);
  require(read_only && read_only.value().read_only, "read-only open failed");
  require(!store.save(project, read_only.value()), "read-only workspace was saved");

  const auto old_root = directory.path() / "old-root";
  const auto new_root = directory.path() / "new-root";
  std::filesystem::create_directories(old_root / "data");
  std::filesystem::create_directories(new_root / "data");
  write_plain(new_root / "data" / "capture.iq", "relocated");
  require(store.relocate(workspace, old_root, new_root), "batch relocation validation failed");
  require(workspace.data_sources.front().relative_uri == "data/capture.iq", "relocation changed portable URI");

  require(store.close(workspace), "workspace close failed");
  require(workspace.project_id.empty() && workspace.data_sources.empty(), "workspace close retained state");
}

void test_workspace_schema() {
  TemporaryDirectory directory;
  AtomicFileStore files;
  WorkspaceStore store;
  const auto future = directory.path() / "future.signal-workspace";
  const std::string future_json =
      R"({"schema":"signal.workspace/1.1","workspaceId":"future","resources":[],"objects":[],"extensions":{},"unknownOptional":true})";
  require(files.write(future, bytes(future_json)), "future-schema fixture write failed");
  auto future_loaded = store.load(future);
  require(future_loaded && future_loaded.value().read_only, "newer same-major schema was not downgraded safely");
  require(future_loaded.value().loaded_schema_version == SchemaVersion{1U, 1U}, "loaded schema provenance lost");
  require(!store.save(future, future_loaded.value()), "newer same-major workspace was downgraded");

  const auto incompatible = directory.path() / "incompatible.signal-workspace";
  const std::string incompatible_json =
      R"({"schema":"signal.workspace/2.0","workspaceId":"future","resources":[],"objects":[],"extensions":{}})";
  require(files.write(incompatible, bytes(incompatible_json)), "incompatible-schema fixture write failed");
  require(!store.load(incompatible), "incompatible schema major was accepted");
}

void test_recent_projects() {
  TemporaryDirectory directory;
  RecentProjectStore recent{directory.path() / "state" / "recent.txt"};
  const auto first = directory.path() / "first.signal-workspace";
  const auto second = directory.path() / "second.signal-workspace";
  require(recent.record(first), "first recent project record failed");
  require(recent.record(second), "second recent project record failed");
  require(recent.record(first), "recent project deduplication failed");
  auto loaded = recent.load();
  require(loaded && loaded.value().size() == 2U, "recent projects did not persist");
  require(loaded.value().front().filename() == first.filename(), "most-recent ordering failed");
}

void test_current_context() {
  CurrentContextStore contexts;
  require(!contexts.switch_to({"", "source", "version", 0U}), "invalid current context accepted");
  require(contexts.switch_to({"project", "source-a", "version-a", 99U}), "current context switch failed");
  const auto first = contexts.snapshot();
  require(first.generation == 1U, "context generation was not assigned atomically");
  require(contexts.validate_consistency(std::array{first, first}), "consistent consumers rejected");
  require(contexts.switch_to({"project", "source-b", "version-b", 0U}), "second context switch failed");
  require(!contexts.validate_consistency(std::array{first}), "stale context was not blocked");

  WorkspaceStore store;
  auto created = store.create("project");
  require(created, "context workspace create failed");
  auto workspace = std::move(created).value();
  workspace.data_sources.push_back({"source-a", "version-a", "data/a.iq", "{}", 0U, 10U, true});
  workspace.data_sources.push_back({"source-b", "version-b", "data/b.iq", "{}", 0U, 10U, true});
  workspace.objects.push_back({"selection-a", "selection", "version-a", {}, {}});
  workspace.objects.push_back({"selection-b", "selection", "version-b", {}, {}});
  auto current = contexts.select_current_objects(workspace, "selection");
  require(current && current.value().size() == 1U && current.value().front().id == "selection-b",
          "旧数据源对象被错误选择为新数据源的当前对象");
}

} // namespace

int main(int argc, char** argv) {
  const std::map<std::string, std::function<void()>> tests{
      {"result-and-services", test_result_and_services},
      {"paths-and-atomic-files", test_paths_and_atomic_files},
      {"source-fingerprint", test_source_fingerprint},
      {"workspace-lifecycle", test_workspace_lifecycle},
      {"workspace-schema", test_workspace_schema},
      {"recent-projects", test_recent_projects},
      {"current-context", test_current_context},
      {"FR-PRJ-001",
       [] {
         test_workspace_lifecycle();
         test_recent_projects();
       }},
      {"FR-PRJ-002", test_workspace_lifecycle},
      {"FR-PRJ-003", test_paths_and_atomic_files},
      {"FR-PRJ-004",
       [] {
         test_source_fingerprint();
         test_workspace_lifecycle();
       }},
      {"FR-PRJ-005", test_source_fingerprint},
      {"FR-PRJ-006", test_workspace_lifecycle},
      {"FR-PRJ-007", test_workspace_schema},
      {"FR-PRJ-008", test_workspace_lifecycle},
      {"FR-PRJ-009",
       [] {
         test_workspace_lifecycle();
         test_current_context();
       }},
      {"FR-PRJ-010", test_current_context},
      {"FR-CORE-101", test_result_and_services},
  };
  const std::string selected = argc == 3 && std::string_view{argv[1]} == "--case" ? argv[2] : "";
  if (!selected.empty() && !tests.contains(selected))
    return 2;
  int failed = 0;
  for (const auto& [name, test] : tests) {
    if (!selected.empty() && selected != name)
      continue;
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
      ++failed;
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
  }
  return failed == 0 ? 0 : 1;
}
