#include "ms02_benchmark_support.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace signal;
using namespace signal::benchmarks::ms02;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename T> void require(const core::Result<T>& result, std::string_view message) {
  require(static_cast<bool>(result), message);
}

void require(const core::Status& status, std::string_view message) {
  require(static_cast<bool>(status), message);
}

void verify_external_data_flow(dsp::IFftBackend& backend) {
  const std::filesystem::path root{SIGNAL_STUDIO_EXTERNAL_TEST_DATA_DIR};
  const auto wav = data::FileDataSource::open_wav(
      root / "20241110-174401-662_bw_12800000_sampleTime_0.4_rollOff_0.3.wav", "ms02-integration-wav", true);
  require(wav, "批准 WAV 打开失败");
  const auto wav_read = wav.value()->read({range(4096U), 4096U * 16U, {}});
  require(wav_read && wav_read.value().bytes_read <= 4096U * 16U, "批准 WAV 未执行有界读取");
  const auto wav_psd = dsp::calculate_psd(backend, wav_read.value().samples.view(),
                                          {wav.value()->descriptor().sample_rate_hz, 0.0, dsp::WindowKind::hann,
                                           wav_read.value().samples.kind() == data::SignalKind::complex
                                               ? dsp::SpectrumSidedness::two_sided_shifted
                                               : dsp::SpectrumSidedness::one_sided});
  require(wav_psd && std::ranges::all_of(wav_psd.value().db_per_hz, [](double value) { return std::isfinite(value); }),
          "批准 WAV Data→PSD 结果无效");

  data::SignalDescriptor descriptor;
  descriptor.signal_kind = data::SignalKind::complex;
  descriptor.scalar_type = data::ScalarType::int16;
  descriptor.component_layout = data::ComponentLayout::interleaved;
  descriptor.component_order = data::ComponentOrder::iq;
  descriptor.endianness = data::Endianness::little;
  descriptor.sample_rate_hz = 50'000'000.0;
  descriptor.center_frequency_hz = 1'245'000'000.0;
  descriptor.requested_sample_range = range(4096U);
  const auto raw = data::FileDataSource::open_raw(root / "x310_capture_cf1245MHz_sr50MSps_20260521_144927.sc16",
                                                  descriptor, "ms02-integration-sc16");
  require(raw, "批准 SC16 打开失败");
  const auto raw_read = raw.value()->read({range(4096U), 4096U * 4U, {}});
  require(raw_read && raw_read.value().bytes_read == 4096U * 4U, "批准 SC16 未执行精确有界读取");
  const auto stft = dsp::calculate_stft(
      backend, raw_read.value().samples.view(),
      {50'000'000.0, 1'245'000'000.0, 512U, 256U, dsp::WindowKind::hann, dsp::SpectrumSidedness::two_sided_shifted});
  require(stft && stft.value().rows == 15U && stft.value().columns == 512U, "批准 SC16 Data→STFT 结果无效");
}

void verify_cache_equivalence() {
  const auto key = cache_key();
  const auto expected = tile_for(key, 3.25F);
  auto memory = data::MemoryTileCache::create(64U * 1024U * 1024U, 25U);
  require(memory, "等价性内存缓存创建失败");
  require(memory.value()->put(key, std::make_shared<const data::Tile>(expected)), "等价性内存缓存写入失败");
  const auto memory_hit = memory.value()->get(key);
  require(memory_hit && memory_hit->values == expected.values, "内存缓存结果不等价");

  const auto directory = std::filesystem::temp_directory_path() /
                         ("signal-studio-ms02-equivalence-" + std::to_string(Clock::now().time_since_epoch().count()));
  {
    data::DiskTileStore disk{directory, 16U * 1024U * 1024U};
    require(disk.recover() && disk.put(key, expected), "磁盘缓存写入失败");
  }
  data::DiskTileStore recovered{directory, 16U * 1024U * 1024U};
  const auto disk_hit = recovered.get(key);
  require(disk_hit && disk_hit.value()->values == expected.values &&
              disk_hit.value()->time_range == expected.time_range,
          "进程重建后的磁盘缓存结果不等价");
  std::error_code remove_error;
  std::filesystem::remove_all(directory, remove_error);
}

} // namespace

int main() {
  try {
    const auto discovered = compute::discover_compute_backends();
    require(discovered.size() >= 4U && std::ranges::any_of(discovered,
                                                           [](const auto& backend) {
                                                             const auto capability = backend->capabilities();
                                                             return capability.kind ==
                                                                        compute::BackendKind::cpu_scalar &&
                                                                    capability.available;
                                                           }),
            "生产计算后端探测不完整");
    const auto backend = dsp::make_cpu_fft_backend();
    require(backend, "oneMKL 生产 FFT 后端不可用");
    verify_external_data_flow(*backend.value());
    verify_cache_equivalence();
    std::cout << "MS-02 Data→DSP→Cache 集成验收 PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
