#pragma once

#include <QStringList>
#include <map>
#include <optional>

#include "application/annotation_service.h"
#include "infrastructure/label_import.h"

namespace signalstudio {
enum class LabelProfile { MatlabDataTxt, GeneratorText, TorchSig };
enum class LabelDiscoveryState { Found, NotFound, Empty, NeedsMapping, Ambiguous,
                                 Error, LimitExceeded, Cancelled };
struct LabelDiscoveryOptions {
  std::size_t maximumDirectoryEntries = 2048;
  qint64 maximumPreviewBytes = 16384;
  LabelImportOptions parsing;
};
struct LabelCandidate {
  LabelProfile profile = LabelProfile::MatlabDataTxt;
  QString dataPath, labelPath, supplementalPath, preview;
};
struct LabelDiscoveryResult {
  LabelDiscoveryState state = LabelDiscoveryState::NotFound;
  std::vector<LabelCandidate> candidates;
  QString message;
  QStringList inspectedDirectories;
};

// Bounded, nonrecursive, read-only. RAW format/Fs/Fc must already be supplied
// by source import. No binary encoding is inferred from a .data suffix.
LabelDiscoveryResult discoverLabelCandidates(const FileMetadata&,
                                             const LabelDiscoveryOptions& = {});
// Reverse lookup is only for label/<name>.txt -> data/<name>.data. The caller
// still needs the ordinary source-import dialog to determine binary metadata.
LabelDiscoveryResult discoverDataForLabel(const QString&,
                                          const LabelDiscoveryOptions& = {});

enum class LabelImportDisposition { Ready, Empty, IncompleteEvidence, AlreadyImported,
                                    SourceChanged, NeedsMapping, Rejected };
struct LabelImportPlan {
  LabelImportDisposition disposition = LabelImportDisposition::Rejected;
  LabelImportResult imported;
  QString message;
  std::map<std::string, std::size_t> classCounts;
  std::optional<TimeRange> timeExtent;
  std::optional<FrequencyRange> frequencyExtent;
};
// Runs discovery again (do not trust a saved candidate), then parses the whole
// batch. Supplying a mapping means an explicit user/profile decision. Unknown
// TXT is never routed to the generator parser merely because it is a TXT file.
LabelImportPlan prepareDiscoveredLabels(const LabelCandidate&, const FileState&,
                                        std::uint64_t projectGeneration = 0,
                                        const std::optional<LabelFieldMapping>& = {},
                                        const LabelDiscoveryOptions& = {});
// Call on the owning application thread after checking project/file generation.
// Rechecks current immutable source bindings and delegates atomic publication
// and undo to AnnotationService; never reloads or overwrites user revisions.
// IncompleteEvidence requires the app's explicit source-only confirmation;
// it preserves raw text/mapping/diagnostics with zero located annotations.
AnnotationResult publishDiscoveredLabels(AnnotationService&, FileState&,
                                         const LabelImportPlan&,
                                         std::uint64_t projectGeneration);
}  // namespace signalstudio
