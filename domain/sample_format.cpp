#include "domain/sample_format.h"
#include <array>
#include <cmath>

namespace signalstudio {
std::uint64_t bytesPerComponent(const SampleFormat& f) {
    const auto e = static_cast<unsigned>(f.componentEncoding);
    constexpr std::array<std::uint64_t,6> sizes{1,1,2,4,4,8};
    return e < sizes.size() ? sizes[e] : 0;
}
std::uint64_t bytesPerChannelSample(const SampleFormat& f) { return bytesPerComponent(f)*(f.structure==SampleStructure::ComplexIQ?2:1); }
std::uint64_t bytesPerFrame(const SampleFormat& f) { return bytesPerChannelSample(f)*f.channels; }
bool validateSampleFormat(const SampleFormat& f, std::string& error) {
    error.clear();
    if (static_cast<unsigned>(f.structure)>1 || static_cast<unsigned>(f.componentEncoding)>5 ||
        static_cast<unsigned>(f.byteOrder)>2 || static_cast<unsigned>(f.iqLayout)>3 ||
        static_cast<unsigned>(f.channelLayout)>1) error="格式枚举无效";
    else if (!f.channels || f.channels>128 || f.selectedChannel>=f.channels) error="通道数或选中通道无效";
    else if ((bytesPerComponent(f)==1)!=(f.byteOrder==ByteOrder::NotApplicable)) error="字节序与分量宽度不一致";
    else if ((f.structure==SampleStructure::Real)!=(f.iqLayout==IQLayout::NotApplicable)) error="实数/复数与 I/Q 布局不一致";
    else if (f.componentEncoding>=ComponentEncoding::Float32 && f.normalizeIntegerAdc) error="浮点不能采用整数 ADC 归一化";
    return error.empty();
}
bool sampleCountForBytes(const SampleFormat& f,std::uint64_t size,std::uint64_t& count,std::string& error) {
    count=0;
    if(!validateSampleFormat(f,error))return false;
    if(f.headerBytes>size || f.trailerBytes>size-f.headerBytes){error="头尾偏移超出物理文件";return false;}
    const auto payload=size-f.headerBytes-f.trailerBytes, frame=bytesPerFrame(f);
    if(!payload){error="文件没有有效样本";return false;}
    if(payload%frame){error="有效载荷不是完整采样帧，需明确修正尾偏移";return false;}
    count=payload/frame;return true;
}
std::string formatId(const SampleFormat& f) {
    constexpr std::array<const char*,6> ids{"I8","U8","I16","I32","F32","F64"};
    const auto e=static_cast<unsigned>(f.componentEncoding);
    return std::string(f.structure==SampleStructure::Real?"R":"C")+(e<ids.size()?ids[e]:"?");
}
std::string sampleFormatKey(const SampleFormat& f) {
    return formatId(f)+"/"+std::to_string(static_cast<int>(f.byteOrder))+"/"+std::to_string(static_cast<int>(f.iqLayout))+
        "/"+std::to_string(f.channels)+"/"+std::to_string(f.selectedChannel)+"/"+std::to_string(static_cast<int>(f.channelLayout))+
        "/"+std::to_string(f.headerBytes)+"/"+std::to_string(f.trailerBytes)+"/"+std::to_string(f.normalizeIntegerAdc);
}
double adcScale(const SampleFormat& f) {
    if(!f.normalizeIntegerAdc || f.componentEncoding>=ComponentEncoding::Float32)return 1;
    if(bytesPerComponent(f)==1)return 128;
    return f.componentEncoding==ComponentEncoding::Int16?32768.0:2147483648.0;
}
}
