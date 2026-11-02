#pragma once

#include <QString>

namespace signalstudio {

// The active FFSCN package is either the deployed profile directory or an
// explicit selection recorded beside it. Selection does not delete either tree.
struct FfscnActivePackage {
  QString packageDirectory;
  QString manifestPath;
  QString enginePath;
  QString engineSha256;
  QString checkpointSha256;
  QString manifestSha256;
  bool explicitSelection = false;
};

inline bool isLowercaseSha256(const QString& value) {
  if (value.size() != 64) return false;
  for (const QChar character : value) {
    const char16_t code = character.unicode();
    if (!((code >= u'0' && code <= u'9') || (code >= u'a' && code <= u'f')))
      return false;
  }
  return true;
}

QString detectionModelsRoot();
QString ffscnActivePackageRecordPath(const QString& modelRoot);

// FFSCN follows ffscn-active-package.json when that record exists. Every other
// profile stays at modelRoot/<profileId>. A broken FFSCN record fails closed.
bool resolveDetectionPackageDirectory(const QString& modelRoot,
                                      const QString& profileId,
                                      QString& directory, QString& error);

// Missing record reports the deployed ffscn-gui-iq-v1 directory and
// explicitSelection == false. A present but unreadable record is an error.
bool readFfscnActivePackage(const QString& modelRoot, FfscnActivePackage& active,
                            QString& error);

// Contract check only. SCN packages, FP16, tool manifests, and a missing
// manifest.json are rejected. Nothing on disk is modified.
bool inspectFfscnEnginePackage(const QString& packageDirectory,
                               FfscnActivePackage& inspected, QString& error);

// Validates, then atomically publishes the active-package record with QSaveFile.
// The previous package files and any previous record stay in place on failure.
bool activateFfscnEnginePackage(const QString& packageDirectory,
                                const QString& modelRoot,
                                FfscnActivePackage& activated, QString& error);

}  // namespace signalstudio
