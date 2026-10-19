#pragma once
#include <cstdint>
#include <string>

namespace signalstudio {
enum class SampleStructure { ComplexIQ, Real };
enum class ComponentEncoding { Int8, UInt8, Int16, Int32, Float32, Float64 };
enum class ByteOrder { NotApplicable, Little, Big };
enum class IQLayout { NotApplicable, IQInterleaved, QIInterleaved, PlanarIQ };
enum class ChannelLayout { TimeInterleaved, ChannelPlanar };
struct SampleFormat {
    SampleStructure structure = SampleStructure::ComplexIQ;
    ComponentEncoding componentEncoding = ComponentEncoding::Int16;
    ByteOrder byteOrder = ByteOrder::Little;
    IQLayout iqLayout = IQLayout::IQInterleaved;
    std::uint32_t channels = 1, selectedChannel = 0;
    ChannelLayout channelLayout = ChannelLayout::TimeInterleaved;
    std::uint64_t headerBytes = 0, trailerBytes = 0;
    bool normalizeIntegerAdc = true;
    bool operator==(const SampleFormat&) const = default;
};
std::uint64_t bytesPerComponent(const SampleFormat&);
std::uint64_t bytesPerChannelSample(const SampleFormat&);
std::uint64_t bytesPerFrame(const SampleFormat&);
bool validateSampleFormat(const SampleFormat&, std::string& error);
bool sampleCountForBytes(const SampleFormat&, std::uint64_t size, std::uint64_t& count, std::string& error);
std::string formatId(const SampleFormat&);
std::string sampleFormatKey(const SampleFormat&);
double adcScale(const SampleFormat&);
} // namespace signalstudio
