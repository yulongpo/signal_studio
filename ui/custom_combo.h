#pragma once
#include <QComboBox>
#include <QLineEdit>
#include <QSettings>
#include <QSignalBlocker>
#include <QApplication>
#include <QKeyEvent>
#include <QTimer>
#include <limits>
namespace signalstudio {
inline QString numericComboText(QComboBox* combo,double value,const QString& suffix,bool exact=false) {
    const auto decimals=combo->property("numericDisplayDecimals");
    if(decimals.isValid()){
        const auto compact=QString::number(value,'f',decimals.toInt());
        if(!exact||compact.toDouble()==value)return compact+suffix;
    }
    return QString::number(value,'g',exact?std::numeric_limits<double>::max_digits10:12)+suffix;
}

// Presentation never writes rounded text back to the numeric model. Entering
// the editor reveals the exact committed value; drafts remain the input policy's responsibility.
class NumericComboPresentation final : public QObject {
 public:
    NumericComboPresentation(QComboBox* combo,int decimals):QObject(combo),combo_(combo) {
        combo_->setProperty("numericDisplayDecimals",decimals);
        combo_->lineEdit()->installEventFilter(this);
        combo_->installEventFilter(this);
        connect(combo_->lineEdit(),&QLineEdit::textEdited,this,[this]{editing_=true;});
    }
    void setValue(double value,const QString& suffix) {
        value_=value;suffix_=suffix;ready_=true;refresh();
    }
 protected:
    bool eventFilter(QObject*,QEvent* event) override {
        if(event->type()==QEvent::FocusIn){editing_=true;refresh();}
        else if(event->type()==QEvent::FocusOut){editing_=false;schedule();}
        else if(event->type()==QEvent::KeyPress){
            const auto key=static_cast<QKeyEvent*>(event)->key();
            if(key==Qt::Key_Return||key==Qt::Key_Enter||key==Qt::Key_Escape){editing_=false;schedule();}
        } else if(event->type()==QEvent::DynamicPropertyChange||event->type()==QEvent::Hide)schedule();
        return false;
    }
 private:
    void refresh() {
        if(!ready_||combo_->property("parameterDraft").toBool())return;
        const auto compact=numericComboText(combo_,value_,suffix_);
        const auto exact=numericComboText(combo_,value_,suffix_,true);
        const bool editing=editing_&&(combo_->hasFocus()||combo_->lineEdit()->hasFocus());
        const auto text=editing?exact:compact;
        if(combo_->property("parameterCommittedText").toString()!=compact)combo_->setProperty("parameterCommittedText",compact);
        const QSignalBlocker block(combo_),edit(combo_->lineEdit());
        if(combo_->currentText()!=text){combo_->setEditText(text);combo_->lineEdit()->setCursorPosition(0);}
        combo_->setToolTip(combo_->accessibleName()+"\n"+exact);
    }
    void schedule() {
        if(pending_)return;pending_=true;
        QTimer::singleShot(0,this,[this]{pending_=false;refresh();});
    }
    QComboBox* combo_;double value_=0;QString suffix_;bool ready_=false,editing_=false,pending_=false;
};
inline NumericComboPresentation* numericComboPresentation(QComboBox* combo) {
    for(auto* child:combo->children())if(auto* presentation=dynamic_cast<NumericComboPresentation*>(child))return presentation;
    return nullptr;
}
inline void enableCompactNumericCombo(QComboBox* combo,int decimals=2) {
    if(!numericComboPresentation(combo))new NumericComboPresentation(combo,decimals);
}
inline void rememberCustomValue(QComboBox* combo, double value, const QString& suffix) {
    const QSignalBlocker block(combo), edit(combo->lineEdit());
    const auto text=combo->currentText();const int cursor=combo->lineEdit()->cursorPosition();
    const int selection=combo->lineEdit()->selectionStart(),length=combo->lineEdit()->selectedText().size();
    if(!combo->property("hasCustomRow").toBool()){combo->insertItem(0,{},value);combo->insertSeparator(1);combo->setProperty("hasCustomRow",true);}
    combo->setItemText(0,numericComboText(combo,value,suffix));combo->setItemData(0,value);
    combo->setItemData(0,QStringLiteral("最近自定义值（手动输入或自动适配）\n")+numericComboText(combo,value,suffix,true),Qt::ToolTipRole);
    combo->setProperty("lastCustomValue",value);
    if(qApp->property("uiStatePersistenceEnabled").toBool())QSettings().setValue("customCombos/"+combo->objectName(),value);
    combo->setEditText(text);combo->lineEdit()->setCursorPosition(cursor);
    if(selection>=0)combo->lineEdit()->setSelection(selection,length);
}
inline void restoreCustomValue(QComboBox* combo,const QString& suffix) {
    if(!qApp->property("uiStatePersistenceEnabled").toBool())return;
    QSettings settings;const auto key="customCombos/"+combo->objectName();
    if(settings.contains(key))rememberCustomValue(combo,settings.value(key).toDouble(),suffix);
}
inline void displayComboValue(QComboBox* combo,double value,const QString& suffix) {
    combo->setProperty("parameterCommittedText",numericComboText(combo,value,suffix));
    if(combo->property("parameterDraft").toBool())return;
    const QSignalBlocker block(combo),edit(combo->lineEdit());
    int index=combo->findData(value);
    if(index<0){rememberCustomValue(combo,value,suffix);index=0;}
    combo->setCurrentIndex(index);
    if(auto* presentation=numericComboPresentation(combo)){
        for(int row=0;row<combo->count();++row){
            if(!combo->itemData(row).isValid())continue;
            const double itemValue=combo->itemData(row).toDouble();
            const auto itemText=numericComboText(combo,itemValue,suffix);
            if(combo->itemText(row)!=itemText)combo->setItemText(row,itemText);
            combo->setItemData(row,numericComboText(combo,itemValue,suffix,true),Qt::ToolTipRole);
        }
        presentation->setValue(value,suffix);
    }
}
}
