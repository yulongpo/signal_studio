#include "infrastructure/detection_spectrum_spool.h"

#include "infrastructure/detection_source.h"

#include <QByteArray>
#include <QStorageInfo>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

namespace signalstudio {
namespace {
constexpr std::uint64_t kReadBlockBytes = 4 * 1024 * 1024;

bool fail(QString& error, const QString& reason) {
  error = reason;
  return false;
}
bool stopped(const std::function<bool()>& cancel, QString& error) {
  if (!cancel || !cancel()) return false;
  error = "frequency-analysis-cancelled";
  return true;
}
std::uint32_t orderedFloatBits(float value) {
  const auto bits = std::bit_cast<std::uint32_t>(value);
  return bits & 0x80000000U ? ~bits : bits ^ 0x80000000U;
}
float floatFromOrderedBits(std::uint32_t ordered) {
  const auto bits = ordered & 0x80000000U ? ordered ^ 0x80000000U : ~ordered;
  return std::bit_cast<float>(bits);
}
}  // namespace

bool SpectrumSpool::create(FixedDetectionSource& source,
                           std::uint64_t firstFrame, std::uint64_t endFrame,
                           std::uint64_t byteBudget, QString& error,
                           const std::function<bool()>& cancel) {
  bins_ = source.plan().config.fftSize;
  if (endFrame < firstFrame || bins_ == 0)
    return fail(error, "frequency-spool-invalid-range");
  firstFrame_ = firstFrame;
  frameCount_ = endFrame - firstFrame;
  const auto stride = bins_ * sizeof(float);
  if (frameCount_ > byteBudget / stride)
    return fail(error, "frequency-spool-disk-budget-exceeded");
  const auto bytes = frameCount_ * stride;
  if (!file_.open())
    return fail(error, "frequency-spool-open-failed: " + file_.errorString());
  const QStorageInfo storage(file_.fileName());
  if (storage.isValid() && storage.isReady() &&
      storage.bytesAvailable() >= 0 &&
      static_cast<std::uint64_t>(storage.bytesAvailable()) <
          bytes + 64 * 1024 * 1024)
    return fail(error, "frequency-spool-insufficient-free-disk");
  QByteArray encoded(static_cast<qsizetype>(stride), '\0');
  for (auto frameIndex = firstFrame; frameIndex < endFrame; ++frameIndex) {
    if (stopped(cancel, error)) return false;
    DetectionSpectrumFrame frame;
    if (!source.readFrame(frameIndex, frame, error, cancel)) return false;
    if (frame.densityDb.size() != bins_)
      return fail(error, "frequency-spool-frame-shape-mismatch");
    for (std::size_t bin = 0; bin < bins_; ++bin) {
      if (!std::isfinite(frame.densityDb[bin]))
        return fail(error, "frequency-spool-nonfinite-db");
      qToLittleEndian(std::bit_cast<std::uint32_t>(frame.densityDb[bin]),
                      encoded.data() + bin * sizeof(float));
    }
    if (file_.write(encoded) != encoded.size())
      return fail(error, "frequency-spool-write-failed: " + file_.errorString());
  }
  if (!file_.flush() || file_.size() != static_cast<qint64>(bytes))
    return fail(error, "frequency-spool-size-or-flush-failed");
  return true;
}

bool SpectrumSpool::percentile(double& result, QString& error,
                               const std::function<bool()>& cancel) {
  const auto count = frameCount_ * bins_;
  if (!count) return fail(error, "frequency-spool-empty-percentile");
  const double fractionalRank = .1 * static_cast<double>(count - 1);
  std::array<std::uint64_t, 2> ranks{
      static_cast<std::uint64_t>(std::floor(fractionalRank)),
      static_cast<std::uint64_t>(std::ceil(fractionalRank))};
  std::array<std::uint32_t, 2> prefixes{};
  QByteArray block(static_cast<qsizetype>(kReadBlockBytes), '\0');
  for (int shift = 24; shift >= 0; shift -= 8) {
    std::array<std::array<std::uint64_t, 256>, 2> histogram{};
    const auto mask = shift == 24 ? 0U : ~0U << (shift + 8);
    if (!file_.seek(0)) return fail(error, "frequency-spool-seek-failed");
    for (std::uint64_t offset = 0; offset < count * sizeof(float);) {
      if (stopped(cancel, error)) return false;
      const auto size = static_cast<qint64>(
          std::min(kReadBlockBytes, count * sizeof(float) - offset));
      if (file_.read(block.data(), size) != size)
        return fail(error, "frequency-spool-radix-read-failed");
      for (qint64 position = 0; position < size; position += sizeof(float)) {
        const auto bits =
            qFromLittleEndian<std::uint32_t>(block.data() + position);
        const auto ordered = orderedFloatBits(std::bit_cast<float>(bits));
        const auto bucket = (ordered >> shift) & 255U;
        for (std::size_t rank = 0; rank < ranks.size(); ++rank)
          if ((ordered & mask) == prefixes[rank]) ++histogram[rank][bucket];
      }
      offset += static_cast<std::uint64_t>(size);
    }
    for (std::size_t rank = 0; rank < ranks.size(); ++rank) {
      std::size_t bucket = 0;
      while (bucket < 256 && ranks[rank] >= histogram[rank][bucket]) {
        ranks[rank] -= histogram[rank][bucket];
        ++bucket;
      }
      if (bucket == 256)
        return fail(error, "frequency-spool-radix-rank-inconsistent");
      prefixes[rank] |= static_cast<std::uint32_t>(bucket) << shift;
    }
  }
  const float lower = floatFromOrderedBits(prefixes[0]);
  const float upper = floatFromOrderedBits(prefixes[1]);
  const float difference = upper - lower;
  const double gamma = fractionalRank - std::floor(fractionalRank);
  // NumPy 2.4's np.percentile(float32, Python scalar 10) uses weak scalar
  // promotion: each subtraction/product/addition rounds to float32. The
  // upper-end interpolation avoids cancellation for gamma >= one half.
  const float weight = static_cast<float>(gamma >= .5 ? 1 - gamma : gamma);
  const float product = difference * weight;
  const float interpolated = gamma >= .5 ? upper - product : lower + product;
  result = interpolated;
  return !stopped(cancel, error);
}

bool SpectrumSpool::mean(std::uint64_t first, std::uint64_t end,
                         std::vector<double>& meanDb, QString& error,
                         const std::function<bool()>& cancel) {
  if (end <= first) return fail(error, "frequency-spool-empty-mean");
  meanDb.assign(bins_, 0);
  QByteArray bytes(static_cast<qsizetype>(bins_ * sizeof(float)), '\0');
  if (!file_.seek(static_cast<qint64>((first - firstFrame_) * bins_ *
                                      sizeof(float))))
    return fail(error, "frequency-spool-track-seek-failed");
  for (auto index = first; index < end; ++index) {
    if (stopped(cancel, error)) return false;
    if (file_.read(bytes.data(), bytes.size()) != bytes.size())
      return fail(error, "frequency-spool-track-read-failed");
    for (std::size_t bin = 0; bin < bins_; ++bin) {
      const auto bits = qFromLittleEndian<std::uint32_t>(bytes.data() +
                                                         bin * sizeof(float));
      meanDb[bin] += std::bit_cast<float>(bits);
    }
  }
  for (auto& value : meanDb) value /= end - first;
  return true;
}

bool SpectrumSpool::covers(std::uint64_t first, std::uint64_t end) const {
  return bins_ != 0 && frameCount_ != 0 && first <= end &&
         first >= firstFrame_ && end <= firstFrame_ + frameCount_;
}

bool SpectrumSpool::readDensity(std::uint64_t absoluteFrame,
                                std::vector<float>& densityDb, QString& error) {
  if (!frameCount_ || !bins_ || absoluteFrame < firstFrame_ ||
      absoluteFrame >= firstFrame_ + frameCount_)
    return fail(error, "frequency-spool-frame-out-of-range");
  QByteArray bytes(static_cast<qsizetype>(bins_ * sizeof(float)), '\0');
  const auto offset = static_cast<qint64>((absoluteFrame - firstFrame_) *
                                          bins_ * sizeof(float));
  if (!file_.seek(offset))
    return fail(error, "frequency-spool-frame-seek-failed");
  if (file_.read(bytes.data(), bytes.size()) != bytes.size())
    return fail(error, "frequency-spool-frame-read-failed");
  densityDb.resize(bins_);
  for (std::size_t bin = 0; bin < bins_; ++bin) {
    const auto bits =
        qFromLittleEndian<std::uint32_t>(bytes.data() + bin * sizeof(float));
    densityDb[bin] = std::bit_cast<float>(bits);
  }
  return true;
}
}  // namespace signalstudio
