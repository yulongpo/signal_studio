#include <cufftdx.hpp>
#include <cuda_runtime.h>
#include <iostream>
#include <vector>

template <unsigned N, unsigned Ept>
int bench() {
  using namespace cufftdx;
  using FFT = decltype(Size<N>() + Precision<float>() + Type<fft_type::c2c>() +
                       Direction<fft_direction::forward>() + SM<890>() + Block() +
                       ElementsPerThread<Ept>() + FFTsPerBlock<1>());
  using Value = typename FFT::value_type;
  Value* data = nullptr;
  if (cudaMalloc(&data, sizeof(Value) * N) != cudaSuccess) return 2;
  auto kernel = [] __global__ (Value* values) {
    Value local[FFT::storage_size];
    unsigned index = threadIdx.x;
    for (unsigned i = 0; i < FFT::elements_per_thread; ++i) {
      local[i] = values[index];
      index += FFT::stride;
    }
    FFT().execute(local);
    index = threadIdx.x;
    for (unsigned i = 0; i < FFT::elements_per_thread; ++i) {
      values[index] = local[i];
      index += FFT::stride;
    }
  };
  kernel<<<1, FFT::block_dim>>>(data);
  if (cudaDeviceSynchronize() != cudaSuccess) return 3;
  cudaEvent_t a, b;
  cudaEventCreate(&a);
  cudaEventCreate(&b);
  constexpr int repeats = 20;
  cudaEventRecord(a);
  for (int i = 0; i < repeats; ++i) kernel<<<1, FFT::block_dim>>>(data);
  cudaEventRecord(b);
  cudaEventSynchronize(b);
  float ms = 0;
  cudaEventElapsedTime(&ms, a, b);
  std::cout << "{\"library\":\"cufftdx-1.6.1\",\"kind\":\"gpu-c2c-f32\",\"n\":" << N
            << ",\"batch\":1,\"executeMs\":" << (ms / repeats) << "}\n";
  cudaFree(data);
  return 0;
}

int main() {
  int status = bench<2048, 8>();
  if (status) return status;
  return bench<8192, 16>();
}
