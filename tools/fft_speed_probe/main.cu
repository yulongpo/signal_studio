// Same-length wall-clock probe. Libraries are loaded from explicit paths.
// GPU times use CUDA events. CPU times use steady_clock around execute only.
#include <cuda_runtime.h>
#include <cufft.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
double median(std::vector<double> values) {
  if (values.empty()) return -1;
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}
void require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}
struct Row {
  std::string library, kind;
  int n = 0, batch = 1;
  double executeMs = -1, roundTripMs = -1, planMs = -1;
  std::string note;
};

void radix2(std::vector<std::complex<double>>& data) {
  const auto n = data.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(data[i], data[j]);
  }
  for (std::size_t length = 2; length <= n; length <<= 1) {
    const auto root = std::polar(1.0, -2 * 3.14159265358979323846 / static_cast<double>(length));
    for (std::size_t start = 0; start < n; start += length) {
      std::complex<double> phase{1, 0};
      for (std::size_t k = 0; k < length / 2; ++k) {
        const auto even = data[start + k];
        const auto odd = phase * data[start + k + length / 2];
        data[start + k] = even + odd;
        data[start + k + length / 2] = even - odd;
        phase *= root;
      }
    }
  }
}

std::vector<std::complex<double>> signal(int n, int batch) {
  std::vector<std::complex<double>> values(static_cast<std::size_t>(n) * batch);
  for (std::size_t i = 0; i < values.size(); ++i)
    values[i] = {std::sin(0.017 * i), std::cos(0.013 * i)};
  return values;
}

Row timeHandwritten(int n, int repeats) {
  Row row{"handwritten-radix2", "cpu-double", n, 1};
  if (n <= 1 || (n & (n - 1))) { row.note = "power-of-two only"; return row; }
  auto values = signal(n, 1);
  radix2(values);
  std::vector<double> samples;
  for (int i = 0; i < repeats; ++i) {
    values = signal(n, 1);
    const auto start = Clock::now();
    radix2(values);
    samples.push_back(msSince(start));
  }
  row.executeMs = median(samples);
  return row;
}

using FftwPlan = void*;
using FftwComplex = double[2];
using Plan1d = FftwPlan(__cdecl*)(int, FftwComplex*, FftwComplex*, int, unsigned);
using Execute = void(__cdecl*)(FftwPlan);
using Destroy = void(__cdecl*)(FftwPlan);

Row timeFftw(HMODULE library, int n, int repeats) {
  Row row{"fftw-3.3.11", "cpu-double", n, 1};
  auto plan1d = reinterpret_cast<Plan1d>(GetProcAddress(library, "fftw_plan_dft_1d"));
  auto execute = reinterpret_cast<Execute>(GetProcAddress(library, "fftw_execute"));
  auto destroy = reinterpret_cast<Destroy>(GetProcAddress(library, "fftw_destroy_plan"));
  if (!plan1d || !execute || !destroy) { row.note = "missing fftw exports"; return row; }
  std::vector<std::complex<double>> in(n), out(n);
  for (int i = 0; i < n; ++i) in[i] = {std::sin(0.017 * i), std::cos(0.013 * i)};
  const auto planning = Clock::now();
  auto plan = plan1d(n, reinterpret_cast<FftwComplex*>(in.data()),
                      reinterpret_cast<FftwComplex*>(out.data()), -1, 0);
  row.planMs = msSince(planning);
  if (!plan) { row.note = "fftw plan failed"; return row; }
  execute(plan);
  std::vector<double> samples;
  for (int i = 0; i < repeats; ++i) {
    const auto start = Clock::now();
    execute(plan);
    samples.push_back(msSince(start));
  }
  destroy(plan);
  row.executeMs = median(samples);
  return row;
}

using MklCreate = long(__cdecl*)(void**, int, long);
using MklCommit = long(__cdecl*)(void*);
using MklForward = long(__cdecl*)(void*, void*);
using MklFree = long(__cdecl*)(void**);
Row timeMklSingle(HMODULE library, int n, int repeats) {
  Row row{"onemkl-dfti", "cpu-float", n, 1};
  auto create = reinterpret_cast<MklCreate>(GetProcAddress(library, "DftiCreateDescriptor_s_1d"));
  auto commit = reinterpret_cast<MklCommit>(GetProcAddress(library, "DftiCommitDescriptor"));
  auto forward = reinterpret_cast<MklForward>(GetProcAddress(library, "DftiComputeForward"));
  auto freeDesc = reinterpret_cast<MklFree>(GetProcAddress(library, "DftiFreeDescriptor"));
  if (!create || !commit || !forward || !freeDesc) { row.note = "missing single DFTI exports"; return row; }
  void* desc = nullptr;
  const auto planning = Clock::now();
  if (create(&desc, 32, n) != 0 || !desc || commit(desc) != 0) {
    row.note = "single DFTI plan failed";
    if (desc) freeDesc(&desc);
    return row;
  }
  row.planMs = msSince(planning);
  std::vector<std::complex<float>> values(n);
  for (int i = 0; i < n; ++i) values[i] = {std::sin(0.017f * i), std::cos(0.013f * i)};
  require(forward(desc, values.data()) == 0, "single DFTI execute");
  std::vector<double> samples;
  for (int i = 0; i < repeats; ++i) {
    const auto start = Clock::now();
    require(forward(desc, values.data()) == 0, "single DFTI execute");
    samples.push_back(msSince(start));
  }
  freeDesc(&desc);
  row.executeMs = median(samples);
  return row;
}

Row timeMkl(HMODULE library, int n, int repeats) {
  Row row{"onemkl-dfti", "cpu-double", n, 1};
  auto create = reinterpret_cast<MklCreate>(GetProcAddress(library, "DftiCreateDescriptor_d_1d"));
  auto commit = reinterpret_cast<MklCommit>(GetProcAddress(library, "DftiCommitDescriptor"));
  auto forward = reinterpret_cast<MklForward>(GetProcAddress(library, "DftiComputeForward"));
  auto freeDesc = reinterpret_cast<MklFree>(GetProcAddress(library, "DftiFreeDescriptor"));
  if (!create || !commit || !forward || !freeDesc) { row.note = "missing DFTI exports"; return row; }
  void* desc = nullptr;
  const auto planning = Clock::now();
  // DFTI_COMPLEX == 32. Length is the fourth logical argument of the 1d creator.
  if (create(&desc, 32, n) != 0 || !desc || commit(desc) != 0) {
    row.note = "DFTI plan failed";
    if (desc) freeDesc(&desc);
    return row;
  }
  row.planMs = msSince(planning);
  std::vector<std::complex<double>> values(n);
  for (int i = 0; i < n; ++i) values[i] = {std::sin(0.017 * i), std::cos(0.013 * i)};
  require(forward(desc, values.data()) == 0, "DFTI execute");
  std::vector<double> samples;
  for (int i = 0; i < repeats; ++i) {
    const auto start = Clock::now();
    require(forward(desc, values.data()) == 0, "DFTI execute");
    samples.push_back(msSince(start));
  }
  freeDesc(&desc);
  row.executeMs = median(samples);
  return row;
}

float elapsed(cudaEvent_t a, cudaEvent_t b) {
  float value = 0;
  require(cudaEventElapsedTime(&value, a, b) == cudaSuccess, "cuda event");
  return value;
}

Row timeCufft(int n, int batch, cufftType type, int repeats, bool roundTrip) {
  const bool single = type == CUFFT_C2C;
  Row row{"cufft-12.4", single ? "gpu-c2c-f32" : "gpu-z2z-f64", n, batch};
  const auto element = single ? sizeof(cufftComplex) : sizeof(cufftDoubleComplex);
  const auto bytes = static_cast<std::size_t>(n) * batch * element;
  cufftHandle plan = 0;
  const auto planning = Clock::now();
  if (cufftPlanMany(&plan, 1, &n, nullptr, 1, n, nullptr, 1, n, type, batch) != CUFFT_SUCCESS) {
    row.note = "cuFFT plan failed";
    return row;
  }
  row.planMs = msSince(planning);
  void* device = nullptr;
  require(cudaMalloc(&device, bytes) == cudaSuccess, "cudaMalloc");
  auto host = signal(n, batch);
  std::vector<char> packed(bytes);
  if (single) {
    auto* out = reinterpret_cast<cufftComplex*>(packed.data());
    for (std::size_t i = 0; i < host.size(); ++i)
      out[i] = {static_cast<float>(host[i].real()), static_cast<float>(host[i].imag())};
  } else {
    std::memcpy(packed.data(), host.data(), bytes);
  }
  require(cudaMemcpy(device, packed.data(), bytes, cudaMemcpyHostToDevice) == cudaSuccess, "H2D");
  auto exec = [&] {
    if (single) require(cufftExecC2C(plan, static_cast<cufftComplex*>(device),
                                      static_cast<cufftComplex*>(device), CUFFT_FORWARD) == CUFFT_SUCCESS, "C2C");
    else require(cufftExecZ2Z(plan, static_cast<cufftDoubleComplex*>(device),
                              static_cast<cufftDoubleComplex*>(device), CUFFT_FORWARD) == CUFFT_SUCCESS, "Z2Z");
  };
  exec();
  require(cudaDeviceSynchronize() == cudaSuccess, "sync");
  cudaEvent_t start, stop;
  require(cudaEventCreate(&start) == cudaSuccess && cudaEventCreate(&stop) == cudaSuccess, "event");
  std::vector<double> samples;
  for (int i = 0; i < repeats; ++i) {
    require(cudaEventRecord(start) == cudaSuccess, "record");
    exec();
    require(cudaEventRecord(stop) == cudaSuccess, "record");
    require(cudaEventSynchronize(stop) == cudaSuccess, "sync");
    samples.push_back(elapsed(start, stop));
  }
  row.executeMs = median(samples);
  if (roundTrip) {
    std::vector<double> trips;
    for (int i = 0; i < repeats; ++i) {
      require(cudaEventRecord(start) == cudaSuccess, "record");
      require(cudaMemcpy(device, packed.data(), bytes, cudaMemcpyHostToDevice) == cudaSuccess, "H2D");
      exec();
      require(cudaMemcpy(packed.data(), device, bytes, cudaMemcpyDeviceToHost) == cudaSuccess, "D2H");
      require(cudaEventRecord(stop) == cudaSuccess, "record");
      require(cudaEventSynchronize(stop) == cudaSuccess, "sync");
      trips.push_back(elapsed(start, stop));
    }
    row.roundTripMs = median(trips);
  }
  cudaEventDestroy(start);
  cudaEventDestroy(stop);
  cudaFree(device);
  cufftDestroy(plan);
  return row;
}

void print(const Row& row) {
  std::cout << "{\"library\":\"" << row.library << "\",\"kind\":\"" << row.kind
            << "\",\"n\":" << row.n << ",\"batch\":" << row.batch
            << ",\"executeMs\":" << row.executeMs << ",\"roundTripMs\":" << row.roundTripMs
            << ",\"planMs\":" << row.planMs << ",\"note\":\"" << row.note << "\"}\n";
}
}  // namespace

int main() {
  try {
    int device = 0;
    cudaDeviceProp prop{};
    require(cudaGetDeviceProperties(&prop, device) == cudaSuccess, "no CUDA device");
    std::cout << "{\"device\":\"" << prop.name << "\",\"major\":" << prop.major
              << ",\"minor\":" << prop.minor << "}\n";
    const char* fftwPath = std::getenv("SS_FFTW_DLL");
    const char* mklPath = std::getenv("SS_MKL_DLL");
    HMODULE fftw = fftwPath ? LoadLibraryA(fftwPath) : nullptr;
    HMODULE mkl = mklPath ? LoadLibraryA(mklPath) : nullptr;
    if (fftwPath && !fftw) std::cout << "{\"library\":\"fftw-3.3.11\",\"note\":\"LoadLibrary failed\"}\n";
    if (mklPath && !mkl) std::cout << "{\"library\":\"onemkl-dfti\",\"note\":\"LoadLibrary failed\"}\n";
    const int sizes[] = {2048, 8192, 65536, 262144, 10007};
    for (int n : sizes) {
      print(timeHandwritten(n, n >= 262144 ? 2 : 3));
      if (fftw) print(timeFftw(fftw, n, 5));
      if (mkl) {
        print(timeMkl(mkl, n, 5));
        print(timeMklSingle(mkl, n, 5));
      }
      if ((n & (n - 1)) == 0) {
        print(timeCufft(n, 1, CUFFT_C2C, 8, true));
        print(timeCufft(n, 8, CUFFT_C2C, 8, true));
        print(timeCufft(n, 1, CUFFT_Z2Z, 8, true));
        print(timeCufft(n, 8, CUFFT_Z2Z, 8, true));
        if (n == 65536) print(timeCufft(n, 32, CUFFT_Z2Z, 5, true));
      }
    }
    if (fftw) FreeLibrary(fftw);
    if (mkl) FreeLibrary(mkl);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
