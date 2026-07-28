#pragma once

#include "signal_studio/dsp/analysis.hpp"

#include <cstdint>

namespace signal::dsp::benchmark_internal {

struct PlanCacheCounters final {
  std::uint64_t hits{};
  std::uint64_t misses{};
  std::uint64_t entries{};
};

// 仅供仓库内性能专项使用，不属于安装后的公共 SDK。
[[nodiscard]] core::Result<PlanCacheCounters> inspect_mkl_plan_cache(IFftBackend& backend);

} // namespace signal::dsp::benchmark_internal
