#pragma once

#include <QString>
#include <QTemporaryFile>

#include <cstdint>
#include <functional>
#include <vector>

namespace signalstudio {
class FixedDetectionSource;

// Little-endian float32 densityDb, one analysis frame per record, in the same
// order frequency analysis writes. The file is removed when this object dies.
class SpectrumSpool {
 public:
  bool create(FixedDetectionSource& source, std::uint64_t firstFrame,
              std::uint64_t endFrame, std::uint64_t byteBudget, QString& error,
              const std::function<bool()>& cancel);
  bool percentile(double& result, QString& error,
                  const std::function<bool()>& cancel);
  bool mean(std::uint64_t firstFrame, std::uint64_t endFrame,
            std::vector<double>& meanDb, QString& error,
            const std::function<bool()>& cancel);
  bool covers(std::uint64_t firstFrame, std::uint64_t endFrame) const;
  bool readDensity(std::uint64_t absoluteFrame, std::vector<float>& densityDb,
                   QString& error);

 private:
  QTemporaryFile file_;
  std::uint64_t firstFrame_ = 0;
  std::uint64_t frameCount_ = 0;
  std::size_t bins_ = 0;
};
}  // namespace signalstudio
