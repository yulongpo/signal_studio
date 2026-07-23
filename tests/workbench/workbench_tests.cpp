#include "signal_studio/workbench/workbench.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

using namespace signal;  // brings core:: and workbench:: into scope

int g_failures = 0;

void check(bool cond, std::string_view msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++g_failures;
  }
}

int case_service_registry() {
  auto reg = signal::workbench::make_service_registry();
  check(reg != nullptr, "service registry created");
  auto svc = std::make_shared<int>(42);
  check(reg->register_service({"compute.selector"}, std::static_pointer_cast<void>(svc)).ok(), "register service");
  auto resolved = reg->resolve({"compute.selector"});
  check(resolved != nullptr, "resolve service");
  check(reg->resolve({"missing"}) == nullptr, "missing resolves null");
  check(!reg->register_service({""}, nullptr).ok(), "empty id rejected");
  return g_failures == 0 ? 0 : 1;
}

int case_command_registry() {
  auto reg = signal::workbench::make_command_registry();
  int counter = 0;
  check(reg->register_command("file.open", [&] {
    ++counter;
    return core::Status::success();
  }).ok(), "register command");
  auto r = reg->invoke("file.open");
  check(r.ok(), "invoke command");
  check(counter == 1, "command action ran");
  auto r2 = reg->invoke("missing");
  check(!r2.ok(), "unknown command fails");
  auto ids = reg->registered_command_ids();
  check(!ids.empty() && ids[0] == "file.open", "registered ids");
  return g_failures == 0 ? 0 : 1;
}

namespace {
class DummyPanel final : public signal::workbench::IPanel {
 public:
  explicit DummyPanel(std::string id, signal::workbench::PanelRegion region)
      : id_(std::move(id)), region_(region) {}
  std::string_view id() const noexcept override { return id_; }
  signal::workbench::PanelRegion region() const noexcept override { return region_; }
  void* native_widget() noexcept override { return nullptr; }

 private:
  std::string id_;
  signal::workbench::PanelRegion region_;
};
}  // namespace

int case_panel_factory() {
  signal::workbench::PanelFactory factory;
  check(factory.register_creator("inspector", [](const signal::workbench::PanelContext& ctx) {
    return core::Result<std::unique_ptr<signal::workbench::IPanel>>(
        std::unique_ptr<signal::workbench::IPanel>(std::make_unique<DummyPanel>(ctx.panel_id, ctx.region)));
  }).ok(), "register creator");
  signal::workbench::PanelContext ctx{"inspector", signal::workbench::PanelRegion::right_dock};
  auto r = factory.create(ctx);
  check(r.ok(), "create panel");
  check((*r)->region() == signal::workbench::PanelRegion::right_dock, "panel region");
  check(!factory.create({"missing", signal::workbench::PanelRegion::center}).ok(), "unknown panel fails");
  check(factory.registered_panel_ids().size() == 1, "one creator registered");
  return g_failures == 0 ? 0 : 1;
}

int case_diagnostics() {
  auto prov = signal::workbench::make_diagnostics_provider();
  check(prov != nullptr, "diagnostics provider created");
  auto* cfg = dynamic_cast<signal::workbench::ConfigurableDiagnosticsProvider*>(prov.get());
  check(cfg != nullptr, "configurable provider");
  cfg->set_compute_info("cuda", "RTX-5060");
  cfg->set_active_panels({"inspector", "task-center"});
  cfg->add_note("oneMKL CPU FFT unavailable");
  auto snap = prov->snapshot();
  check(snap.compute_backend == "cuda", "compute backend recorded");
  check(snap.cuda_device == "RTX-5060", "cuda device recorded");
  check(snap.active_panels.size() == 2, "active panels");
  check(!snap.qt_version.empty(), "qt version non-empty");
  check(!snap.notes.empty(), "notes recorded");
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string_view{argv[1]} != "--case") {
    std::cerr << "usage: workbench_tests --case <name>\n";
    return 2;
  }
  std::string_view name = argv[2];
  if (name == "service-registry") return case_service_registry();
  if (name == "command-registry") return case_command_registry();
  if (name == "panel-factory") return case_panel_factory();
  if (name == "diagnostics") return case_diagnostics();
  std::cerr << "unknown case: " << name << "\n";
  return 2;
}
