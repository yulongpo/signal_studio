#include "infrastructure/detection_sample_energy.h"
#include "infrastructure/mkl_dft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <numbers>

namespace signalstudio {
namespace {
using Complex = std::complex<float>;
constexpr double kTau = 2 * std::numbers::pi;

Complex unit(double angle) {
  return {static_cast<float>(std::cos(angle)),
          static_cast<float>(std::sin(angle))};
}
bool stopped(const DetectionSourceCancel& cancel, QString& error) {
  if (!cancel || !cancel()) return false;
  error = "Cancelled during exact-length sample FFT";
  return true;
}
void markFailure(DetectionFailureCode* slot, DetectionFailureCode code) {
  if (slot) *slot = code;
}
QString energyWorkingSetError(long double required, std::uint64_t allowed) {
  const auto bytes =
      required > static_cast<long double>(std::numeric_limits<quint64>::max())
          ? std::numeric_limits<quint64>::max()
          : static_cast<quint64>(std::ceil(required));
  return QString("Energy working-set exceeded: requiredBytes=%1 allowedBytes=%2")
      .arg(qulonglong(bytes))
      .arg(qulonglong(allowed));
}
std::size_t radix(std::size_t size) {
  for (const auto factor : {2U, 3U, 5U, 7U, 11U, 13U})
    if (size % factor == 0) return factor;
  return size;
}
std::size_t convolutionLength(std::size_t size) {
  if (size > std::numeric_limits<std::size_t>::max() / 2) return 0;
  std::size_t length = 1;
  while (length < 2 * size - 1) {
    if (length > std::numeric_limits<std::size_t>::max() / 2) return 0;
    length *= 2;
  }
  return length;
}
std::size_t largestLeaf(std::size_t size) {
  while (size > 1) {
    const auto factor = radix(size);
    if (factor == size) return size;
    size /= factor;
  }
  return 1;
}
bool radixTwo(std::vector<Complex>& values, bool inverse, QString& error,
              const DetectionSourceCancel& cancel) {
  const auto size = values.size();
  for (std::size_t i = 1, j = 0; i < size; ++i) {
    auto bit = size / 2;
    for (; j & bit; bit /= 2) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(values[i], values[j]);
  }
  const double sign = inverse ? 1 : -1;
  for (std::size_t width = 2; width <= size; width *= 2) {
    if (stopped(cancel, error)) return false;
    const std::complex<double> step = std::polar(1.0, sign * kTau / width);
    for (std::size_t start = 0; start < size; start += width) {
      std::complex<double> phase{1, 0};
      for (std::size_t j = 0; j < width / 2; ++j) {
        if ((j & 1023U) == 0) phase = std::polar(1.0, sign * kTau * j / width);
        const auto a = values[start + j];
        const auto b = values[start + j + width / 2] *
                       Complex{static_cast<float>(phase.real()),
                               static_cast<float>(phase.imag())};
        values[start + j] = a + b;
        values[start + j + width / 2] = a - b;
        phase *= step;
      }
    }
    if (width == size) break;
  }
  if (inverse)
    for (auto& value : values) value /= static_cast<float>(size);
  return true;
}
bool bluestein(std::span<Complex> values, bool inverse, QString& error,
               const DetectionSourceCancel& cancel) {
  const auto size = values.size(), length = convolutionLength(size);
  std::vector<Complex> a(length), b(length);
  const double sign = inverse ? 1 : -1;
  const auto chirp = [&](std::size_t i) {
    const auto squared = std::fmod(static_cast<long double>(i) * i,
                                   static_cast<long double>(2) * size);
    return unit(sign * std::numbers::pi * static_cast<double>(squared) / size);
  };
  for (std::size_t i = 0; i < size; ++i) {
    a[i] = values[i] * chirp(i);
    b[i] = std::conj(chirp(i));
    if (i) b[length - i] = b[i];
  }
  if (!radixTwo(a, false, error, cancel) || !radixTwo(b, false, error, cancel))
    return false;
  for (std::size_t i = 0; i < length; ++i) a[i] *= b[i];
  if (!radixTwo(a, true, error, cancel)) return false;
  for (std::size_t i = 0; i < size; ++i) values[i] = a[i] * chirp(i);
  return true;
}

// Decimation in frequency: r small DFTs, then r contiguous transforms. A
// bounded bitset permutes the r x m result into natural frequency order.
bool mixedTransform(std::span<Complex> values, bool inverse, QString& error,
                    const DetectionSourceCancel& cancel) {
  const auto size = values.size();
  if (size == 1) return true;
  if (stopped(cancel, error)) return false;
  const auto factor = radix(size);
  if (factor == size && size > 13)
    return bluestein(values, inverse, error, cancel);
  const auto part = size / factor;
  const double sign = inverse ? 1 : -1;
  std::array<Complex, 13> inputs{}, outputs{};
  std::array<std::array<Complex, 13>, 13> roots{};
  std::array<std::complex<double>, 13> phases{}, steps{};
  for (std::size_t p = 0; p < factor; ++p)
    for (std::size_t q = 0; q < factor; ++q)
      roots[p][q] = unit(sign * kTau * p * q / factor);
  for (std::size_t p = 0; p < factor; ++p) {
    phases[p] = {1, 0};
    steps[p] = std::polar(1.0, sign * kTau * p / size);
  }
  for (std::size_t j = 0; j < part; ++j) {
    if ((j & 4095U) == 0 && stopped(cancel, error)) return false;
    for (std::size_t q = 0; q < factor; ++q) inputs[q] = values[q * part + j];
    for (std::size_t p = 0; p < factor; ++p) {
      if ((j & 1023U) == 0)
        phases[p] = std::polar(1.0, sign * kTau * p * j / size);
      Complex sum{};
      for (std::size_t q = 0; q < factor; ++q) sum += inputs[q] * roots[p][q];
      outputs[p] = sum * Complex{static_cast<float>(phases[p].real()),
                                 static_cast<float>(phases[p].imag())};
      phases[p] *= steps[p];
    }
    for (std::size_t p = 0; p < factor; ++p) values[p * part + j] = outputs[p];
  }
  for (std::size_t p = 0; p < factor; ++p)
    if (!mixedTransform(values.subspan(p * part, part), inverse, error, cancel))
      return false;
  std::vector<bool> visited(size, false);
  for (std::size_t start = 0; start < size; ++start) {
    if (visited[start]) continue;
    if ((start & 4095U) == 0 && stopped(cancel, error)) return false;
    auto i = start;
    auto carried = values[i];
    do {
      const auto next = (i % part) * factor + i / part;
      std::swap(carried, values[next]);
      visited[i] = true;
      i = next;
    } while (i != start);
  }
  return true;
}
}  // namespace

bool detectionComplexTransform(std::span<Complex> samples, bool inverse,
                               std::uint64_t budget, QString& error,
                               const DetectionSourceCancel& cancel,
                               DetectionFailureCode* failure) {
  if (samples.empty() || samples.size() > std::numeric_limits<std::uint32_t>::max()) {
    error = "Sample FFT length exceeds oneMKL descriptor range";
    markFailure(failure, DetectionFailureCode::HardFailure);
    return false;
  }
  const auto minimum =
      static_cast<long double>(samples.size()) * sizeof(Complex) + 65536;
  if (minimum > budget) {
    error = energyWorkingSetError(minimum, budget);
    markFailure(failure, DetectionFailureCode::ResourceLimit);
    return false;
  }
  for (const auto value : samples)
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
      error = "Sample FFT input is not finite";
      markFailure(failure, DetectionFailureCode::HardFailure);
      return false;
    }
  static std::mutex mklMutex;
  static MklDft mkl;
  std::string why;
  {
    std::lock_guard<std::mutex> lock(mklMutex);
    if (mkl.transform(samples.data(), static_cast<std::uint32_t>(samples.size()), inverse, why)) {
      error.clear();
      markFailure(failure, DetectionFailureCode::None);
      return true;
    }
  }
  if (why != "missing") {
    error = QString::fromStdString(why);
    markFailure(failure, DetectionFailureCode::HardFailure);
    return false;
  }
  const auto leaf = largestLeaf(samples.size());
  const auto scratch = leaf > 13 ? convolutionLength(leaf) : 0;
  const auto bytes =
      static_cast<long double>(samples.size()) * (sizeof(Complex) + 1.0 / 8) +
      static_cast<long double>(scratch) * 2 * sizeof(Complex) + 65536;
  if (leaf > 13 && !scratch) {
    error = "Exact-length sample FFT scratch length is unavailable";
    markFailure(failure, DetectionFailureCode::HardFailure);
    return false;
  }
  if (bytes > budget) {
    error = energyWorkingSetError(bytes, budget);
    markFailure(failure, DetectionFailureCode::ResourceLimit);
    return false;
  }
  if (!mixedTransform(samples, inverse, error, cancel)) {
    markFailure(failure, cancel && cancel() ? DetectionFailureCode::Cancelled
                                            : DetectionFailureCode::HardFailure);
    return false;
  }
  if (inverse)
    for (auto& value : samples) value /= static_cast<float>(samples.size());
  markFailure(failure, DetectionFailureCode::None);
  return true;
}

bool detectionBoundedRadixTwoTransform(std::vector<Complex>& samples,bool inverse,
    std::uint64_t budget,QString& error,const DetectionSourceCancel& cancel,
    DetectionFailureCode* failure) {
  const auto size=samples.size();
  const auto required=static_cast<long double>(size)*sizeof(Complex)+65536;
  if(!size || size>(1U<<20) || (size&(size-1))) {
    error="Bounded radix-two FFT length is invalid";
    markFailure(failure, DetectionFailureCode::HardFailure);
    return false;
  }
  if(required>budget) {
    error=energyWorkingSetError(required, budget);
    markFailure(failure, DetectionFailureCode::ResourceLimit);
    return false;
  }
  for(const auto value:samples)
    if(!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
      error="Bounded radix-two FFT input is not finite";
      markFailure(failure, DetectionFailureCode::HardFailure);
      return false;
    }
  if(stopped(cancel,error)) {
    markFailure(failure, DetectionFailureCode::Cancelled);
    return false;
  }
  if(!radixTwo(samples,inverse,error,cancel)) {
    markFailure(failure, cancel && cancel() ? DetectionFailureCode::Cancelled
                                            : DetectionFailureCode::HardFailure);
    return false;
  }
  markFailure(failure, DetectionFailureCode::None);
  return true;
}

bool detectionBandEnergy(std::vector<Complex> samples, double rate,
                         double center, double bandwidth,
                         std::vector<float>& output, std::uint64_t budget,
                         QString& error, const DetectionSourceCancel& cancel,
                         const DetectionFilteredSampleObserver& filteredObserver,
                         DetectionFailureCode* failure) {
  const auto required = static_cast<long double>(samples.size()) *
                            (sizeof(Complex) + sizeof(float)) +
                        65536;
  if (!std::isfinite(rate) || rate <= 0 || !std::isfinite(center) ||
      !std::isfinite(bandwidth) || bandwidth <= 0 || bandwidth > rate) {
    error = "Sample energy input is invalid";
    markFailure(failure, DetectionFailureCode::HardFailure);
    return false;
  }
  if (required > budget) {
    error = energyWorkingSetError(required, budget);
    markFailure(failure, DetectionFailureCode::ResourceLimit);
    return false;
  }
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if ((i & 4095U) == 0 && stopped(cancel, error)) {
      markFailure(failure, DetectionFailureCode::Cancelled);
      return false;
    }
    // Same relative phase and complex64 oscillator contract as the reference.
    double phase = std::fmod(-kTau * center * i / rate, kTau);
    if (phase < 0) phase += kTau;
    samples[i] *= unit(static_cast<float>(phase));
  }
  if (!detectionComplexTransform(samples, false, budget, error, cancel, failure))
    return false;
  const double cutoff = bandwidth / 2;
  const double taper = std::max(bandwidth * .25, 1.0);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double frequency =
        std::min(i, samples.size() - i) * rate / samples.size();
    double gain = 1;
    if (frequency > cutoff + taper)
      gain = 0;
    else if (frequency > cutoff)
      gain =
          .5 * (1 + std::cos(std::numbers::pi * (frequency - cutoff) / taper));
    samples[i] *= static_cast<float>(gain);
  }
  if (!detectionComplexTransform(samples, true, budget, error, cancel, failure))
    return false;
  if (filteredObserver) {
    for (std::size_t first = 0; first < samples.size();) {
      if (stopped(cancel, error)) {
        markFailure(failure, DetectionFailureCode::Cancelled);
        return false;
      }
      const auto count = std::min<std::size_t>(131072, samples.size() - first);
      if (!filteredObserver(first, std::span<const Complex>(samples).subspan(first, count), error)) {
        markFailure(failure, DetectionFailureCode::HardFailure);
        return false;
      }
      first += count;
    }
  }
  std::vector<float> power(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    if ((i & 4095U) == 0 && stopped(cancel, error)) {
      markFailure(failure, DetectionFailureCode::Cancelled);
      return false;
    }
    const float value = std::norm(samples[i]);
    if (!std::isfinite(value)) {
      error = "Sample energy output is not finite";
      markFailure(failure, DetectionFailureCode::HardFailure);
      return false;
    }
    power[i] = value;
  }
  output = std::move(power);
  markFailure(failure, DetectionFailureCode::None);
  return true;
}
}  // namespace signalstudio
