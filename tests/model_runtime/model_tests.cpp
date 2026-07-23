#include "signal_studio/model_runtime/model_runtime.hpp"

#include <iostream>
#include <string_view>

namespace {

int g_failures = 0;

void check(bool cond, std::string_view msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++g_failures;
  }
}

int case_registry_install_resolve() {
  using namespace signal::model;
  ModelRegistry registry;
  ModelInfo info;
  info.model_id = "demo.v1";
  info.version_major = 1;
  info.runtime = "onnxruntime";
  info.input_names = {"x"};
  info.output_names = {"y"};
  check(registry.install(info).ok(), "install ok");
  check(!registry.install(info).ok(), "duplicate install rejected");
  auto resolved = registry.resolve("demo.v1");
  check(resolved.has_value() && resolved->model_id == "demo.v1", "resolve installed");
  check(!registry.resolve("missing").has_value(), "missing resolves null");
  check(registry.list().size() == 1, "list size 1");
  return g_failures == 0 ? 0 : 1;
}

int case_null_session_unavailable() {
  using namespace signal::model;
  auto session = make_inference_session();
  check(session != nullptr, "session created");
  check(!session->available(), "session unavailable (no ONNX Runtime)");
  InferenceRequest req;
  req.model_id = "demo.v1";
  auto r = session->run(req);
  check(!r.ok(), "run rejected without ONNX Runtime");
  check(session->runtime_name() == "null", "runtime name null");
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string_view{argv[1]} != "--case") {
    std::cerr << "usage: model_tests --case <name>\n";
    return 2;
  }
  std::string_view name = argv[2];
  if (name == "registry-install-resolve") return case_registry_install_resolve();
  if (name == "null-session-unavailable") return case_null_session_unavailable();
  std::cerr << "unknown case: " << name << "\n";
  return 2;
}
