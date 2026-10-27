#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QCloseEvent;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSpinBox;

namespace signalstudio {
class ParameterInputPolicy;
class ParameterTextEdit;

struct FfscnEngineRequest {
  QString pythonPath;
  QString checkpointPath;
  QString trtRoot;
  QString outputDirectory;
  QString repoRoot;
  QString scriptPath;
  QString applicationDirectory;
  int timeFrames = 10;
  int frequencyBins = 65536;
  QString precision = "FP32";
  int workspaceMiB = 1024;
};

// Path and shape checks only. This does not import PyTorch or TensorRT.
QStringList ffscnEngineRequestErrors(const FfscnEngineRequest& request);

// Non-modal Tools dialog. Conversion runs in a spawned Python process.
class FfscnEngineDialog final : public QDialog {
  Q_OBJECT
 public:
  explicit FfscnEngineDialog(QWidget* parent = nullptr,
                             bool restoreSettings = true);
  QStringList requestErrors() const;
  QString summaryText() const;
  bool conversionRunning() const;
  int startedProcesses() const;

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void buildUi();
  void applyDefaults();
  void loadSettings();
  void saveSettings() const;
  void commitVisibleDrafts();
  FfscnEngineRequest currentRequest() const;
  void validateInputs();
  void startConversion();
  void startProcess(bool convert);
  void cancelConversion();
  void consumeProcessOutput();
  void finishConversion(int exitCode, int status);
  void showReport(int exitCode, int status);
  void appendLog(const QString& text);
  void setBusy(bool busy);
  QString repoRoot() const;
  QString scriptPath() const;

  bool restoreSettings_ = true;
  bool busy_ = false;
  int startedProcesses_ = 0;
  ParameterInputPolicy* inputPolicy_ = nullptr;
  ParameterTextEdit* python_ = nullptr;
  ParameterTextEdit* checkpoint_ = nullptr;
  ParameterTextEdit* trtRoot_ = nullptr;
  ParameterTextEdit* output_ = nullptr;
  QSpinBox* timeFrames_ = nullptr;
  QComboBox* frequencyBins_ = nullptr;
  QComboBox* precision_ = nullptr;
  QSpinBox* workspace_ = nullptr;
  QLabel* summary_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
  QPushButton* validateButton_ = nullptr;
  QPushButton* convertButton_ = nullptr;
  QPushButton* cancelButton_ = nullptr;
  QProcess* process_ = nullptr;
  QString logRemainder_;
  QString reportPath_;
};
}  // namespace signalstudio
