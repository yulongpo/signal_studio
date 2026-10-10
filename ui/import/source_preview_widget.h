#pragma once
#include "domain/project.h"
#include "domain/spectral_data.h"
#include <QWidget>
#include <atomic>
#include <condition_variable>
#include <optional>
#include <thread>
#include <complex>

namespace signalstudio {
class SourcePreviewWidget final:public QWidget {
    Q_OBJECT
public:
    explicit SourcePreviewWidget(QWidget* parent=nullptr);
    ~SourcePreviewWidget() override;
    void preview(FileMetadata metadata,SampleIndex start=0);
    void clear();
    bool ready() const {return data_.has_value();}
    QString error() const {return error_;}
signals:
    void previewFinished();
protected:
    void paintEvent(QPaintEvent*) override;
private:
    struct Request {FileMetadata metadata;SampleIndex start=0;std::uint64_t generation=0;};
    struct Data {FileMetadata metadata;SampleIndex begin=0;std::vector<std::complex<float>> samples;std::shared_ptr<SpectralFrame> psd;std::shared_ptr<SpectrogramData> stft;};
    void run();
    std::atomic_bool stop_=false;
    std::atomic_uint64_t generation_=0;
    std::mutex mutex_;std::condition_variable condition_;std::optional<Request> pending_;
    std::thread worker_;
    std::optional<Data> data_;QString error_,status_="未选择数据文件";
};
}
