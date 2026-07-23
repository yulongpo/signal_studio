#include "signal_studio/compute/backend.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int g_failures = 0;

void check(bool cond, std::string_view msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++g_failures;
  }
}

template <typename T>
void check_eq(const T& a, const T& b, std::string_view msg) {
  if (!(a == b)) {
    std::cerr << "FAIL: " << msg << " (mismatch)\n";
    ++g_failures;
  }
}

int case_cpu_backend_capabilities() {
  signal::compute::CpuComputeBackend backend;
  auto caps = backend.capabilities();
  check(caps.available, "cpu backend should be available");
  check(caps.device == signal::compute::ComputeDeviceType::cpu, "cpu device type");
  check(caps.compute_units > 0, "cpu reports positive compute units");
  auto prov = backend.provenance();
  check(prov.device == signal::compute::ComputeDeviceType::cpu, "provenance device");
  check(!prov.backend_id.empty(), "provenance backend id non-empty");
  return g_failures == 0 ? 0 : 1;
}

int case_buffer_pool_budget() {
  signal::compute::BoundedBufferPool pool(1024);
  signal::compute::BufferSpec spec{64, sizeof(double), signal::compute::ComputeDeviceType::cpu};
  auto r1 = pool.acquire(spec);
  check(r1.ok() && r1->valid(), "first acquire within budget");
  check_eq(pool.acquired_bytes(), std::uint64_t{64 * 8}, "acquired bytes tracked");
  // Acquire enough to exceed budget.
  signal::compute::BufferSpec big{200, sizeof(double), signal::compute::ComputeDeviceType::cpu};
  auto r2 = pool.acquire(big);
  check(!r2.ok(), "over-budget acquire rejected");
  return g_failures == 0 ? 0 : 1;
}

int case_buffer_pool_zero_rejected() {
  signal::compute::BoundedBufferPool pool(1024);
  signal::compute::BufferSpec zero{0, sizeof(double), signal::compute::ComputeDeviceType::cpu};
  auto r = pool.acquire(zero);
  check(!r.ok(), "zero-element acquire rejected");
  return g_failures == 0 ? 0 : 1;
}

int case_selector_cpu_fallback() {
  signal::compute::AutoBackendSelector selector;
  check(!selector.has_cuda(), "no cuda registered initially");
  signal::compute::Workload wl;
  wl.work_class = signal::compute::WorkloadClass::dsp_bound;
  wl.benefits_from_gpu = true;
  auto sel = selector.select(wl);
  check(sel.ok(), "selection succeeds");
  check(sel->first == signal::compute::ComputeDeviceType::cpu, "falls back to cpu without cuda");
  return g_failures == 0 ? 0 : 1;
}

int case_selector_gpu_required_missing() {
  signal::compute::AutoBackendSelector selector;
  signal::compute::Workload wl;
  wl.work_class = signal::compute::WorkloadClass::dsp_bound;
  wl.requires_gpu = true;
  auto sel = selector.select(wl);
  check(!sel.ok(), "gpu-required selection fails without cuda backend");
  return g_failures == 0 ? 0 : 1;
}

int case_provenance_recorded() {
  signal::compute::CpuComputeBackend backend;
  auto prov = backend.provenance();
  check(!prov.backend_id.empty(), "backend_id recorded");
  check(!prov.runtime_version.empty(), "runtime_version recorded");
  check(!prov.device_name.empty(), "device_name recorded");
  return g_failures == 0 ? 0 : 1;
}

int case_cuda_backend_probe() {
  auto backend = signal::compute::make_cuda_compute_backend();
  if (!backend.ok()) {
    std::cerr << "SKIP: cuda backend unavailable in this environment\n";
    return 0;  // environment deviation, not a failure
  }
  auto caps = (*backend)->capabilities();
  check(caps.available, "cuda backend available");
  check(caps.device == signal::compute::ComputeDeviceType::cuda, "cuda device type");
  check(caps.compute_units > 0, "cuda reports positive SM count");
  check(!caps.device_name.empty(), "cuda device name non-empty");
  return g_failures == 0 ? 0 : 1;
}

int case_selector_cuda_preferred() {
  signal::compute::AutoBackendSelector selector;
  auto cuda = signal::compute::make_cuda_compute_backend();
  if (!cuda.ok()) {
    std::cerr << "SKIP: cuda backend unavailable in this environment\n";
    return 0;
  }
  selector.register_backend(std::move(*cuda));
  check(selector.has_cuda(), "cuda registered");
  signal::compute::Workload wl;
  wl.work_class = signal::compute::WorkloadClass::dsp_bound;
  wl.benefits_from_gpu = true;
  auto sel = selector.select(wl);
  check(sel.ok(), "selection succeeds");
  check(sel->first == signal::compute::ComputeDeviceType::cuda, "cuda preferred for dsp workload");
  signal::compute::Workload io_wl;
  io_wl.work_class = signal::compute::WorkloadClass::io_bound;
  io_wl.benefits_from_gpu = true;
  auto io_sel = selector.select(io_wl);
  check(io_sel->first == signal::compute::ComputeDeviceType::cpu, "io-bound stays on cpu");
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string_view{argv[1]} != "--case") {
    std::cerr << "usage: compute_tests --case <name>\n";
    return 2;
  }
  std::string_view name = argv[2];
  if (name == "cpu-backend-capabilities") return case_cpu_backend_capabilities();
  if (name == "buffer-pool-budget") return case_buffer_pool_budget();
  if (name == "buffer-pool-zero-rejected") return case_buffer_pool_zero_rejected();
  if (name == "selector-cpu-fallback") return case_selector_cpu_fallback();
  if (name == "selector-gpu-required-missing") return case_selector_gpu_required_missing();
  if (name == "provenance-recorded") return case_provenance_recorded();
  if (name == "cuda-backend-probe") return case_cuda_backend_probe();
  if (name == "selector-cuda-preferred") return case_selector_cuda_preferred();
  std::cerr << "unknown case: " << name << "\n";
  return 2;
}
