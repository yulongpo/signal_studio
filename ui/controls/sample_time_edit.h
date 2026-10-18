#pragma once

#include <cmath>

#include "domain/annotation.h"
#include "ui/controls/adaptive_value_edit.h"

namespace signalstudio {

// Millisecond presentation with an exact sample backing value. Displaying or
// accepting an unchanged value never converts a uint64 sample through double.
class SampleTimeEdit final : public QLineEdit, public ParameterDraft {
 public:
  SampleTimeEdit(SampleIndex sample, double sampleRate,
                 QWidget* parent = nullptr)
      : QLineEdit(parent), sample_(sample), sampleRate_(sampleRate) {
    restoreText();
    connect(this, &QLineEdit::textEdited, this,
            [this] { setProperty("parameterDraft", true); });
  }
  std::function<void()> applied;
  SampleIndex sampleValue() const { return sample_; }
  const QString& error() const { return error_; }
  void setSampleValue(SampleIndex sample) {
    sample_ = sample;
    error_.clear();
    setProperty("invalid", false);
    restoreText();
  }
  void commit() override {
    if (!property("parameterDraft").toBool()) return;
    setProperty("parameterDraft", false);
    if (text() == displayedText_) {
      error_.clear();
      setProperty("invalid", false);
      if (applied) applied();
      return;
    }
    const auto milliseconds = parseAdaptiveValue(text(), UnitKind::Scalar);
    if (!milliseconds || *milliseconds < 0 || !std::isfinite(sampleRate_) ||
        sampleRate_ <= 0) {
      error_ = "请输入有效的非负毫秒数。";
    } else {
      const auto samples = std::round(static_cast<long double>(*milliseconds) /
                                      1000 * sampleRate_);
      if (samples < 0 || samples >= std::ldexp(1.0L, 64))
        error_ = "时间超出 uint64 样本范围。";
      else {
        sample_ = static_cast<SampleIndex>(samples);
        error_.clear();
        restoreText();
      }
    }
    setProperty("invalid", !error_.isEmpty());
    if (applied) applied();
  }
  void discard() override {
    error_.clear();
    setProperty("invalid", false);
    restoreText();
    if (applied) applied();
  }

 protected:
  void keyPressEvent(QKeyEvent* event) override {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
      commit();
      event->accept();
    } else if (event->key() == Qt::Key_Escape) {
      discard();
      event->accept();
    } else {
      QLineEdit::keyPressEvent(event);
    }
  }
  void focusOutEvent(QFocusEvent* event) override {
    if (event->reason() == Qt::TabFocusReason ||
        event->reason() == Qt::BacktabFocusReason)
      commit();
    QLineEdit::focusOutEvent(event);
  }
  void wheelEvent(QWheelEvent* event) override { event->ignore(); }

 private:
  void restoreText() {
    setProperty("parameterDraft", false);
    displayedText_ = QString::number(
        static_cast<double>(sample_) / sampleRate_ * 1000, 'g', 17);
    setText(displayedText_);
    setToolTip(QString("样本 %1；编辑按最近样本量化，未编辑保留精确样本。")
                   .arg(sample_));
  }
  SampleIndex sample_;
  double sampleRate_;
  QString displayedText_, error_;
};

}  // namespace signalstudio
