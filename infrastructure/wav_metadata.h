#pragma once

#include <QString>

#include "domain/sample_format.h"

namespace signalstudio {
// Restricted generator profile: PCM16 little endian, two stored components I/Q,
// represented as one complex channel. No RF center frequency exists in this
// API.
struct WavDescriptor {
  SampleFormat format;
  std::uint32_t sampleRateHz = 0;
  std::uint64_t sampleCount = 0, dataOffset = 0, dataBytes = 0, fileBytes = 0;
  std::uint64_t metadataBytesRead = 0;
};

// Reads only RIFF/chunk/fmt headers and seeks past payloads, including data.
// The descriptor is published only after the entire RIFF layout is validated.
bool readWavDescriptor(const QString& path, WavDescriptor& target,
                       QString& error);
}  // namespace signalstudio
