#pragma once
#include "domain/project.h"
#include <QWidget>
#include <array>
#include <optional>
class QComboBox;class QCheckBox;class QSpinBox;class QDoubleSpinBox;
namespace signalstudio {
class SpectralSettingsWidget:public QWidget {
    Q_OBJECT
public:
    SpectralSettingsWidget(bool psd,QWidget* parent=nullptr);
    SpectralParameters parameters() const;
    SpectrumStatistic statistic() const;
    QComboBox* takeStatisticControl();
    void setParameters(const SpectralParameters&,SpectrumStatistic=SpectrumStatistic::Mean);
signals:
    void edited();
private:
    void updateAvailability();
    void cacheCurrentMethod();
    void changeMethod();
    bool psd_,updating_=false;
    bool initialized_=false;
    SpectralMethod activeMethod_=SpectralMethod::Welch;
    std::array<std::optional<SpectralParameters>,5> methodHistory_;
    QComboBox *method_,*window_,*overlap_,*duration_,*statistic_=nullptr;
    QDoubleSpinBox *beta_,*nw_;QSpinBox *tapers_,*order_;QCheckBox* mean_;
};
}
