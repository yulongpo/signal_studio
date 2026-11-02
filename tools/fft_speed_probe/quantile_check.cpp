#define SS_GPU_DSP_MODULE_ABI_ONLY
#include "infrastructure/gpu_dsp.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  auto* module = LoadLibraryA(argv[1]);
  if (!module) return 3;
  using GetApi = const signalstudio::gpu_dsp::module_abi::Api* (*)(unsigned);
  auto get = reinterpret_cast<GetApi>(GetProcAddress(module, "ssGpuDspGetApi"));
  auto* api = get ? get(signalstudio::gpu_dsp::module_abi::version) : nullptr;
  if (!api || !api->select) return 4;
  signalstudio::gpu_dsp::module_abi::Options options{};
  options.limits = {};
  options.device = 0;
  signalstudio::gpu_dsp::module_abi::Info info{};
  signalstudio::gpu_dsp::module_abi::Failure failure{};
  void* session = api->create(&options, &info, &failure);
  if (!session) {
    std::cerr << failure.stage << " " << failure.detail << '\n';
    return 5;
  }
  std::vector<float> values;
  for (int i = 0; i < 10007; ++i) values.push_back(std::sin(0.01f * i) * ((i % 5) - 2));
  std::vector<float> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  unsigned long long ranks[4] = {0, 100, 5003, 10006};
  float out[4]{};
  if (!api->select(session, values.data(), static_cast<unsigned>(values.size()), ranks, 4, out, &failure)) {
    std::cerr << failure.stage << " " << failure.detail << '\n';
    api->destroy(session);
    return 6;
  }
  for (int i = 0; i < 4; ++i) {
    if (out[i] != sorted[ranks[i]]) {
      std::cerr << "mismatch " << i << " " << out[i] << " " << sorted[ranks[i]] << '\n';
      api->destroy(session);
      return 7;
    }
  }
  api->destroy(session);
  std::cout << "CUB order statistics matched host sort\n";
  return 0;
}
