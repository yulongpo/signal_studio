#pragma once

#include <QDialog>
#include <optional>

#include "infrastructure/label_import.h"

class QComboBox;
class QLabel;
namespace signalstudio {
class ParameterTextEdit;

// Pure configuration UI. The bounded preview is supplied by infrastructure;
// this dialog neither reads files nor parses/publishes annotations.
class LabelMappingDialog final : public QDialog {
 public:
  LabelMappingDialog(const QString& labelPath, const QString& boundedPreview,
                     QWidget* parent = nullptr);
  std::optional<LabelFieldMapping> mapping() const { return accepted_; }

 protected:
  void accept() override;

 private:
  LabelFieldMapping draft_;
  std::optional<LabelFieldMapping> accepted_;
  QComboBox *delimiter_, *timeUnit_, *interval_, *origin_;
  QComboBox *frequencyLayout_, *frequencyUnit_, *frequencyReference_;
  ParameterTextEdit *comment_, *hop_, *length_, *quantization_;
  QLabel* error_;
};
}  // namespace signalstudio
