#include "ui/controls/adaptive_value_edit.h"
#include <array>
#include <limits>

namespace signalstudio {
std::optional<double> parseAdaptiveValue(const QString& text,UnitKind kind){
    const auto match=QRegularExpression("^\\s*([+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?)\\s*([^\\s]*)\\s*$").match(text);
    if(!match.hasMatch())return {};bool ok=false;double v=match.captured(1).toDouble(&ok);if(!ok||!std::isfinite(v))return {};
    auto unit=match.captured(2).toLower();double scale=1;
    if(kind==UnitKind::Time){
        if(unit.isEmpty()||unit=="ms")scale=.001;else if(unit=="s")scale=1;else if(unit=="us"||unit==QString::fromUtf8("µs"))scale=1e-6;
        else if(unit=="ns")scale=1e-9;else if(unit=="min")scale=60;else if(unit=="h")scale=3600;else return {};
    }else {
        const QString suffix=kind==UnitKind::Frequency?"hz":"s/s";
        if(kind==UnitKind::SampleRate&&unit.endsWith("sps"))unit=unit.left(unit.size()-3)+"s/s";
        if(unit==suffix||unit.isEmpty())scale=1;
        else if(unit=="k"||unit=="k"+suffix)scale=1e3;
        else if(unit=="m"||unit=="m"+suffix)scale=1e6;
        else if(unit=="g"||unit=="g"+suffix)scale=1e9;
        else return {};
    }
    v*=scale;if(!std::isfinite(v)||v<0||(kind==UnitKind::SampleRate&&v==0))return {};return v;
}
QString formatAdaptiveValue(double value,UnitKind kind){
    double scale=1;QString unit=kind==UnitKind::SampleRate?"S/s":"Hz";
    if(kind==UnitKind::Time){scale=.001;unit="ms";if(value>=1){scale=1;unit="s";}else if(value>0&&value<.001){scale=1e-6;unit="us";}}
    else if(std::abs(value)>=1e9){scale=1e9;unit=kind==UnitKind::SampleRate?"GS/s":"GHz";}
    else if(std::abs(value)>=1e6){scale=1e6;unit=kind==UnitKind::SampleRate?"MS/s":"MHz";}
    else if(std::abs(value)>=1e3){scale=1e3;unit=kind==UnitKind::SampleRate?"kS/s":"kHz";}
    // Preserve fractional device metadata: round only when exact integer base units.
    const int precision=kind==UnitKind::Time?9:scale==1?0:scale==1e3?3:scale==1e6?6:9;
    QString numeric=std::floor(value)==value?QString::number(value/scale,'f',precision):QString::number(value/scale,'g',17);
    if(numeric.contains('.')&&!numeric.contains('e')){while(numeric.endsWith('0'))numeric.chop(1);if(numeric.endsWith('.'))numeric.chop(1);}
    return numeric+" "+unit;
}
AdaptiveValueEdit::AdaptiveValueEdit(UnitKind kind,double value,QWidget* parent):QLineEdit(parent),kind_(kind),value_(value){
    setText(formatAdaptiveValue(value_,kind_));setMinimumWidth(0);setToolTip(kind==UnitKind::SampleRate?"S/s、kS/s、MS/s、GS/s；无单位按 S/s":kind==UnitKind::Time?"ns、us、ms、s；无单位按 ms":"Hz、kHz、MHz、GHz；无单位按 Hz");
    connect(this,&QLineEdit::textEdited,this,[this]{setProperty("parameterDraft",true);});
}
void AdaptiveValueEdit::setValue(double value){value_=value;if(!property("parameterDraft").toBool())setText(formatAdaptiveValue(value_,kind_));}
void AdaptiveValueEdit::commit(){
    if(!property("parameterDraft").toBool())return;
    const auto parsed=parseAdaptiveValue(text(),kind_);setProperty("parameterDraft",false);
    if(!parsed){setProperty("invalid",true);setText(formatAdaptiveValue(value_,kind_));return;}
    setProperty("invalid",false);value_=*parsed;setText(formatAdaptiveValue(value_,kind_));emit valueCommitted(value_);
}
void AdaptiveValueEdit::discard(){setProperty("parameterDraft",false);setText(formatAdaptiveValue(value_,kind_));}
void AdaptiveValueEdit::keyPressEvent(QKeyEvent* event){if(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter){commit();event->accept();}else if(event->key()==Qt::Key_Escape){discard();event->accept();}else QLineEdit::keyPressEvent(event);}
void AdaptiveValueEdit::focusOutEvent(QFocusEvent* event){if(event->reason()==Qt::MouseFocusReason||event->reason()==Qt::TabFocusReason||event->reason()==Qt::BacktabFocusReason)commit();QLineEdit::focusOutEvent(event);}
}
