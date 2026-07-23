#include "import_wizard.hpp"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace signal::studio {

ImportWizard::ImportWizard(QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("Import SC16 (RAW IQ)"));
  setMinimumWidth(360);

  auto* form = new QFormLayout;
  sampleRate_ = new QDoubleSpinBox(this);
  sampleRate_->setRange(1.0, 1.0e12);
  sampleRate_->setSuffix(" Hz");
  sampleRate_->setDecimals(3);
  centerFreq_ = new QDoubleSpinBox(this);
  centerFreq_->setRange(0.0, 1.0e12);
  centerFreq_->setSuffix(" Hz");
  centerFreq_->setDecimals(3);
  form->addRow(tr("Sample rate:"), sampleRate_);
  form->addRow(tr("Center frequency:"), centerFreq_);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto* layout = new QVBoxLayout(this);
  auto* label = new QLabel(tr("Confirm capture parameters parsed from the filename."), this);
  layout->addWidget(label);
  layout->addLayout(form);
  layout->addWidget(buttons);
}

void ImportWizard::applyFilenameHint(const FilenameHint& hint) {
  if (hint.had_sample_rate)
    sampleRate_->setValue(hint.sample_rate_hz);
  if (hint.had_center_frequency)
    centerFreq_->setValue(hint.center_frequency_hz);
}

double ImportWizard::sampleRateHz() const {
  return sampleRate_->value();
}
double ImportWizard::centerFrequencyHz() const {
  return centerFreq_->value();
}

} // namespace signal::studio
