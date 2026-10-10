#pragma once
#include "ui/parameter_input.h"
#include <QRegularExpression>
#include <cmath>
#include <optional>

namespace signalstudio {
enum class UnitKind {Frequency,SampleRate,Time};
std::optional<double> parseAdaptiveValue(const QString&,UnitKind);
QString formatAdaptiveValue(double,UnitKind);
class AdaptiveValueEdit final:public QLineEdit,public ParameterDraft {
    Q_OBJECT
public:
    AdaptiveValueEdit(UnitKind kind,double value,QWidget* parent=nullptr);
    double value() const {return value_;}
    void setValue(double);
    void commit() override;
    void discard() override;
signals:
    void valueCommitted(double);
protected:
    void keyPressEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void wheelEvent(QWheelEvent* e) override {e->ignore();}
private:
    UnitKind kind_;
    double value_;
};
}
