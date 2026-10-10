#pragma once
#include <QComboBox>
#include <QLineEdit>
#include <QSettings>
#include <QSignalBlocker>
#include <QApplication>
namespace signalstudio {
inline void rememberCustomValue(QComboBox* combo, double value, const QString& suffix) {
    const QSignalBlocker block(combo), edit(combo->lineEdit());
    const auto text=combo->currentText();const int cursor=combo->lineEdit()->cursorPosition();
    const int selection=combo->lineEdit()->selectionStart(),length=combo->lineEdit()->selectedText().size();
    if(!combo->property("hasCustomRow").toBool()){combo->insertItem(0,{},value);combo->insertSeparator(1);combo->setProperty("hasCustomRow",true);}
    combo->setItemText(0,QString::number(value,'g',12)+suffix);combo->setItemData(0,value);
    combo->setItemData(0,QStringLiteral("最近自定义值（手动输入或自动适配）"),Qt::ToolTipRole);
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
    if(combo->lineEdit()->hasFocus())return;
    const QSignalBlocker block(combo);
    int index=combo->findData(value);
    if(index<0){rememberCustomValue(combo,value,suffix);index=0;}
    combo->setCurrentIndex(index);
}
}
