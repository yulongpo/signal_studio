#include "ui/spectral_settings_widget.h"
#include "ui/custom_combo.h"
#include "ui/parameter_input.h"
#include "infrastructure/spectral_analysis.h"
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
namespace signalstudio {
SpectralSettingsWidget::SpectralSettingsWidget(bool psd,QWidget* parent):QWidget(parent),psd_(psd) {
    auto* form=new QFormLayout(this);form->setContentsMargins(0,0,0,0);form->setSpacing(8);
    const QString prefix=psd?"psd":"stft";
    method_=new QComboBox;method_->setObjectName(prefix+"Method");
    if(psd)method_->addItems({"FFT 周期图","Bartlett","Welch","DPSS 多窗","AR Burg"});
    else {method_->addItem("普通 STFT",static_cast<int>(SpectralMethod::Welch));method_->addItem("DPSS 多窗",static_cast<int>(SpectralMethod::Multitaper));}
    form->addRow("谱估计方法",method_);
    window_=new QComboBox;window_->setObjectName(prefix+"Window");window_->addItems({"矩形","Hann","Hamming","Blackman","Blackman-Harris","Flat-top","Kaiser"});form->addRow("窗函数",window_);
    const auto editable=[&](QString name,QStringList values){auto* combo=new QComboBox;combo->setObjectName(prefix+name);combo->setEditable(true);combo->setInsertPolicy(QComboBox::NoInsert);combo->setCompleter(nullptr);for(const auto& v:values)combo->addItem(v,v.toDouble());restoreCustomValue(combo,{});return combo;};
    overlap_=editable("Overlap",{"0","25","50","75","90"});form->addRow("重叠 (%)",overlap_);
    duration_=editable("SegmentMs",{"0","0.1","1","10","100"});duration_->setToolTip("0 为自动 N/B；其它值为实际观测窗时长（毫秒）");form->addRow("分段时长 (ms)",duration_);
    if(psd){statistic_=new QComboBox;statistic_->setObjectName("psdStatistic");statistic_->addItems({"平均谱","最大谱","最小谱"});form->addRow("统计类型",statistic_);}
    beta_=new ParameterDoubleSpinBox;beta_->setObjectName(prefix+"KaiserBeta");beta_->setRange(0,30);beta_->setDecimals(3);form->addRow("Kaiser β",beta_);
    nw_=new ParameterDoubleSpinBox;nw_->setObjectName(prefix+"TimeBandwidth");nw_->setRange(.5,32);nw_->setDecimals(2);form->addRow("DPSS NW",nw_);
    tapers_=new ParameterIntSpinBox;tapers_->setObjectName(prefix+"Tapers");tapers_->setRange(1,32);form->addRow("DPSS 窗数",tapers_);
    order_=new ParameterIntSpinBox(this);order_->setObjectName(prefix+"BurgOrder");order_->setRange(1,256);if(psd)form->addRow("Burg 阶数",order_);else order_->hide();
    mean_=new QCheckBox("去除有效样本均值");mean_->setObjectName(prefix+"RemoveMean");form->addRow(mean_);
    const auto changed=[this]{if(updating_)return;updateAvailability();if(validSpectralParameters(parameters())){cacheCurrentMethod();emit edited();}};
    connect(method_,&QComboBox::currentIndexChanged,this,[this]{if(!updating_)changeMethod();});
    for(auto* combo:{window_,statistic_})if(combo)connect(combo,&QComboBox::currentIndexChanged,this,changed);
    for(auto* combo:{overlap_,duration_}){
        bindParameterCombo(combo,[this,combo,changed]{
            if(updating_)return;
            bool ok=false;const double value=combo->currentText().toDouble(&ok);
            auto candidate=parameters();
            if(combo==overlap_)candidate.overlap=value/100;else candidate.segmentMilliseconds=value;
            if(!ok||!validSpectralParameters(candidate)) {
                combo->setProperty("parameterDraft",true);
                setParameterError(combo,combo==overlap_?"重叠率必须是 0（含）到 100（不含）之间的有限数值":"分段时长必须是 0 到 1e9 毫秒之间的有限数值；0 为自动 N/B");
                return;
            }
            combo->setProperty("parameterDraft",false);
            setParameterError(combo,{});
            combo->setProperty("lastValidValue",value);
            if(combo->property("parameterCustomCommit").toBool())rememberCustomValue(combo,value,{});
            displayComboValue(combo,value,{});
            changed();
        });
    }
    for(auto* spin:{beta_,nw_})connect(spin,&QDoubleSpinBox::valueChanged,this,changed);
    for(auto* spin:{tapers_,order_})connect(spin,&QSpinBox::valueChanged,this,changed);
    connect(mean_,&QCheckBox::toggled,this,changed);setParameters({});
}
SpectralParameters SpectralSettingsWidget::parameters() const {
    SpectralParameters p;p.method=activeMethod_;
    const auto exact=[](QDoubleSpinBox* control){
        return control->property("spectralShownValue").isValid()&&
            control->value()==control->property("spectralShownValue").toDouble()
            ?control->property("spectralExactValue").toDouble():control->value();
    };
    p.window=static_cast<SpectralWindow>(window_->currentIndex());
    p.overlap=activeMethod_==SpectralMethod::Bartlett?0:overlap_->property("lastValidValue").toDouble()/100;
    p.segmentMilliseconds=duration_->property("lastValidValue").toDouble();
    p.kaiserBeta=exact(beta_);p.timeBandwidth=exact(nw_);p.tapers=tapers_->value();p.burgOrder=order_->value();p.removeMean=mean_->isChecked();return p;
}
SpectrumStatistic SpectralSettingsWidget::statistic() const{return statistic_?static_cast<SpectrumStatistic>(statistic_->currentIndex()):SpectrumStatistic::Mean;}
QComboBox* SpectralSettingsWidget::takeStatisticControl() {
    if (!statistic_) return nullptr;
    auto* form = qobject_cast<QFormLayout*>(layout());
    if (form && form->indexOf(statistic_) >= 0) {
        const auto taken = form->takeRow(statistic_);
        if (taken.labelItem) { delete taken.labelItem->widget(); delete taken.labelItem; }
        delete taken.fieldItem;
    }
    return statistic_;
}
QComboBox* SpectralSettingsWidget::takeInlineControl(const QString& suffix) {
    auto* control=suffix=="Method"?method_:suffix=="Window"?window_:suffix=="Overlap"?overlap_:nullptr;
    if(!control)return nullptr;
    auto* form=qobject_cast<QFormLayout*>(layout());
    if(form&&form->indexOf(control)>=0){
        const auto taken=form->takeRow(control);
        if(taken.labelItem){delete taken.labelItem->widget();delete taken.labelItem;}
        delete taken.fieldItem;
    }
    return control;
}
void SpectralSettingsWidget::setParameters(const SpectralParameters& p,SpectrumStatistic statistic) {
    if(!validSpectralParameters(p))return;
    auto accepted=p;
    if(!psd_&&accepted.method!=SpectralMethod::Multitaper)accepted.method=SpectralMethod::Welch;
    if(accepted.method==SpectralMethod::Bartlett)accepted.overlap=0;
    updating_=true;
    if(initialized_&&activeMethod_!=accepted.method){
        cacheCurrentMethod();
        // A method change leaves its drafts behind; only committed values
        // belong in history. Ordinary domain refreshes preserve active drafts.
        for(auto* combo:{overlap_,duration_}){
            for(auto* child:combo->children())if(auto* draft=dynamic_cast<ParameterDraft*>(child))draft->discard();
            setParameterError(combo,{});
        }
        for(auto* spin:{static_cast<QAbstractSpinBox*>(beta_),static_cast<QAbstractSpinBox*>(nw_),static_cast<QAbstractSpinBox*>(tapers_),static_cast<QAbstractSpinBox*>(order_)}){
            if(auto* draft=dynamic_cast<ParameterDraft*>(spin))draft->discard();
            setParameterError(spin,{});
        }
    }
    activeMethod_=accepted.method;
    methodHistory_[static_cast<int>(activeMethod_)]=accepted;
    method_->setCurrentIndex(psd_?static_cast<int>(accepted.method):accepted.method==SpectralMethod::Multitaper?1:0);window_->setCurrentIndex(static_cast<int>(accepted.window));
    overlap_->setProperty("lastValidValue",accepted.overlap*100);duration_->setProperty("lastValidValue",accepted.segmentMilliseconds);displayComboValue(overlap_,accepted.overlap*100,{});displayComboValue(duration_,accepted.segmentMilliseconds,{});
    displayParameterValue(beta_,accepted.kaiserBeta);displayParameterValue(nw_,accepted.timeBandwidth);
    // Imported committed values can be more precise than the spin's display.
    // Visiting another method must not turn display rounding into an edit.
    for(auto* control:{beta_,nw_}){
        control->setProperty("spectralExactValue",control==beta_?accepted.kaiserBeta:accepted.timeBandwidth);
        control->setProperty("spectralShownValue",control->value());
    }
    displayParameterValue(tapers_,accepted.tapers);displayParameterValue(order_,accepted.burgOrder);mean_->setChecked(accepted.removeMean);
    for(auto* combo:{overlap_,duration_})if(!combo->property("parameterDraft").toBool())setParameterError(combo,{});
    if(statistic_)statistic_->setCurrentIndex(static_cast<int>(statistic));updateAvailability();updating_=false;
    initialized_=true;
}
void SpectralSettingsWidget::cacheCurrentMethod(){
    if(!initialized_)return;
    const auto p=parameters();
    if(validSpectralParameters(p))methodHistory_[static_cast<int>(activeMethod_)]=p;
}
void SpectralSettingsWidget::changeMethod(){
    const auto selected=psd_?static_cast<SpectralMethod>(method_->currentIndex()):static_cast<SpectralMethod>(method_->currentData().toInt());
    if(selected==activeMethod_)return;
    cacheCurrentMethod();
    auto next=methodHistory_[static_cast<int>(selected)].value_or(SpectralParameters{});
    next.method=selected;
    const auto currentStatistic=statistic();
    setParameters(next,currentStatistic);
    emit edited();
}
void SpectralSettingsWidget::updateAvailability(){const auto p=parameters();const bool mt=p.method==SpectralMethod::Multitaper,burg=p.method==SpectralMethod::Burg,period=p.method==SpectralMethod::Periodogram;
    auto* form=qobject_cast<QFormLayout*>(layout());
    const auto row=[form](QWidget* field,bool visible){if(!field)return;field->setProperty("chartSettingRelevant",visible);if(form&&form->indexOf(field)>=0){form->setRowVisible(field,visible);field->setEnabled(visible);}};
    row(window_,!mt&&!burg);row(beta_,!mt&&!burg&&p.window==SpectralWindow::Kaiser);
    row(nw_,mt);row(tapers_,mt);row(order_,burg);
    row(overlap_,!period&&p.method!=SpectralMethod::Bartlett);row(duration_,!period);row(statistic_,!period);
    method_->setToolTip(p.method==SpectralMethod::Bartlett?"Bartlett：固定 0% 重叠，各段互不重叠":
        burg?"Burg：阶数 1–256，且必须小于真实有效样本数；补零不参与模型拟合":
        mt?"DPSS 多窗：NW 0.5–32，窗数 K 1–32；短数据受有效样本支持限制":
        period?"周期图：单次 FFT，使用窗函数；重叠、分段时长和统计类型不适用":
        "Welch：按分段时长与重叠率估计；短数据的有效观测和补零由分析诊断显示，补零不提高真实分辨率");
    if(!overlap_->property("invalid").toBool())overlap_->setToolTip("重叠率范围 [0, 100%)；短窄带时频图会显示实际重叠");
    if(!duration_->property("invalid").toBool())duration_->setToolTip("0 为自动 N/B；其它值为实际观测窗时长（毫秒），范围 [0, 1e9]；数据不足时显示有效观测和补零量");
}
}
