#pragma once
#include <QString>

#include "domain/project.h"

namespace signalstudio {
struct LabelImportResult {
  AnnotationSourceSet sourceSet;
  std::vector<Annotation> annotations;
  std::vector<AnnotationDiagnostic> diagnostics;
  QString error;
  bool ok() const { return error.isEmpty(); }
  explicit operator bool() const { return ok(); }
};
LabelImportResult importGeneratorLabels(const QString&, const FileMetadata&,
                                        std::uint64_t projectGeneration = 0);
LabelImportResult importTorchSigLabels(const QString&, const FileMetadata&,
                                       std::uint64_t projectGeneration = 0,
                                       bool ignoreSupplemental = false);
LabelImportResult importLabelSource(const QString&, const FileMetadata&,
                                    std::uint64_t projectGeneration = 0,
                                    bool ignoreSupplemental = false);
}  // namespace signalstudio
