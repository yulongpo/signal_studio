#include "signal_studio/core/capability.hpp"

#include <chrono>
#include <iostream>

int main() {
  using Clock = std::chrono::steady_clock;
  signal::core::CapabilityRegistry registry;
  if (!registry.register_capability({"compute.cpu", signal::core::CapabilityAvailability::available, "cpu", "smoke"})) return 1;
  constexpr int iterations = 250'000;
  const auto start = Clock::now();
  int hits = 0;
  for (int index = 0; index < iterations; ++index) hits += registry.is_available("compute.cpu") ? 1 : 0;
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
  std::cout << "benchmark_smoke iterations=" << iterations << " elapsed_ms=" << elapsed.count()
            << " threshold_ms=5000 classification=non-release-regression-guard\n";
  return hits == iterations && elapsed < std::chrono::seconds{5} ? 0 : 2;
}
