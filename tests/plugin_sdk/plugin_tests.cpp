#include "signal_studio/plugin_sdk/algorithm_plugin.hpp"
#include "signal_studio/plugin_sdk/plugin_host.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, std::string_view msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++g_failures;
  }
}

int case_algorithm_rms() {
  using namespace signal::plugin;
  RmsAlgorithmPlugin plugin;
  check(plugin.descriptor().plugin_id == "builtin.rms", "plugin id");
  AlgorithmRequest req;
  req.operation = "rms";
  req.input = signal::data::SignalBuffer::from_real({3.0, 4.0}).view();
  auto r = plugin.run(req);
  check(r.ok(), "rms run ok");
  if (!r.ok()) return 1;
  check(r->scalar_outputs.size() == 1, "rms one scalar output");
  // RMS of {3,4} = sqrt((9+16)/2) = sqrt(12.5) = 3.5355...
  check(std::fabs(r->scalar_outputs[0] - std::sqrt(12.5)) < 1e-9, "rms = sqrt(12.5)");
  return g_failures == 0 ? 0 : 1;
}

int case_algorithm_bad_operation() {
  using namespace signal::plugin;
  RmsAlgorithmPlugin plugin;
  AlgorithmRequest req;
  req.operation = "unknown";
  req.input = signal::data::SignalBuffer::from_real({1.0}).view();
  auto r = plugin.run(req);
  check(!r.ok(), "unknown operation rejected");
  return g_failures == 0 ? 0 : 1;
}

int case_registry_validate() {
  using namespace signal::plugin;
  PluginHost host;
  // Loading a non-existent library must fail cleanly, not crash.
  auto h = host.load(std::filesystem::path("does_not_exist.dll"));
  check(!h.ok(), "missing library rejected");
  auto discovered = host.discover(std::filesystem::path("no_such_dir"));
  check(!discovered.ok(), "missing directory rejected");
  return g_failures == 0 ? 0 : 1;
}

int case_host_load_minimal() {
  using namespace signal::plugin;
  // The minimal_plugin shared library is built into bin/ by the SDK example target.
  const auto candidate = std::filesystem::current_path() / "bin" / "signal_studio_minimal_plugin.dll";
  if (!std::filesystem::exists(candidate)) {
    std::cerr << "SKIP: minimal plugin not built at " << candidate << "\n";
    return 0;
  }
  PluginHost host;
  auto handle = host.load(candidate);
  check(handle.ok(), "load minimal plugin");
  if (!handle.ok()) return 1;
  check(!handle->info().plugin_id.empty(), "plugin id populated");
  auto act = handle->activate();
  check(act.ok(), "activate plugin");
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string_view{argv[1]} != "--case") {
    std::cerr << "usage: plugin_tests --case <name>\n";
    return 2;
  }
  std::string_view name = argv[2];
  if (name == "algorithm-rms") return case_algorithm_rms();
  if (name == "algorithm-bad-operation") return case_algorithm_bad_operation();
  if (name == "registry-validate") return case_registry_validate();
  if (name == "host-load-minimal") return case_host_load_minimal();
  std::cerr << "unknown case: " << name << "\n";
  return 2;
}
