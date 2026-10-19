// Native TensorRT/CUDA parity probe. Uses only staged official SDK resources.

#include <NvInfer.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class Logger final : public nvinfer1::ILogger {
 public:
  void log(Severity severity, const char* message) noexcept override {
    if (severity <= Severity::kWARNING) std::cerr << message << '\n';
  }
};

void CheckCuda(cudaError_t status, const char* operation) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string(operation) + ": " +
                             cudaGetErrorString(status));
  }
}

struct CudaMemoryDeleter {
  void operator()(void* memory) const noexcept { cudaFree(memory); }
};

using DeviceMemory = std::unique_ptr<void, CudaMemoryDeleter>;

DeviceMemory AllocateDevice(std::size_t bytes) {
  void* memory = nullptr;
  CheckCuda(cudaMalloc(&memory, bytes), "cudaMalloc");
  return DeviceMemory(memory);
}

class CudaStream {
 public:
  CudaStream() { CheckCuda(cudaStreamCreate(&stream_), "cudaStreamCreate"); }
  ~CudaStream() { cudaStreamDestroy(stream_); }
  CudaStream(const CudaStream&) = delete;
  CudaStream& operator=(const CudaStream&) = delete;
  cudaStream_t get() const { return stream_; }

 private:
  cudaStream_t stream_ = nullptr;
};

template <typename T>
std::vector<T> ReadBinary(const std::string& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) throw std::runtime_error("Cannot open " + path);
  const auto bytes = stream.tellg();
  if (bytes < 0 || static_cast<std::size_t>(bytes) % sizeof(T) != 0) {
    throw std::runtime_error("Invalid binary length " + path);
  }
  std::vector<T> values(static_cast<std::size_t>(bytes) / sizeof(T));
  stream.seekg(0);
  if (!stream.read(reinterpret_cast<char*>(values.data()), bytes)) {
    throw std::runtime_error("Cannot read " + path);
  }
  return values;
}

bool RunProbe(const std::string& directory, const std::string& prefix,
              int batch, const std::string& report_path) {
  const auto cold_start = std::chrono::steady_clock::now();
  Logger logger;
  auto engine_bytes =
      ReadBinary<char>(directory + "/ffscn_epoch1200_fp32.engine");
  std::unique_ptr<nvinfer1::IRuntime> runtime(
      nvinfer1::createInferRuntime(logger));
  if (!runtime) throw std::runtime_error("Cannot create TensorRT runtime");
  std::unique_ptr<nvinfer1::ICudaEngine> engine(
      runtime->deserializeCudaEngine(engine_bytes.data(), engine_bytes.size()));
  if (!engine) throw std::runtime_error("Cannot deserialize FP32 engine");
  std::unique_ptr<nvinfer1::IExecutionContext> context(
      engine->createExecutionContext());
  if (!context || !context->setInputShape(
                      "spectrum", nvinfer1::Dims4(batch, 1, 10, 65536))) {
    throw std::runtime_error("Cannot configure B1/B2 input shape");
  }
  const auto input = ReadBinary<float>(directory + "/" + prefix + ".input.f32");
  if (input.size() != static_cast<std::size_t>(batch) * 655360) {
    throw std::runtime_error("Input byte count differs from frozen shape");
  }
  CudaStream stream;
  auto device_input = AllocateDevice(input.size() * sizeof(float));
  CheckCuda(cudaMemcpyAsync(device_input.get(), input.data(),
                            input.size() * sizeof(float),
                            cudaMemcpyHostToDevice, stream.get()),
            "cudaMemcpyAsync input");
  if (!context->setTensorAddress("spectrum", device_input.get())) {
    throw std::runtime_error("Cannot bind input tensor");
  }
  const char* output_names[] = {"hm", "bw", "off"};
  std::vector<DeviceMemory> device_outputs;
  const std::size_t elements = static_cast<std::size_t>(batch) * 16384;
  for (const char* name : output_names) {
    const auto shape = context->getTensorShape(name);
    if (shape.nbDims != 4 || shape.d[0] != batch || shape.d[1] != 1 ||
        shape.d[2] != 1 || shape.d[3] != 16384 ||
        engine->getTensorDataType(name) != nvinfer1::DataType::kFLOAT) {
      throw std::runtime_error(std::string("Unexpected output shape: ") + name);
    }
    device_outputs.push_back(AllocateDevice(elements * sizeof(float)));
    if (!context->setTensorAddress(name, device_outputs.back().get())) {
      throw std::runtime_error(std::string("Cannot bind output: ") + name);
    }
  }
  if (!context->enqueueV3(stream.get())) {
    throw std::runtime_error("TensorRT enqueue failed");
  }
  CheckCuda(cudaStreamSynchronize(stream.get()), "cudaStreamSynchronize");
  const auto cold_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - cold_start)
                           .count();
  std::size_t gpu_free_bytes = 0, gpu_total_bytes = 0;
  CheckCuda(cudaMemGetInfo(&gpu_free_bytes, &gpu_total_bytes),
            "cudaMemGetInfo");
  std::vector<double> hot_ms;
  for (int iteration = 0; iteration < 5; ++iteration) {
    const auto start = std::chrono::steady_clock::now();
    CheckCuda(cudaMemcpyAsync(device_input.get(), input.data(),
                              input.size() * sizeof(float),
                              cudaMemcpyHostToDevice, stream.get()),
              "hot input upload");
    if (!context->enqueueV3(stream.get()))
      throw std::runtime_error("Hot TensorRT enqueue failed");
    std::vector<float> downloaded(elements);
    for (const auto& output : device_outputs)
      CheckCuda(cudaMemcpyAsync(downloaded.data(), output.get(),
                                elements * sizeof(float),
                                cudaMemcpyDeviceToHost, stream.get()),
                "hot output download");
    CheckCuda(cudaStreamSynchronize(stream.get()), "hot synchronization");
    hot_ms.push_back(std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count());
  }
  cudaDeviceProp device{};
  CheckCuda(cudaGetDeviceProperties(&device, 0), "cudaGetDeviceProperties");
  std::ofstream report(report_path);
  if (!report) throw std::runtime_error("Cannot open native report");
  report << "{\n  \"schema\": \"signalstudio-native-model-parity-v1\",\n"
         << "  \"gpu\": \"" << device.name << "\",\n"
         << "  \"batch\": " << batch
         << ",\n  \"coldReadEngineContextAllocateUploadInferMilliseconds\": "
         << cold_ms << ",\n  \"hotUploadInferDownloadMilliseconds\": [";
  for (std::size_t index = 0; index < hot_ms.size(); ++index)
    report << (index ? ", " : "") << hot_ms[index];
  auto ordered_hot = hot_ms;
  std::sort(ordered_hot.begin(), ordered_hot.end());
  report << "],\n  \"hotMedianWindowsPerSecond\": "
         << batch * 1000.0 / ordered_hot[ordered_hot.size() / 2]
         << ",\n  \"explicitIoAllocationBytes\": "
         << (input.size() + elements * 3) * sizeof(float)
         << ",\n  \"engineContextDeviceMemoryBytes\": "
         << engine->getDeviceMemorySizeV2() << ",\n  \"wholeDeviceUsedBytes\": "
         << gpu_total_bytes - gpu_free_bytes
         << ",\n  \"resourceScope\": \"Whole-device usage is not process "
            "allocation; "
            "timings exclude source DSP, tracking, refinement and persistence\""
         << ",\n  \"outputs\": [\n";
  bool passed = true;
  for (std::size_t index = 0; index < 3; ++index) {
    std::vector<float> actual(elements);
    CheckCuda(cudaMemcpy(actual.data(), device_outputs[index].get(),
                         elements * sizeof(float), cudaMemcpyDeviceToHost),
              "cudaMemcpy output");
    const auto expected =
        ReadBinary<float>(directory + "/" + prefix + "." + output_names[index] +
                          ".reference.f32");
    if (expected.size() != elements) {
      throw std::runtime_error("Reference output count differs");
    }
    double maximum = 0;
    std::size_t outside = 0;
    for (std::size_t item = 0; item < elements; ++item) {
      const double difference = std::abs(static_cast<double>(actual[item]) -
                                         static_cast<double>(expected[item]));
      maximum = std::max(maximum, difference);
      if (!std::isfinite(actual[item]) || !std::isfinite(expected[item]) ||
          difference > 1e-4 + 1e-3 * std::abs(expected[item])) {
        ++outside;
      }
    }
    passed = passed && outside == 0;
    report << "    {\"name\": \"" << output_names[index]
           << "\", \"elements\": " << elements
           << ", \"maxAbsolute\": " << maximum
           << ", \"outsideTolerance\": " << outside << "}"
           << (index == 2 ? "\n" : ",\n");
  }
  report << "  ],\n  \"passed\": " << (passed ? "true" : "false") << "\n}\n";
  return passed;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 5) {
      std::cerr
          << "Usage: ModelNativeProbe asset-directory prefix batch report\n";
      return 2;
    }
    const int batch = std::stoi(argv[3]);
    if (batch < 1 || batch > 2)
      throw std::runtime_error("Batch must be 1 or 2");
    return RunProbe(argv[1], argv[2], batch, argv[4]) ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
