#define SS_GPU_DSP_MODULE_ABI_ONLY
#include "infrastructure/gpu_dsp.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <iostream>
#include <vector>

double msSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  auto* module = LoadLibraryA(argv[1]);
  if (!module) return 3;
  using GetApi = const signalstudio::gpu_dsp::module_abi::Api* (*)(unsigned);
  auto* api = reinterpret_cast<GetApi>(GetProcAddress(module, "ssGpuDspGetApi"))(
      signalstudio::gpu_dsp::module_abi::version);
  if (!api || !api->select || !api->fir) return 4;
  signalstudio::gpu_dsp::module_abi::Options options{};
  signalstudio::gpu_dsp::module_abi::Info info{};
  signalstudio::gpu_dsp::module_abi::Failure failure{};
  void* session = api->create(&options, &info, &failure);
  if (!session) { std::cerr << failure.detail << '\n'; return 5; }

  constexpr int n = 1 << 20;
  std::vector<float> values(n), sorted(n);
  for (int i = 0; i < n; ++i) values[i] = std::sin(0.001f * i) * ((i % 17) - 8);
  sorted = values;
  const auto cpuSort = std::chrono::steady_clock::now();
  std::sort(sorted.begin(), sorted.end());
  const auto cpuSortMs = msSince(cpuSort);
  unsigned long long ranks[2] = {n / 100, n / 2};
  float out[2]{};
  if (!api->select(session, values.data(), n, ranks, 2, out, &failure)) {
    std::cerr << failure.detail << '\n'; return 6;
  }
  const auto gpuSort = std::chrono::steady_clock::now();
  for (int i = 0; i < 5; ++i)
    if (!api->select(session, values.data(), n, ranks, 2, out, &failure)) return 6;
  const auto gpuSortMs = msSince(gpuSort) / 5;
  if (out[0] != sorted[ranks[0]] || out[1] != sorted[ranks[1]]) {
    std::cerr << "quantile mismatch\n"; return 7;
  }

  constexpr unsigned k = 4096, radius = 8, count = 2048, rawCount = count + 2 * radius;
  std::vector<float> coefficients(k, 0.f);
  coefficients[radius] = 1.f;
  signalstudio::gpu_dsp::FirFilter filter{};
  if (!api->prepare(session, coefficients.data(), k, radius, &filter, &failure)) {
    std::cerr << failure.detail << '\n'; return 8;
  }
  std::vector<std::complex<float>> raw(rawCount, {0.1f, -0.2f});
  std::vector<float> energy(count);
  if (!api->fir(session, &filter, raw.data(), rawCount, 0, 100000, 20e6, 1000, count,
                energy.data(), nullptr, &failure)) {
    std::cerr << failure.detail << '\n'; return 9;
  }
  const auto gpuFir = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    if (!api->fir(session, &filter, raw.data(), rawCount, i, 100000, 20e6, 1000, count,
                  energy.data(), nullptr, &failure)) return 9;
  }
  const auto gpuFirMs = msSince(gpuFir) / 20;
  api->destroy(session);
  std::cout << "order statistic\t1048576\tstd::sort\t" << cpuSortMs << "\n";
  std::cout << "order statistic\t1048576\tCUB\t" << gpuSortMs << "\n";
  std::cout << "order speedup\t\t\t" << (cpuSortMs / gpuSortMs) << "\n";
  std::cout << "periodic FIR replay\t4096 graph launch\tCUDA graph\t" << gpuFirMs << "\n";
  return 0;
}
