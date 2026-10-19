#pragma once
#include "domain/project.h"
#include <QWidget>
class QComboBox;class QCheckBox;class QSpinBox;class QDoubleSpinBox;
namespace signalstudio {
class SpectralSettingsWidget:public QWidget {
    Q_OBJECT
public:
    SpectralSettingsWidget(bool psd,QWidget* parent=nullptr);
    SpectralParameters parameters() const;
    SpectrumStatistic statistic() const;
    void setParameters(const SpectralParameters&,SpectrumStatistic=SpectrumStatistic::Mean);
signals:
    void edited();
private:
    void updateAvailability();
    bool psd_,updating_=false;
    QComboBox *method_,*window_,*overlap_,*duration_,*statistic_=nullptr;
    QDoubleSpinBox *beta_,*nw_;QSpinBox *tapers_,*order_;QCheckBox* mean_;
};
}
