#pragma once

#include <QDialog>
#include <QString>

#include <cstdint>

#include "domain/detection.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace signalstudio {
class ParameterInputPolicy;

// Resource choices for detection requests created after the dialog is accepted.
// Analysis-window, track, comparison, shard and engine-batch limits stay fixed.
struct DetectionResourceOptions {
  bool retainRawModelOutput = false;
  std::uint64_t evidenceBudgetBytes = detectionDiskBudgetDefaultBytes({});
  std::uint64_t workingSetBudgetBytes = detectionWorkingSetDefaultBytes();
  std::uint64_t sampleScratchBudgetBytes = detectionDiskBudgetDefaultBytes({});
  // Applied only while retainRawModelOutput is set. Zero is not a quota.
  std::uint64_t rawHeadBudgetBytes = detectionDiskBudgetDefaultBytes({});
  // Empty uses the application log file. A value is a directory, not a file.
  QString logDirectory;

  // storagePath is the project file or directory whose volume holds the disk
  // budgets. Empty uses the current directory.
  static DetectionResourceOptions load(const QString& storagePath = {});
  void save() const;
  void applyTo(DetectionRequest& request) const;
  QString resolvedLogFile() const;
};

class DetectionResourceOptionsDialog final : public QDialog {
  Q_OBJECT

 public:
  explicit DetectionResourceOptionsDialog(QWidget* parent = nullptr,
                                          const QString& storagePath = {});

 public slots:
  void accept() override;

 private:
  void refreshLogPath();
  bool commitDrafts();
  QString previewLogFile() const;

  ParameterInputPolicy* inputPolicy_ = nullptr;
  QCheckBox* retain_ = nullptr;
  QLabel* retentionPolicy_ = nullptr;
  QSpinBox* evidenceMiB_ = nullptr;
  QSpinBox* workingSetMiB_ = nullptr;
  QSpinBox* scratchGiB_ = nullptr;
  QSpinBox* rawHeadGiB_ = nullptr;
  QLineEdit* logDirectory_ = nullptr;
  QLabel* logPath_ = nullptr;
  QString storagePath_;
};

}  // namespace signalstudio
