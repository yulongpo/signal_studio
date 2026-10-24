#pragma once
#include <QString>
#include <QJsonObject>
#include <functional>

#include "domain/project.h"

namespace signalstudio {
enum class LabelImportState { Valid, Empty, NeedsMapping, Incomplete, Error,
                              Cancelled, LimitExceeded };
struct LabelImportOptions {
  qint64 maximumBytes = 32 * 1024 * 1024;
  std::size_t maximumRecords = 100000;
  std::size_t maximumLines = 1000000;
  int maximumJsonDepth = 64;
  qint64 timeoutMs = 5000;
  // Called on the worker thread. The caller owns any captured cancellation token.
  std::function<bool()> cancelled;
};
// Content signature only; successful recognition is never schema validation.
bool hasGeneratorLabelHeader(const QString& boundedPreview);
// Read-only bounded UTF-8 preview for arbitrary manually selected label paths.
// The output is cleared on failure. Call on the IO worker, not the GUI thread.
bool previewLabelSource(const QString& path, QString& preview, QString& error,
                         const LabelImportOptions& = {},
                         qint64 maximumPreviewBytes = 16384,
                         LabelImportState* failureState = nullptr);

// No inferred columns, units, index origin, or interval semantics. Column
// numbers are zero based; -1 explicitly means that the field is unknown.
enum class LabelDelimiter { Unspecified, Whitespace, Tab, Comma, Semicolon };
enum class LabelTimeUnit { Unspecified, Unknown, Samples, Seconds, Frames };
enum class LabelInterval { Unspecified, ExclusiveEnd, InclusiveEnd, StartCount };
enum class LabelFrequencyLayout { Unspecified, Unknown, Edges, CenterBandwidth };
enum class LabelFrequencyUnit { Unspecified, Hz, KHz, MHz, GHz };
enum class LabelFrequencyReference { Unspecified, RF, Baseband };
struct LabelFieldMapping {
  int schemaVersion = 1;
  LabelDelimiter delimiter = LabelDelimiter::Unspecified;
  int skipLines = 0;
  QString commentPrefix;
  int classColumn = -1, startColumn = -1, endColumn = -1;
  int lowerColumn = -1, upperColumn = -1;
  LabelTimeUnit timeUnit = LabelTimeUnit::Unspecified;
  LabelInterval interval = LabelInterval::Unspecified;
  int indexOrigin = -1;
  std::uint64_t frameHopSamples = 0, frameLengthSamples = 0;
  bool allowTimeRounding = false;
  double timeQuantizationSeconds = 0;
  LabelFrequencyLayout frequencyLayout = LabelFrequencyLayout::Unspecified;
  LabelFrequencyUnit frequencyUnit = LabelFrequencyUnit::Unspecified;
  LabelFrequencyReference frequencyReference = LabelFrequencyReference::Unspecified;
};
QJsonObject encodeLabelFieldMapping(const LabelFieldMapping&);
bool decodeLabelFieldMapping(const QJsonObject&, LabelFieldMapping&, QString& error);
bool restoreLabelFieldMapping(const AnnotationSourceSet&, LabelFieldMapping&,
                              QString& error);

struct LabelImportResult {
  AnnotationSourceSet sourceSet;
  std::vector<Annotation> annotations;
  std::vector<AnnotationDiagnostic> diagnostics;
  QString error;
  LabelImportState state = LabelImportState::Valid;
  std::size_t incompleteRecords = 0;
  bool ok() const { return error.isEmpty(); }
  explicit operator bool() const { return ok(); }
};
LabelImportResult importGeneratorLabels(const QString&, const FileMetadata&,
                                        std::uint64_t projectGeneration = 0,
                                        const LabelImportOptions& = {});
LabelImportResult importTorchSigLabels(const QString&, const FileMetadata&,
                                       std::uint64_t projectGeneration = 0,
                                       bool ignoreSupplemental = false,
                                       const LabelImportOptions& = {});
LabelImportResult importLabelSource(const QString&, const FileMetadata&,
                                    std::uint64_t projectGeneration = 0,
                                    bool ignoreSupplemental = false,
                                    const LabelImportOptions& = {});
LabelImportResult importMappedTextLabels(const QString&, const FileMetadata&,
                                         const LabelFieldMapping&,
                                         std::uint64_t projectGeneration = 0,
                                         const LabelImportOptions& = {});
}  // namespace signalstudio
