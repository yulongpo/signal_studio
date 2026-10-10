#include "ui/spectral_settings_widget.h"
#include "ui/custom_combo.h"
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
    beta_=new QDoubleSpinBox;beta_->setObjectName(prefix+"KaiserBeta");beta_->setRange(0,30);beta_->setDecimals(3);form->addRow("Kaiser β",beta_);
    nw_=new QDoubleSpinBox;nw_->setObjectName(prefix+"TimeBandwidth");nw_->setRange(.5,32);nw_->setDecimals(2);form->addRow("DPSS NW",nw_);
    tapers_=new QSpinBox;tapers_->setObjectName(prefix+"Tapers");tapers_->setRange(1,32);form->addRow("DPSS 窗数",tapers_);
    order_=new QSpinBox;order_->setObjectName(prefix+"BurgOrder");order_->setRange(1,256);if(psd)form->addRow("Burg 阶数",order_);else order_->hide();
    mean_=new QCheckBox("去除有效样本均值");mean_->setObjectName(prefix+"RemoveMean");form->addRow(mean_);
    const auto changed=[this]{if(updating_)return;updateAvailability();if(validSpectralParameters(parameters()))emit edited();};
    for(auto* combo:{method_,window_,statistic_})if(combo)connect(combo,&QComboBox::currentIndexChanged,this,changed);
    for(auto* combo:{overlap_,duration_}){
        connect(combo,&QComboBox::editTextChanged,this,changed);
        connect(combo->lineEdit(),&QLineEdit::editingFinished,this,[this,combo]{const QSignalBlocker block(combo);combo->setEditText(QString::number(combo->property("lastValidValue").toDouble(),'g',12));});
        connect(combo->lineEdit(),&QLineEdit::textEdited,this,[this,combo]{bool ok=false;const double v=combo->currentText().toDouble(&ok);if(ok&&validSpectralParameters(parameters())){combo->setProperty("lastValidValue",v);rememberCustomValue(combo,v,{});}});
    }
    for(auto* spin:{beta_,nw_})connect(spin,&QDoubleSpinBox::valueChanged,this,changed);
    for(auto* spin:{tapers_,order_})connect(spin,&QSpinBox::valueChanged,this,changed);
    connect(mean_,&QCheckBox::toggled,this,changed);setParameters({});
}
SpectralParameters SpectralSettingsWidget::parameters() const {
    SpectralParameters p;p.method=psd_?static_cast<SpectralMethod>(method_->currentIndex()):static_cast<SpectralMethod>(method_->currentData().toInt());
    p.window=static_cast<SpectralWindow>(window_->currentIndex());bool ok1=false,ok2=false;
    p.overlap=overlap_->currentText().toDouble(&ok1)/100;p.segmentMilliseconds=duration_->currentText().toDouble(&ok2);
    if(!ok1)p.overlap=-1;if(!ok2)p.segmentMilliseconds=-1;
    p.kaiserBeta=beta_->value();p.timeBandwidth=nw_->value();p.tapers=tapers_->value();p.burgOrder=order_->value();p.removeMean=mean_->isChecked();return p;
}
SpectrumStatistic SpectralSettingsWidget::statistic() const{return statistic_?static_cast<SpectrumStatistic>(statistic_->currentIndex()):SpectrumStatistic::Mean;}
void SpectralSettingsWidget::setParameters(const SpectralParameters& p,SpectrumStatistic statistic) {
    updating_=true;method_->setCurrentIndex(psd_?static_cast<int>(p.method):p.method==SpectralMethod::Multitaper?1:0);window_->setCurrentIndex(static_cast<int>(p.window));
    overlap_->setProperty("lastValidValue",p.overlap*100);duration_->setProperty("lastValidValue",p.segmentMilliseconds);displayComboValue(overlap_,p.overlap*100,{});displayComboValue(duration_,p.segmentMilliseconds,{});
    beta_->setValue(p.kaiserBeta);nw_->setValue(p.timeBandwidth);tapers_->setValue(p.tapers);order_->setValue(p.burgOrder);mean_->setChecked(p.removeMean);
    if(statistic_)statistic_->setCurrentIndex(static_cast<int>(statistic));updateAvailability();updating_=false;
}
void SpectralSettingsWidget::updateAvailability(){const auto p=parameters();const bool mt=p.method==SpectralMethod::Multitaper,burg=p.method==SpectralMethod::Burg,period=p.method==SpectralMethod::Periodogram;
    window_->setEnabled(!mt&&!burg);beta_->setEnabled(!mt&&!burg&&p.window==SpectralWindow::Kaiser);nw_->setEnabled(mt);tapers_->setEnabled(mt);order_->setEnabled(burg);
    if(p.method==SpectralMethod::Bartlett){const QSignalBlocker block(overlap_);overlap_->setCurrentIndex(overlap_->findData(0.0));}
    overlap_->setEnabled(!period&&p.method!=SpectralMethod::Bartlett);duration_->setEnabled(!period);
    if(statistic_)statistic_->setEnabled(!period);
    overlap_->setToolTip(p.method==SpectralMethod::Bartlett?"Bartlett 固定零重叠":"配置重叠；短窄带时频图会显示实际重叠");
}
}
