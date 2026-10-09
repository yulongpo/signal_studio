#pragma once

#include "application/session.h"
#include "infrastructure/channel_processor.h"

#include <QImage>
#include <QJsonObject>
#include <QWidget>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTimer;

class QStackedWidget;

namespace signalstudio {

class NarrowbandChart;

class NarrowbandWorkspace final : public QWidget {
    Q_OBJECT
public:
    explicit NarrowbandWorkspace(Session& session, QWidget* parent = nullptr);
    ~NarrowbandWorkspace() override;

    void setCallbacks(std::function<void()> locateSource, std::function<void()> editChannel,
                      std::function<void()> returnToWide, std::function<void(const QString&)> log);
    void refreshFromSession();
    void cancelWork();
    QString dataStatusText() const;
    int currentPageIndex() const;
    void activatePageForAcceptance(int index);
    bool visibleChartsSettled() const;
    bool visibleChartsGpuReady() const;
    quint64 visibleTextureUploads() const;
    quint64 visibleCompletedFrames() const;
    quint64 visibleGpuDataDrawCalls() const;
    quint64 visibleGpuVertexUploads() const;
    QString visibleBackendDescription() const;
    void invalidateVisibleOverlays();

private:
    void buildPages();
    void showPage(NarrowbandPage page);
    void updateFrequencyAxisLabels();
    void finishChartWheel();
    void cancelChartWheel();
    void requestDisplay();
    void installFrame(std::uint64_t generation, std::uint64_t configVersion,
                      std::vector<float> waveformI, std::vector<float> waveformQ,
                      std::vector<float> psd, QImage stft, QString status, bool samplePointsVisible,
                      bool psdReady, bool stftReady, std::uint64_t firstOutputSample);
    void startDemoRecognition();
    void stopRecognition();
    void stopDemoRecognition();
    void updateRecognition();
    void exportRecognition();
    void exportBitstream();
    void copyBitstream();
    void updateCaption();
    void markRecognitionStale(const QString& reason);
    void updateRecognitionDetail();

    struct RecognitionSegment {
        std::uint64_t sourceBegin = 0;
        std::uint64_t sourceEnd = 0;
        std::array<double, 5> scores{};
        int topClass = 0;
    };

    Session& session_;
    QStackedWidget* pages_ = nullptr;
    std::array<QWidget*, 4> pageWidgets_{};
    std::array<QPushButton*, 4> pageButtons_{};
    std::vector<NarrowbandChart*> charts_;
    NarrowbandChart* navigation_ = nullptr;
    NarrowbandChart *waveform_ = nullptr, *psd_ = nullptr, *stft_ = nullptr;
    NarrowbandChart *modulationStft_ = nullptr, *recognitionStft_ = nullptr;
    NarrowbandChart *constellationSmall_ = nullptr, *constellationLarge_ = nullptr, *constellationDemod_ = nullptr;
    NarrowbandChart *eyeSmall_ = nullptr, *eyeLarge_ = nullptr, *eyeDemod_ = nullptr, *timeline_ = nullptr;
    QLabel *caption_ = nullptr, *dataStatus_ = nullptr, *analysisStatus_ = nullptr;
    QLabel *recognitionTop5_ = nullptr;
    QProgressBar* recognitionProgress_ = nullptr;
    QPushButton *runRecognition_ = nullptr, *stopRecognition_ = nullptr;
    QComboBox *model_ = nullptr, *fftSize_ = nullptr, *waveformMode_ = nullptr;
    QComboBox *displayPsdFft_ = nullptr, *displayStftFft_ = nullptr, *frequencyMode_ = nullptr;
    QComboBox *eyeComponent_ = nullptr, *eyePeriods_ = nullptr, *eyeTraces_ = nullptr;
    QCheckBox* grid_ = nullptr;
    QComboBox *recognitionScope_ = nullptr, *recognitionNormalization_ = nullptr, *recognitionOverlap_ = nullptr;
    QDoubleSpinBox *recognitionSampleRate_ = nullptr, *recognitionThreshold_ = nullptr;
    QCheckBox* recognitionAllowResample_ = nullptr;
    QPlainTextEdit* bitstream_ = nullptr;
    QTimer* recognitionTimer_ = nullptr;
    QTimer* chartWheelTimer_ = nullptr;
    std::optional<ChannelViewSnapshot> chartWheelBase_;
    QString selectedModelFile_;
    std::vector<RecognitionSegment> recognitionSegments_;
    std::uint64_t recognitionConfigVersion_ = 0;
    std::string recognitionChannelId_;
    std::string recognitionSourceFileId_;
    std::string recognitionSourceMarkId_;
    std::uint64_t recognitionRangeBegin_ = 0;
    std::uint64_t recognitionRangeEnd_ = 0;
    int selectedRecognitionSegment_ = -1;
    bool recognitionResultStale_ = false;
    std::function<void()> locateSource_, editChannel_, returnToWide_;
    std::function<void(const QString&)> log_;
    std::thread worker_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    std::string lastChannelId_;
    std::string lastSourcePath_;
    std::uint64_t lastConfigVersion_ = 0;
    TimeRange lastVisibleTime_;
    FrequencyRange lastVisibleFrequency_;
    NarrowbandWaveform lastWaveform_ = NarrowbandWaveform::IQ;
    int lastPsdFft_ = 0, lastStftFft_ = 0;
    bool hasDisplayRequest_ = false;
    std::uint64_t requestGeneration_ = 0;
    std::uint64_t frameGeneration_ = 0;
    std::uint64_t recognitionGeneration_ = 0;
    int recognitionProgressValue_ = 0;
    bool recognitionActive_ = false;
    ChannelSampleCache sampleCache_;
};

} // namespace signalstudio
