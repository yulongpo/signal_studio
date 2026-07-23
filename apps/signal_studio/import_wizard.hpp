#pragma once

#include "application.hpp"

#include <QDialog>

class QDoubleSpinBox;

namespace signal::studio {

class ImportWizard final : public QDialog {
  Q_OBJECT
 public:
  explicit ImportWizard(QWidget* parent = nullptr);
  void applyFilenameHint(const FilenameHint& hint);
  [[nodiscard]] double sampleRateHz() const;
  [[nodiscard]] double centerFrequencyHz() const;

 private:
  QDoubleSpinBox* sampleRate_{nullptr};
  QDoubleSpinBox* centerFreq_{nullptr};
};

}  // namespace signal::studio
