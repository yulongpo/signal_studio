#pragma once
#include <QRegularExpression>
#include <cmath>
#include <optional>

#include "ui/parameter_input.h"

namespace signalstudio {
enum class UnitKind { Frequency, SampleRate, Time, Scalar };
std::optional<double> parseAdaptiveValue(const QString&, UnitKind,
                                         bool allowNegative = false);
QString formatAdaptiveValue(double, UnitKind);
class AdaptiveValueEdit final : public QLineEdit, public ParameterDraft {
  Q_OBJECT
 public:
  AdaptiveValueEdit(UnitKind kind, double value, QWidget* parent = nullptr);
  double value() const { return value_; }
  void setAllowNegative(bool allow) { allowNegative_ = allow; }
  void setValue(double);
  void commit() override;
  void discard() override;
 signals:
  void valueCommitted(double);

 protected:
  void keyPressEvent(QKeyEvent*) override;
  void focusOutEvent(QFocusEvent*) override;
  void wheelEvent(QWheelEvent* e) override { e->ignore(); }

 private:
  UnitKind kind_;
  double value_;
  bool allowNegative_ = false;
};
}  // namespace signalstudio
