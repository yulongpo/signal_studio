#pragma once
#include "domain/project.h"
#include <QFile>
#include <complex>
#include <functional>
#include <vector>

namespace signalstudio {
bool describeRawFile(const QString& path,const SampleFormat& format,double fs,double fc,double bandwidth,FileMetadata& target,QString& error);
// Read-only address mapping has no file-sized heap copy. Each decoded read is bounded.
class RawSampleReader {
public:
    ~RawSampleReader();
    bool open(const QString&,const SampleFormat&,QString&);
    std::uint64_t sampleCount() const {return count_;}
    bool isOpen() const {return mapped_!=nullptr;}
    bool sampleAt(std::uint64_t,std::complex<double>&,QString* error=nullptr) const;
    bool read(TimeRange,std::vector<std::complex<float>>&,const std::function<bool()>& cancel={},QString* error=nullptr) const;
private:
    double component(std::uint64_t) const;
    double decode(const uchar*) const;
    QFile file_;
    uchar* mapped_=nullptr;
    SampleFormat format_;
    std::uint64_t count_=0;
};
}
