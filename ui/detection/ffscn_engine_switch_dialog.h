#pragma once

#include <QDialog>
#include <QString>
#include <functional>

#include "infrastructure/ffscn_engine_package.h"

class QLabel;

namespace signalstudio {
class ParameterInputPolicy;
class ParameterTextEdit;

// Confirms an explicit FFSCN package selection. Conversion output is not
// installed from here, and an active detection run is left on its current engine.
class FfscnEngineSwitchDialog final : public QDialog {
  Q_OBJECT
 public:
  explicit FfscnEngineSwitchDialog(QWidget* parent = nullptr,
                                   QString modelRoot = {});
  void setBusyCheck(std::function<bool()> busy);
  bool switchApplied() const { return applied_; }
  FfscnActivePackage activatedPackage() const { return activated_; }

 signals:
  void engineActivated(const QString& packageDirectory,
                       const QString& engineSha256);

 private:
  void buildUi();
  void refreshCurrentPackage();
  void commitVisibleDrafts();
  bool detectionBusy() const;
  void refuseWhileBusy();
  void inspectSelection();
  void activateSelection();
  void restoreDeployedPackage();
  bool confirmAndActivate(const QString& directory, const QString& title,
                          const QString& detail);

  QString modelRoot_;
  std::function<bool()> busyCheck_;
  bool applied_ = false;
  FfscnActivePackage activated_;
  ParameterInputPolicy* inputPolicy_ = nullptr;
  ParameterTextEdit* directory_ = nullptr;
  QLabel* current_ = nullptr;
  QLabel* summary_ = nullptr;
};
}  // namespace signalstudio
