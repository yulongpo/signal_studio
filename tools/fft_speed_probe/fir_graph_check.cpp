#define SS_GPU_DSP_MODULE_ABI_ONLY
#include "infrastructure/gpu_dsp.h"

#include <windows.h>

#include <cmath>
#include <complex>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  auto* module = LoadLibraryA(argv[1]);
  if (!module) return 3;
  using GetApi = const signalstudio::gpu_dsp::module_abi::Api* (*)(unsigned);
  auto* api = reinterpret_cast<GetApi>(GetProcAddress(module, "ssGpuDspGetApi"))(
      signalstudio::gpu_dsp::module_abi::version);
  if (!api) return 4;
  signalstudio::gpu_dsp::module_abi::Options options{};
  signalstudio::gpu_dsp::module_abi::Info info{};
  signalstudio::gpu_dsp::module_abi::Failure failure{};
  void* session = api->create(&options, &info, &failure);
  if (!session) { std::cerr << failure.detail << '\n'; return 5; }
  constexpr unsigned k = 256, radius = 1, count = 8, rawCount = count + 2 * radius;
  std::vector<float> coefficients(k, 0.f);
  coefficients[radius] = 1.f;
  signalstudio::gpu_dsp::FirFilter filter{};
  if (!api->prepare(session, coefficients.data(), k, radius, &filter, &failure)) {
    std::cerr << failure.stage << ' ' << failure.detail << '\n'; return 6;
  }
  auto run = [&](double fc, int impulse) {
    std::vector<std::complex<float>> raw(rawCount);
    raw[radius + impulse] = {1.f, 0.f};
    std::vector<float> energy(count);
    std::vector<std::complex<float>> tap(count);
    if (!api->fir(session, &filter, raw.data(), rawCount, 0, 100000, 1.0, fc, count,
                  energy.data(), tap.data(), &failure)) {
      std::cerr << failure.stage << ' ' << failure.detail << '\n';
      return false;
    }
    const double tau = 6.28318530717958647692;
    const unsigned local = radius + impulse;
    double phase = std::fmod(-tau * fc * local, tau);
    if (phase < 0) phase += tau;
    const double rounded = static_cast<double>(static_cast<float>(phase));
    const std::complex<float> expected{static_cast<float>(std::cos(rounded)),
                                        static_cast<float>(std::sin(rounded))};
    const auto got = tap[impulse];
    if (std::abs(got - expected) > 1e-4f || std::abs(energy[impulse] - 1.f) > 1e-4f) {
      std::cerr << "graph mismatch fc=" << fc << " got " << got.real() << "," << got.imag()
                << " expected " << expected.real() << "," << expected.imag()
                << " energy " << energy[impulse] << '\n';
      return false;
    }
    return true;
  };
  const bool first = run(0.0, 3);
  const bool second = run(0.125, 4);
  api->destroy(session);
  if (!first || !second) return 7;
  std::cout << "CUDA graph FIR replay matched the center-tap reference\n";
  return 0;
}
