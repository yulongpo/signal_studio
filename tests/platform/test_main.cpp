#include "signal_studio/compute/module.hpp"
#include "signal_studio/core/capability.hpp"
#include "signal_studio/core/error.hpp"
#include "signal_studio/core/module_descriptor.hpp"
#include "signal_studio/core/version.hpp"
#include "signal_studio/data/module.hpp"
#include "signal_studio/dataset/module.hpp"
#include "signal_studio/dsp/module.hpp"
#include "signal_studio/model_runtime/module.hpp"
#include "signal_studio/plugin_sdk/module.hpp"
#include "signal_studio/task_runtime/module.hpp"
#if SIGNAL_STUDIO_TEST_HAS_UI
#include "signal_studio/visualization/module.hpp"
#include "signal_studio/workbench/module.hpp"
#endif

#include <algorithm>
#include <array>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using signal::core::ModuleDescriptor;
using signal::core::ModuleId;

void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string{message});
}

template <typename Callable>
void require_invalid_argument(Callable&& callable, std::string_view message) {
  try {
    std::forward<Callable>(callable)();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error(std::string{message});
}

std::vector<std::reference_wrapper<const ModuleDescriptor>> descriptors() {
  std::vector<std::reference_wrapper<const ModuleDescriptor>> result{
      std::cref(signal::core::module_descriptor()), std::cref(signal::compute::module_descriptor()),
      std::cref(signal::data::module_descriptor()), std::cref(signal::task::module_descriptor()),
      std::cref(signal::dsp::module_descriptor()), std::cref(signal::model::module_descriptor()),
      std::cref(signal::dataset::module_descriptor()), std::cref(signal::plugin::module_descriptor())};
#if SIGNAL_STUDIO_TEST_HAS_UI
  result.push_back(std::cref(signal::visualization::module_descriptor()));
  result.push_back(std::cref(signal::workbench::module_descriptor()));
#endif
  return result;
}

void test_version() {
  const auto& info = signal::core::build_info();
  require(info.product == "Signal Processing Platform", "core branding is not product-neutral");
  require(info.version == signal::core::SemanticVersion{1, 0, 0}, "platform version changed");
  require(!info.compiler.empty() && !info.source_revision.empty(), "build metadata missing");
}

void test_error_contract() {
  using namespace signal::core;
  const std::vector<std::pair<ErrorDomain, std::string_view>> stable_domains{
      {ErrorDomain::core, "SS-CORE-E004"}, {ErrorDomain::data, "SS-DATA-E004"},
      {ErrorDomain::dsp, "SS-DSP-E004"}, {ErrorDomain::compute, "SS-COMPUTE-E004"},
      {ErrorDomain::task_runtime, "SS-TASK-E004"}, {ErrorDomain::visualization, "SS-VIS-E004"},
      {ErrorDomain::workbench, "SS-WB-E004"}, {ErrorDomain::plugin_sdk, "SS-PLG-E004"},
      {ErrorDomain::model_runtime, "SS-MODEL-E004"}, {ErrorDomain::dataset, "SS-DSET-E004"}};
  for (const auto& [domain, text] : stable_domains) {
    require(ErrorCode{domain, ErrorReason::internal_failure}.stable_text() == text, "stable domain code changed");
  }
  ErrorDetails details{ErrorCategory::resource, ErrorSeverity::error, "Samples unavailable", "mapping failed",
                       {"Select a readable source", "Retry the task"}, true, "task-7", "object-3", "data-v2",
                       {{{ErrorDomain::data, ErrorReason::unavailable}, ErrorCategory::resource,
                         "source was removed"}}, {"metric://read/7"}};
  const auto status = Status::failure({ErrorDomain::data, ErrorReason::unavailable}, details);
  require(status.code().stable_text() == "SS-DATA-E002", "stable BL1.0 code changed");
  require(validate_error(status.code(), status.details()).ok(), "structured error rejected");
  require(status.details().retryable && status.details().task_id == "task-7", "error metadata lost");
  ErrorDetails incomplete;
  incomplete.user_message = "Incomplete";
  require(!validate_error({ErrorDomain::core, ErrorReason::invalid_argument}, incomplete), "missing recovery accepted");
}

void test_error_invariants() {
  using namespace signal::core;
  const auto valid_details = [] {
    ErrorDetails details;
    details.category = ErrorCategory::contract;
    details.severity = ErrorSeverity::error;
    details.user_message = "Invalid request";
    details.recovery_actions = {"Correct the request"};
    return details;
  };

  require_invalid_argument([&] {
    (void)Status::failure({static_cast<ErrorDomain>(99), ErrorReason::invalid_argument}, valid_details());
  }, "invalid domain survived factory validation");
  require_invalid_argument([&] {
    (void)Status::failure({ErrorDomain::core, static_cast<ErrorReason>(99)}, valid_details());
  }, "invalid reason survived factory validation");
  require_invalid_argument([&] {
    (void)Status::failure({ErrorDomain::none, ErrorReason::invalid_argument}, valid_details());
  }, "malformed domain/reason pair survived factory validation");
  require_invalid_argument([&] {
    (void)Status::failure({ErrorDomain::core, ErrorReason::none}, valid_details());
  }, "missing stable reason survived factory validation");
  require_invalid_argument([&] {
    auto details = valid_details();
    details.category = static_cast<ErrorCategory>(0);
    (void)Status::failure({ErrorDomain::core, ErrorReason::invalid_argument}, std::move(details));
  }, "invalid category enum survived factory validation");
  require_invalid_argument([&] {
    auto details = valid_details();
    details.category = ErrorCategory::resource;
    (void)Status::failure({ErrorDomain::core, ErrorReason::invalid_argument}, std::move(details));
  }, "reason/category mismatch survived factory validation");
  require_invalid_argument([&] {
    auto details = valid_details();
    details.severity = static_cast<ErrorSeverity>(0);
    (void)Status::failure({ErrorDomain::core, ErrorReason::invalid_argument}, std::move(details));
  }, "invalid severity survived factory validation");
  require_invalid_argument([&] {
    auto details = valid_details();
    details.retryable = true;
    (void)Status::failure({ErrorDomain::core, ErrorReason::invalid_argument}, std::move(details));
  }, "invalid retryability survived factory validation");
  require_invalid_argument([&] {
    auto details = valid_details();
    details.recovery_actions = {""};
    (void)Status::failure({ErrorDomain::core, ErrorReason::invalid_argument}, std::move(details));
  }, "empty recovery action survived factory validation");
  require_invalid_argument([&] {
    auto details = valid_details();
    details.cause_chain.push_back(
        {{ErrorDomain::data, ErrorReason::unavailable}, ErrorCategory::contract, "missing source"});
    (void)Status::failure({ErrorDomain::core, ErrorReason::invalid_argument}, std::move(details));
  }, "nested cause category mismatch survived factory validation");
  require_invalid_argument([&] {
    auto details = valid_details();
    details.cause_chain.push_back(
        {{static_cast<ErrorDomain>(77), ErrorReason::unavailable}, ErrorCategory::resource, "bad domain"});
    (void)Status::failure({ErrorDomain::core, ErrorReason::invalid_argument}, std::move(details));
  }, "invalid nested cause code survived factory validation");
  require_invalid_argument([&] {
    auto details = valid_details();
    for (std::size_t index = 0; index <= max_error_cause_depth; ++index) {
      details.cause_chain.push_back(
          {{ErrorDomain::data, ErrorReason::unavailable}, ErrorCategory::resource, "nested failure"});
    }
    (void)Status::failure({ErrorDomain::core, ErrorReason::invalid_argument}, std::move(details));
  }, "excessive nested cause depth survived factory validation");
}

void test_error_serialization() {
  using namespace signal::core;
  ErrorDetails details{ErrorCategory::contract, ErrorSeverity::warning, "Bad \"range\"", "end < begin",
                       {"Correct the range"}, false, "task-1", "object-1", "data-v1",
                       {{{ErrorDomain::data, ErrorReason::unavailable}, ErrorCategory::resource,
                         "source mapping unavailable"}}, {"metric://range"}};
  const auto json = Status::failure({ErrorDomain::core, ErrorReason::invalid_argument}, details).serialize_json();
  require(json.find("SS-CORE-E001") != std::string::npos, "serialized stable code missing");
  require(json.find("\\\"range\\\"") != std::string::npos, "JSON escaping failed");
  require(json.find("dataSourceVersionId") != std::string::npos && json.find("metricsRef") != std::string::npos,
          "required fields missing from serialization");
  require(json.find("\"category\":\"resource\"") != std::string::npos &&
              json.find("unknown") == std::string::npos,
          "validated cause code/category did not serialize stably");
}

void test_error_propagation() {
  using namespace signal::core;
  const auto root = Status::failure({ErrorDomain::dsp, ErrorReason::internal_failure}, "FFT failed", "backend error");
  require(root.details().category == ErrorCategory::adapter, "internal failure category mapping changed");
  auto propagated = root.with_context("PSD task");
  require(propagated.diagnostic() == "PSD task: backend error", "context not propagated");
  require(propagated.details().cause_chain.size() == 1, "cause chain not retained");
  require(propagated.code().stable_text() == "SS-DSP-E004", "propagation changed stable code");

  propagated = root;
  for (std::size_t attempt = 1; attempt <= 9; ++attempt) {
    propagated = propagated.with_context("context-" + std::to_string(attempt));
    const auto expected_depth = std::min(attempt, max_error_cause_depth);
    require(propagated.details().cause_chain.size() == expected_depth, "propagation exceeded bounded cause depth");
    require(validate_error(propagated.code(), propagated.details()).ok(), "bounded propagated status became invalid");
  }
  require(propagated.details().cause_chain.front().technical_details == "backend error",
          "bounded propagation discarded the root cause");
  require(propagated.details().cause_chain.back().technical_details.starts_with("context-8:"),
          "bounded propagation did not retain the most recent prior context");
  require(propagated.diagnostic().starts_with("context-9:"), "latest propagation context was not retained");
}

void test_capabilities() {
  using namespace signal::core;
  CapabilityRegistry registry;
  require(is_known_capability_availability(CapabilityAvailability::unavailable) &&
              is_known_capability_availability(CapabilityAvailability::available) &&
              is_known_capability_availability(CapabilityAvailability::degraded) &&
              !is_known_capability_availability(static_cast<CapabilityAvailability>(99)),
          "capability availability known-value predicate is incomplete");
  require(registry.register_capability({"compute.cpu", CapabilityAvailability::available, "portable-cpu", "C++20"}).ok(),
          "registration failed");
  require(registry.is_available("compute.cpu") && !registry.is_available("compute.cuda"), "availability incorrect");
  require(!registry.register_capability({"compute.cpu", CapabilityAvailability::available, "duplicate", {}}),
          "duplicate accepted");
  const auto before_invalid = registry.snapshot();
  require(!registry.register_capability(
              {"compute.invalid", static_cast<CapabilityAvailability>(99), "invalid-provider", {}}),
          "invalid availability accepted");
  require(registry.snapshot().size() == before_invalid.size() && !registry.find("compute.invalid"),
          "registry changed after invalid availability rejection");
}

void test_module_validation() {
  using namespace signal::core;
  require(is_known_module_id(ModuleId::core) && is_known_module_id(ModuleId::workbench) &&
              !is_known_module_id(static_cast<ModuleId>(0)) && !is_known_module_id(static_cast<ModuleId>(99)),
          "module id known-value predicate is incomplete");

  const ModuleDescriptor empty_capability_contract{
      ModuleId::core, "SignalStudio::Core", "signal::core", {1, 0, 0}, {}, {}};
  require(validate_module_descriptor(empty_capability_contract).ok(),
          "an empty capability collection was rejected without a BL1.0 requirement");

  auto invalid_descriptor = empty_capability_contract;
  invalid_descriptor.id = static_cast<ModuleId>(99);
  require(!validate_module_descriptor(invalid_descriptor), "invalid descriptor id accepted");

  const std::array invalid_dependencies{static_cast<ModuleId>(99)};
  invalid_descriptor = empty_capability_contract;
  invalid_descriptor.dependencies = invalid_dependencies;
  require(!validate_module_descriptor(invalid_descriptor), "invalid dependency id accepted");
}

void test_module(ModuleId id) {
  const auto all = descriptors();
  const auto found = std::ranges::find_if(all, [id](const auto& item) { return item.get().id == id; });
  require(found != all.end(), "module descriptor missing");
  const auto& descriptor = found->get();
  require(signal::core::validate_module_descriptor(descriptor).ok(), "module contract invalid");
  require(descriptor.api_version == signal::core::SemanticVersion{1, 0, 0}, "module compatibility version changed");
  require(descriptor.cmake_target.starts_with("SignalStudio::"), "installed target namespace changed");
  require(!descriptor.capabilities.empty(), "module capability contract empty");
}

void test_dependency_graph() {
  const std::map<ModuleId, std::vector<ModuleId>> expected{
      {ModuleId::core, {}}, {ModuleId::compute, {ModuleId::core}}, {ModuleId::data, {ModuleId::core}},
      {ModuleId::task_runtime, {ModuleId::compute, ModuleId::core}},
      {ModuleId::dsp, {ModuleId::data, ModuleId::compute, ModuleId::core}},
      {ModuleId::model_runtime, {ModuleId::data, ModuleId::compute, ModuleId::task_runtime, ModuleId::core}},
      {ModuleId::dataset, {ModuleId::data, ModuleId::task_runtime, ModuleId::core}},
      {ModuleId::plugin_sdk, {ModuleId::data, ModuleId::task_runtime, ModuleId::core}},
#if SIGNAL_STUDIO_TEST_HAS_UI
      {ModuleId::visualization, {ModuleId::data, ModuleId::task_runtime, ModuleId::core}},
      {ModuleId::workbench, {ModuleId::visualization, ModuleId::task_runtime, ModuleId::core}},
#endif
  };
  const auto all = descriptors();
  require(all.size() == expected.size(), "unexpected module count");
  for (const auto& item : all) {
    require(std::ranges::equal(item.get().dependencies, expected.at(item.get().id)), "approved DAG changed");
  }
  std::set<ModuleId> visited;
  std::set<ModuleId> active;
  const std::function<void(ModuleId)> visit = [&](ModuleId id) {
    require(!active.contains(id), "dependency cycle detected");
    if (visited.contains(id)) return;
    active.insert(id);
    for (const auto dependency : expected.at(id)) visit(dependency);
    active.erase(id);
    visited.insert(id);
  };
  for (const auto& [id, unused] : expected) { (void)unused; visit(id); }
}
}  // namespace

int main(int argc, char** argv) {
  using Test = std::function<void()>;
  const std::map<std::string, Test> tests{
      {"core.version", test_version}, {"core.errors.contract", test_error_contract},
      {"core.errors.invariants", test_error_invariants},
      {"core.errors.serialization", test_error_serialization}, {"core.errors.propagation", test_error_propagation},
      {"core.capabilities", test_capabilities}, {"core.module-descriptor-validation", test_module_validation},
      {"graph.compatibility", test_dependency_graph},
      {"module.core", [] { test_module(ModuleId::core); }}, {"module.compute", [] { test_module(ModuleId::compute); }},
      {"module.data", [] { test_module(ModuleId::data); }}, {"module.task-runtime", [] { test_module(ModuleId::task_runtime); }},
      {"module.dsp", [] { test_module(ModuleId::dsp); }}, {"module.model-runtime", [] { test_module(ModuleId::model_runtime); }},
      {"module.dataset", [] { test_module(ModuleId::dataset); }}, {"module.plugin-sdk", [] { test_module(ModuleId::plugin_sdk); }},
#if SIGNAL_STUDIO_TEST_HAS_UI
      {"module.visualization", [] { test_module(ModuleId::visualization); }},
      {"module.workbench", [] { test_module(ModuleId::workbench); }},
#endif
  };
  const std::string selected = argc == 3 && std::string_view{argv[1]} == "--case" ? argv[2] : "";
  int failed = 0;
  for (const auto& [name, function] : tests) {
    if (!selected.empty() && selected != name) continue;
    try { function(); std::cout << "[PASS] " << name << '\n'; }
    catch (const std::exception& error) { ++failed; std::cerr << "[FAIL] " << name << ": " << error.what() << '\n'; }
  }
  if (!selected.empty() && !tests.contains(selected)) return 2;
  return failed == 0 ? 0 : 1;
}
