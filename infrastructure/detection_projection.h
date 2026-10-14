#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "domain/detection_postprocess.h"
#include "infrastructure/detection_task.h"

namespace signalstudio {

// Keep existing short record IDs; hash longer seeds without losing the full
// seed preserved in immutable track evidence.
std::string detectionTrackRecordId(const std::string& seedId);

// Replays the same durable candidate/mapping pairs used by the live task.
// Frequency association never invents activity intervals from window support.
class DetectionResultProjector {
 public:
  bool reset(const DetectionRequest&, std::uint64_t denominator,
             std::uint64_t residentBudget, QString& error);
  bool consume(const QJsonArray& results, const QJsonArray& mappings,
               QString& error, const PostprocessCancellation& cancelled = {});
  bool finish(std::vector<FrequencyTrack>&, QString& error,
              const PostprocessCancellation& cancelled = {}) const;
  bool attachWindowEvidence(DetectionPublication&, QString& error) const;

 private:
  DetectionRequest request_;
  FrequencyTracker tracker_;
  bool initialized_ = false;
  double firstBinHz_ = 0, binWidthHz_ = 0;
  std::uint64_t denominator_ = 0, residentBudget_ = 0;
  QJsonObject windowEvidence_;
  std::uint64_t windowEvidenceBytes_ = 0;
};

bool projectFrequencyTracks(const DetectionRequest&, const AnnotationSourceSet&,
                            const std::vector<FrequencyTrack>&,
                            std::uint64_t residentBudget, DetectionPublication&,
                            QString& error);
QJsonArray encodeDetectionPublication(const DetectionPublication&);
bool decodeDetectionPublication(const QJsonArray&, const DetectionRequest&,
                                const AnnotationSourceSet&,
                                std::uint64_t residentBudget,
                                DetectionPublication&, QString& error);

bool validateDetectionPublicationBudget(const DetectionPublication&,
                                        std::uint64_t residentBudget,
                                        QString& error);
}  // namespace signalstudio
