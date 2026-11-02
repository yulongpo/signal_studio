#pragma once

#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <memory>

namespace signalstudio::sample_stream::scratch_recovery {
struct Limits {
  std::uint32_t maximumRootEntries = 256;
  std::uint32_t maximumCandidateDirectories = 32;
  std::uint32_t maximumFilesPerDirectory = 4;
  std::uint32_t maximumMilliseconds = 250;
  std::uint64_t maximumReclaimedBytes = 32ULL << 30;
  std::function<bool()> cancelled;
};
struct Report {
  bool supported = false, complete = false, safeToAdmit = false, cancelled = false;
  bool timeBudgetReached = false;
  std::uint64_t elapsedMilliseconds = 0;
  std::uint32_t rootEntriesVisited = 0, candidatesVisited = 0, filesValidated = 0;
  std::uint32_t directoriesRemoved = 0, activeOwnersSkipped = 0;
  std::uint32_t activeOtherProcessOwners = 0, invalidOrForeignSkipped = 0;
  std::uint32_t unknownProcessSkipped = 0;
  std::uint64_t reclaimedBytes = 0, retainedOwnedReservationBytes = 0;
  QStringList diagnostics; // <=32 entries, each <=512 chars
};
// One bounded, NONRECURSIVE pass over sample-[A-Za-z0-9]{6} under a trusted
// root only. Unknown/unmarked directories are never entered/deleted. Incomplete
// or suspicious candidate sets are fail-closed for new scratch admission.
// Local Windows volumes currently supported. Other platforms report unsupported.
bool recoverOrphans(const QString& trustedRoot, const Limits&, Report&, QString& error);
// Existing parent only; no recursive mkdir. Validate/pin ancestry while creating
// the final private root leaf. Never canonicalize away a caller's reparse path.
bool prepareTrustedRoot(const QString& trustedRoot, QString& error);

// QTemporaryDir must already have created the unique empty directory below root.
// Acquire the permanent lock and persist owner BEFORE creating E/S. This object
// holds parent/candidate Windows handles denying renames for its whole lifetime.
class OwnerGuard {
 public:
  OwnerGuard();
  ~OwnerGuard();
  OwnerGuard(const OwnerGuard&) = delete;
  OwnerGuard& operator=(const OwnerGuard&) = delete;
  bool begin(const QString& trustedRoot, const QString& ownedDirectory,
             std::uint64_t samples, std::uint64_t sourceOrigin,
             std::uint64_t reservedBytes, QString& error);
  // Only verified white-listed scalar files, then owner, lock, empty directory.
  // On failure retain remaining owner evidence; caller MUST retain disk lease.
  bool cleanup(QString& error);
  bool ownsDirectory() const;
  std::uint64_t metadataBytes() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace signalstudio::sample_stream::scratch_recovery
