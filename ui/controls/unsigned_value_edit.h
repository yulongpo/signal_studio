#pragma once
#include "ui/parameter_input.h"
#include <QRegularExpressionValidator>
#include <limits>

namespace signalstudio {
class UnsignedValueEdit final:public QLineEdit,public ParameterDraft {
public:
    explicit UnsignedValueEdit(std::uint64_t value,QWidget* parent=nullptr):QLineEdit(parent){setValue(value);setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,20}"),this));connect(this,&QLineEdit::textEdited,this,[this]{setProperty("parameterDraft",true);});}
    std::function<void()> applied;
    std::uint64_t minimum=0,maximum=std::numeric_limits<std::uint64_t>::max();
    void setValue(std::uint64_t value){value_=value;discard();}
    void commit() override {if(!property("parameterDraft").toBool())return;bool ok=false;const auto n=text().toULongLong(&ok);if(ok&&n>=minimum&&n<=maximum){value_=n;discard();if(applied)applied();}else discard();}
    void discard() override {setProperty("parameterDraft",false);setText(QString::number(value_));}
protected:
    void keyPressEvent(QKeyEvent* e) override {if(e->key()==Qt::Key_Return||e->key()==Qt::Key_Enter){commit();e->accept();}else if(e->key()==Qt::Key_Escape){discard();e->accept();}else QLineEdit::keyPressEvent(e);}
    void focusOutEvent(QFocusEvent* e) override {if(e->reason()==Qt::MouseFocusReason||e->reason()==Qt::TabFocusReason||e->reason()==Qt::BacktabFocusReason)commit();QLineEdit::focusOutEvent(e);}
    void wheelEvent(QWheelEvent* e) override {e->ignore();}
private:
    std::uint64_t value_=0;
};
}
