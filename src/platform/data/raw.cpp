#include "signal_studio/data/io.hpp"
#include "signal_studio/data/signal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

namespace signal::data {
namespace {

core::Status data_error(core::ErrorReason reason, std::string message, std::string detail = {}) {
  return core::Status::failure({core::ErrorDomain::data, reason}, std::move(message), std::move(detail));
}

std::uint16_t little_u16(std::span<const std::byte> bytes) {
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[0])) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[1])) << 8U);
}

std::uint32_t little_u32(std::span<const std::byte> bytes) {
  std::uint32_t value{};
  for (std::size_t index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index])) << (index * 8U);
  }
  return value;
}

std::uint64_t unsigned_scalar(std::span<const std::byte> bytes, Endianness endianness) {
  std::uint64_t result{};
  if (endianness == Endianness::big) {
    for (const auto byte : bytes)
      result = (result << 8U) | std::to_integer<std::uint8_t>(byte);
  } else {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      result |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index])) << (index * 8U);
    }
  }
  return result;
}

std::int64_t signed_scalar(std::span<const std::byte> bytes, Endianness endianness) {
  const auto value = unsigned_scalar(bytes, endianness);
  const auto bits = static_cast<unsigned>(bytes.size() * 8U);
  if (bits == 64U)
    return std::bit_cast<std::int64_t>(value);
  const auto sign = std::uint64_t{1} << (bits - 1U);
  if ((value & sign) == 0U)
    return static_cast<std::int64_t>(value);
  const auto mask = ~((std::uint64_t{1} << bits) - 1U);
  return static_cast<std::int64_t>(value | mask);
}

core::Result<double> decode_scalar(std::span<const std::byte> bytes, const SignalDescriptor& descriptor) {
  switch (descriptor.scalar_type) {
  case ScalarType::int8:
    return static_cast<double>(static_cast<std::int8_t>(std::to_integer<std::uint8_t>(bytes[0])));
  case ScalarType::uint8:
    return static_cast<double>(std::to_integer<std::uint8_t>(bytes[0]));
  case ScalarType::int16:
  case ScalarType::int24_packed:
  case ScalarType::int32:
    return static_cast<double>(signed_scalar(bytes, descriptor.endianness));
  case ScalarType::uint16:
    return static_cast<double>(unsigned_scalar(bytes, descriptor.endianness));
  case ScalarType::float32: {
    std::array<std::byte, sizeof(float)> ordered{};
    std::copy(bytes.begin(), bytes.end(), ordered.begin());
    if ((descriptor.endianness == Endianness::little) != (std::endian::native == std::endian::little)) {
      std::reverse(ordered.begin(), ordered.end());
    }
    float value{};
    std::memcpy(&value, ordered.data(), sizeof(value));
    return static_cast<double>(value);
  }
  case ScalarType::float64: {
    std::array<std::byte, sizeof(double)> ordered{};
    std::copy(bytes.begin(), bytes.end(), ordered.begin());
    if ((descriptor.endianness == Endianness::little) != (std::endian::native == std::endian::little)) {
      std::reverse(ordered.begin(), ordered.end());
    }
    double value{};
    std::memcpy(&value, ordered.data(), sizeof(value));
    return value;
  }
  }
  return data_error(core::ErrorReason::invalid_argument, "Unknown scalar type");
}

double apply_amplitude(double raw, const SignalDescriptor& descriptor) {
  return raw * descriptor.scale_factor + descriptor.additive_offset;
}

bool fourcc(std::span<const std::byte> bytes, std::string_view text) {
  if (bytes.size() < text.size())
    return false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (std::to_integer<unsigned char>(bytes[index]) != static_cast<unsigned char>(text[index]))
      return false;
  }
  return true;
}

void append_unsigned(std::vector<std::byte>& output, std::uint64_t value, std::size_t width, Endianness endianness) {
  for (std::size_t index = 0; index < width; ++index) {
    const auto shift_index = endianness == Endianness::big ? width - index - 1U : index;
    output.push_back(static_cast<std::byte>((value >> (shift_index * 8U)) & 0xffU));
  }
}

core::Status encode_scalar(std::vector<std::byte>& output, double value, const SignalDescriptor& descriptor) {
  const double raw = (value - descriptor.additive_offset) / descriptor.scale_factor;
  const auto append_signed = [&](std::int64_t minimum, std::int64_t maximum, std::size_t width) {
    if (!std::isfinite(raw) || raw < static_cast<double>(minimum) || raw > static_cast<double>(maximum)) {
      return data_error(core::ErrorReason::invalid_argument, "Selected integer sample cannot be represented");
    }
    const auto rounded = static_cast<std::int64_t>(std::llround(raw));
    append_unsigned(output, static_cast<std::uint64_t>(rounded), width, descriptor.endianness);
    return core::Status::success();
  };
  switch (descriptor.scalar_type) {
  case ScalarType::int8:
    return append_signed(-128, 127, 1U);
  case ScalarType::uint8:
    if (!std::isfinite(raw) || raw < 0.0 || raw > 255.0) {
      return data_error(core::ErrorReason::invalid_argument, "Selected uint8 sample cannot be represented");
    }
    append_unsigned(output, static_cast<std::uint64_t>(std::llround(raw)), 1U, Endianness::not_applicable);
    return core::Status::success();
  case ScalarType::int16:
    return append_signed(-32768, 32767, 2U);
  case ScalarType::uint16:
    if (!std::isfinite(raw) || raw < 0.0 || raw > 65535.0) {
      return data_error(core::ErrorReason::invalid_argument, "Selected uint16 sample cannot be represented");
    }
    append_unsigned(output, static_cast<std::uint64_t>(std::llround(raw)), 2U, descriptor.endianness);
    return core::Status::success();
  case ScalarType::int24_packed:
    return append_signed(-8388608, 8388607, 3U);
  case ScalarType::int32:
    return append_signed(std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(), 4U);
  case ScalarType::float32: {
    const auto converted = static_cast<float>(raw);
    append_unsigned(output, std::bit_cast<std::uint32_t>(converted), 4U, descriptor.endianness);
    return core::Status::success();
  }
  case ScalarType::float64:
    append_unsigned(output, std::bit_cast<std::uint64_t>(raw), 8U, descriptor.endianness);
    return core::Status::success();
  }
  return data_error(core::ErrorReason::invalid_argument, "Unknown scalar type");
}

void append_little_u16(std::vector<std::byte>& output, std::uint16_t value) {
  append_unsigned(output, value, 2U, Endianness::little);
}

void append_little_u32(std::vector<std::byte>& output, std::uint32_t value) {
  append_unsigned(output, value, 4U, Endianness::little);
}

void append_text(std::vector<std::byte>& output, std::string_view value) {
  for (const char character : value)
    output.push_back(static_cast<std::byte>(character));
}

core::Status atomic_write(const std::filesystem::path& destination, std::span<const std::byte> content) {
  auto temporary = destination;
  temporary += ".tmp";
  auto backup = destination;
  backup += ".bak";
  std::error_code error;
  if (!destination.parent_path().empty())
    std::filesystem::create_directories(destination.parent_path(), error);
  if (error)
    return data_error(core::ErrorReason::unavailable, "Cannot create export directory", error.message());
  {
    std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
    if (!output)
      return data_error(core::ErrorReason::unavailable, "Cannot create temporary export");
    output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output)
      return data_error(core::ErrorReason::unavailable, "Cannot flush temporary export");
  }
  const bool existed = std::filesystem::exists(destination, error);
  error.clear();
  if (existed) {
    std::filesystem::remove(backup, error);
    error.clear();
    std::filesystem::rename(destination, backup, error);
    if (error)
      return data_error(core::ErrorReason::unavailable, "Cannot preserve previous export", error.message());
  }
  std::filesystem::rename(temporary, destination, error);
  if (error) {
    const auto detail = error.message();
    error.clear();
    if (existed)
      std::filesystem::rename(backup, destination, error);
    return data_error(core::ErrorReason::unavailable, "Cannot commit bounded export", detail);
  }
  if (existed)
    std::filesystem::remove(backup, error);
  return core::Status::success();
}

} // namespace

core::Result<RawReadResult> read_raw_samples(const std::filesystem::path& path, const SignalDescriptor& descriptor,
                                             const SampleRange& range, std::uint64_t maximum_read_bytes) {
  const auto validation = descriptor.validate();
  if (!validation)
    return validation;
  if (!descriptor.requested_sample_range.contains(range)) {
    return data_error(core::ErrorReason::invalid_argument, "Read range exceeds the confirmed descriptor range");
  }
  if (maximum_read_bytes == 0U) {
    return data_error(core::ErrorReason::invalid_argument, "A positive read bound is required");
  }
  BoundedFileReader reader{path, maximum_read_bytes};
  const auto file_size = reader.size();
  if (!file_size)
    return file_size.error();
  const auto facts = calculate_data_facts(file_size.value(), descriptor, maximum_read_bytes);
  if (!facts)
    return facts.error();
  if (range.end() > facts.value().available_frames) {
    return data_error(core::ErrorReason::invalid_argument, "Read range exceeds complete source frames");
  }
  const auto scalar_width_result = descriptor.scalar_bytes();
  if (!scalar_width_result)
    return scalar_width_result.error();
  const auto scalar_width = scalar_width_result.value();
  if (range.size() > std::numeric_limits<std::uint64_t>::max() / facts.value().frame_bytes) {
    return data_error(core::ErrorReason::invalid_argument, "Read byte count overflows uint64");
  }
  const auto total_bytes = range.size() * facts.value().frame_bytes;
  if (total_bytes > maximum_read_bytes) {
    return data_error(core::ErrorReason::invalid_argument, "Read request exceeds its explicit byte bound");
  }
  if (range.size() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return data_error(core::ErrorReason::unavailable, "Read request exceeds addressable memory");
  }

  if (descriptor.signal_kind == SignalKind::real) {
    const auto offset = descriptor.byte_offset + range.begin() * scalar_width;
    auto bytes = reader.read(offset, total_bytes);
    if (!bytes)
      return bytes.error();
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(range.size()));
    for (std::uint64_t sample = 0; sample < range.size(); ++sample) {
      const auto byte_offset = static_cast<std::size_t>(sample * scalar_width);
      auto value = decode_scalar(
          std::span<const std::byte>{bytes.value()}.subspan(byte_offset, static_cast<std::size_t>(scalar_width)),
          descriptor);
      if (!value)
        return value.error();
      values.push_back(apply_amplitude(value.value(), descriptor));
    }
    return RawReadResult{SignalBuffer::from_real(std::move(values)), range, total_bytes};
  }

  std::vector<ComplexSample> values;
  values.reserve(static_cast<std::size_t>(range.size()));
  if (descriptor.component_layout == ComponentLayout::interleaved) {
    const auto offset = descriptor.byte_offset + range.begin() * facts.value().frame_bytes;
    auto bytes = reader.read(offset, total_bytes);
    if (!bytes)
      return bytes.error();
    for (std::uint64_t sample = 0; sample < range.size(); ++sample) {
      const auto frame_offset = static_cast<std::size_t>(sample * facts.value().frame_bytes);
      const auto span = std::span<const std::byte>{bytes.value()};
      auto first = decode_scalar(span.subspan(frame_offset, static_cast<std::size_t>(scalar_width)), descriptor);
      auto second = decode_scalar(
          span.subspan(frame_offset + static_cast<std::size_t>(scalar_width), static_cast<std::size_t>(scalar_width)),
          descriptor);
      if (!first)
        return first.error();
      if (!second)
        return second.error();
      const double first_value = apply_amplitude(first.value(), descriptor);
      const double second_value = apply_amplitude(second.value(), descriptor);
      values.push_back(descriptor.component_order == ComponentOrder::iq ? ComplexSample{first_value, second_value}
                                                                        : ComplexSample{second_value, first_value});
    }
  } else {
    const auto component_bytes = range.size() * scalar_width;
    const auto plane_bytes = facts.value().available_frames * scalar_width;
    const auto first_offset = descriptor.byte_offset + range.begin() * scalar_width;
    const auto second_offset = descriptor.byte_offset + plane_bytes + range.begin() * scalar_width;
    auto first_plane = reader.read(first_offset, component_bytes);
    if (!first_plane)
      return first_plane.error();
    auto second_plane = reader.read(second_offset, component_bytes);
    if (!second_plane)
      return second_plane.error();
    for (std::uint64_t sample = 0; sample < range.size(); ++sample) {
      const auto byte_offset = static_cast<std::size_t>(sample * scalar_width);
      auto first = decode_scalar(
          std::span<const std::byte>{first_plane.value()}.subspan(byte_offset, static_cast<std::size_t>(scalar_width)),
          descriptor);
      auto second = decode_scalar(
          std::span<const std::byte>{second_plane.value()}.subspan(byte_offset, static_cast<std::size_t>(scalar_width)),
          descriptor);
      if (!first)
        return first.error();
      if (!second)
        return second.error();
      const double first_value = apply_amplitude(first.value(), descriptor);
      const double second_value = apply_amplitude(second.value(), descriptor);
      values.push_back(descriptor.component_order == ComponentOrder::iq ? ComplexSample{first_value, second_value}
                                                                        : ComplexSample{second_value, first_value});
    }
  }
  return RawReadResult{SignalBuffer::from_complex(std::move(values)), range, total_bytes};
}

core::Result<WavDescriptor> read_wav_descriptor(const std::filesystem::path& path, bool confirm_stereo_iq,
                                                ComponentOrder confirmed_order) {
  constexpr std::uint64_t header_bound = 64U * 1024U;
  BoundedFileReader reader{path, header_bound};
  const auto file_size = reader.size();
  if (!file_size)
    return file_size.error();
  if (file_size.value() < 12U)
    return data_error(core::ErrorReason::invalid_argument, "WAV header is truncated");
  auto riff = reader.read(0U, 12U);
  if (!riff)
    return riff.error();
  if (!fourcc(riff.value(), "RIFF") || !fourcc(std::span<const std::byte>{riff.value()}.subspan(8U), "WAVE")) {
    return data_error(core::ErrorReason::invalid_argument, "Only little-endian RIFF/WAVE is supported");
  }

  std::uint16_t audio_format{};
  std::uint16_t channels{};
  std::uint32_t sample_rate{};
  std::uint16_t block_align{};
  std::uint16_t bits_per_sample{};
  std::uint64_t data_offset{};
  std::uint64_t data_bytes{};
  bool have_format{};
  bool have_data{};
  std::uint64_t cursor = 12U;
  while (cursor + 8U <= file_size.value()) {
    auto chunk_header = reader.read(cursor, 8U);
    if (!chunk_header)
      return chunk_header.error();
    const auto chunk_size =
        static_cast<std::uint64_t>(little_u32(std::span<const std::byte>{chunk_header.value()}.subspan(4U, 4U)));
    if (chunk_size > file_size.value() - cursor - 8U) {
      return data_error(core::ErrorReason::invalid_argument, "WAV chunk extends beyond the source file");
    }
    if (fourcc(chunk_header.value(), "fmt ")) {
      if (chunk_size < 16U || chunk_size > header_bound) {
        return data_error(core::ErrorReason::invalid_argument, "Unsupported WAV format chunk size");
      }
      auto format = reader.read(cursor + 8U, chunk_size);
      if (!format)
        return format.error();
      const auto span = std::span<const std::byte>{format.value()};
      audio_format = little_u16(span.subspan(0U, 2U));
      channels = little_u16(span.subspan(2U, 2U));
      sample_rate = little_u32(span.subspan(4U, 4U));
      block_align = little_u16(span.subspan(12U, 2U));
      bits_per_sample = little_u16(span.subspan(14U, 2U));
      have_format = true;
    } else if (fourcc(chunk_header.value(), "data")) {
      data_offset = cursor + 8U;
      data_bytes = chunk_size;
      have_data = true;
    }
    if (have_format && have_data)
      break;
    if (chunk_size > std::numeric_limits<std::uint64_t>::max() - cursor - 9U) {
      return data_error(core::ErrorReason::invalid_argument, "WAV chunk cursor overflows uint64");
    }
    cursor += 8U + chunk_size + (chunk_size & 1U);
  }
  if (!have_format || !have_data) {
    return data_error(core::ErrorReason::invalid_argument, "WAV requires both format and data chunks");
  }
  if (channels != 1U && channels != 2U) {
    return data_error(core::ErrorReason::invalid_argument, "Only mono real or stereo IQ WAV is supported");
  }
  if (channels == 2U && (!confirm_stereo_iq || confirmed_order == ComponentOrder::not_applicable)) {
    return data_error(core::ErrorReason::invalid_argument, "Stereo WAV requires explicit IQ/QI confirmation");
  }
  if (sample_rate == 0U || block_align == 0U || data_bytes % block_align != 0U) {
    return data_error(core::ErrorReason::invalid_argument, "WAV sample rate or frame alignment is invalid");
  }

  ScalarType scalar_type{};
  double scale = 1.0;
  double additive = 0.0;
  if (audio_format == 1U) {
    switch (bits_per_sample) {
    case 8U:
      scalar_type = ScalarType::uint8;
      scale = 1.0 / 128.0;
      additive = -1.0;
      break;
    case 16U:
      scalar_type = ScalarType::int16;
      scale = 1.0 / 32768.0;
      break;
    case 24U:
      scalar_type = ScalarType::int24_packed;
      scale = 1.0 / 8388608.0;
      break;
    case 32U:
      scalar_type = ScalarType::int32;
      scale = 1.0 / 2147483648.0;
      break;
    default:
      return data_error(core::ErrorReason::invalid_argument, "Unsupported PCM WAV bit depth");
    }
  } else if (audio_format == 3U) {
    if (bits_per_sample == 32U)
      scalar_type = ScalarType::float32;
    else if (bits_per_sample == 64U)
      scalar_type = ScalarType::float64;
    else
      return data_error(core::ErrorReason::invalid_argument, "Unsupported IEEE-float WAV bit depth");
  } else {
    return data_error(core::ErrorReason::invalid_argument, "Compressed or extensible WAV is not supported");
  }

  const auto frames = data_bytes / block_align;
  const auto range = SampleRange::from_count(0U, frames);
  if (!range)
    return range.error();
  SignalDescriptor descriptor;
  descriptor.signal_kind = channels == 1U ? SignalKind::real : SignalKind::complex;
  descriptor.scalar_type = scalar_type;
  descriptor.component_layout = channels == 1U ? ComponentLayout::real : ComponentLayout::interleaved;
  descriptor.component_order = channels == 1U ? ComponentOrder::not_applicable : confirmed_order;
  descriptor.endianness = bits_per_sample == 8U ? Endianness::not_applicable : Endianness::little;
  descriptor.sample_rate_hz = static_cast<double>(sample_rate);
  descriptor.byte_offset = data_offset;
  descriptor.requested_sample_range = range.value();
  descriptor.amplitude_mode = "wav_normalized";
  descriptor.scale_factor = scale;
  descriptor.additive_offset = additive;
  descriptor.provenance = {
      {"signalKind", {FieldOrigin::wav_header, true}},
      {"scalarType", {FieldOrigin::wav_header, true}},
      {"sampleRateHz", {FieldOrigin::wav_header, true}},
      {"componentOrder", {channels == 1U ? FieldOrigin::wav_header : FieldOrigin::user, true}},
  };
  const auto validation = descriptor.validate();
  if (!validation)
    return validation;
  return WavDescriptor{std::move(descriptor), data_bytes};
}

FileDataSource::FileDataSource(std::filesystem::path path, SignalDescriptor descriptor,
                               std::string data_source_version_id, SourceFormat source_format)
    : path_(std::move(path)), descriptor_(std::move(descriptor)),
      data_source_version_id_(std::move(data_source_version_id)), source_format_(source_format) {}

core::Result<std::shared_ptr<FileDataSource>>
FileDataSource::open_raw(std::filesystem::path path, SignalDescriptor descriptor, std::string data_source_version_id) {
  const auto validation = descriptor.validate();
  if (!validation)
    return validation;
  if (data_source_version_id.empty()) {
    return data_error(core::ErrorReason::invalid_argument, "Data source version id is required");
  }
  BoundedFileReader reader{path, 1U};
  const auto size = reader.size();
  if (!size)
    return size.error();
  const auto facts = calculate_data_facts(size.value(), descriptor, 1U);
  if (!facts)
    return facts.error();
  if (descriptor.requested_sample_range.end() > facts.value().available_frames) {
    return data_error(core::ErrorReason::invalid_argument, "Descriptor range exceeds the RAW source");
  }
  return std::shared_ptr<FileDataSource>{
      new FileDataSource{std::move(path), std::move(descriptor), std::move(data_source_version_id), SourceFormat::raw}};
}

core::Result<std::shared_ptr<FileDataSource>> FileDataSource::open_wav(std::filesystem::path path,
                                                                       std::string data_source_version_id,
                                                                       bool confirm_stereo_iq,
                                                                       ComponentOrder confirmed_order) {
  if (data_source_version_id.empty()) {
    return data_error(core::ErrorReason::invalid_argument, "Data source version id is required");
  }
  auto wav = read_wav_descriptor(path, confirm_stereo_iq, confirmed_order);
  if (!wav)
    return wav.error();
  return std::shared_ptr<FileDataSource>{new FileDataSource{std::move(path), std::move(wav.value().descriptor),
                                                            std::move(data_source_version_id), SourceFormat::wav}};
}

core::Result<RawReadResult> FileDataSource::read(const ReadRequest& request) const {
  if (request.cancellation_requested && request.cancellation_requested()) {
    return data_error(core::ErrorReason::cancelled, "Data source read was cancelled before I/O");
  }
  auto result = read_raw_samples(path_, descriptor_, request.range, request.maximum_read_bytes);
  if (!result)
    return result.error();
  if (request.cancellation_requested && request.cancellation_requested()) {
    return data_error(core::ErrorReason::cancelled, "Data source read was cancelled after I/O");
  }
  return result;
}

core::Result<SignalSelection> select_samples(const RawReadResult& browsed, const SampleRange& selection,
                                             const IDataSource& source) {
  if (!browsed.range.contains(selection)) {
    return data_error(core::ErrorReason::invalid_argument, "Selection exceeds the browsed data range");
  }
  const auto relative_offset = selection.begin() - browsed.range.begin();
  auto view = browsed.samples.view().slice(relative_offset, selection.size());
  if (!view)
    return view.error();
  return SignalSelection{selection, view.value(), source.descriptor(), source.source_format(),
                         std::string{source.data_source_version_id()}};
}

core::Result<ExportReceipt> export_selection(const SignalSelection& selection, const std::filesystem::path& destination,
                                             std::uint64_t maximum_export_bytes) {
  const auto validation = selection.source_descriptor.validate();
  if (!validation)
    return validation;
  if (selection.data_source_version_id.empty() || selection.samples.size() != selection.source_range.size() ||
      selection.samples.kind() != selection.source_descriptor.signal_kind || maximum_export_bytes == 0U) {
    return data_error(core::ErrorReason::invalid_argument, "Bounded export selection is inconsistent");
  }
  const auto frame = selection.source_descriptor.frame_bytes();
  if (!frame)
    return frame.error();
  if (selection.samples.size() > std::numeric_limits<std::uint64_t>::max() / frame.value()) {
    return data_error(core::ErrorReason::invalid_argument, "Bounded export size overflows uint64");
  }
  const auto sample_bytes = selection.samples.size() * frame.value();
  const std::uint64_t header_bytes = selection.source_format == SourceFormat::wav ? 44U : 0U;
  if (sample_bytes > maximum_export_bytes || header_bytes > maximum_export_bytes - sample_bytes ||
      sample_bytes + header_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return data_error(core::ErrorReason::invalid_argument, "Bounded export exceeds its explicit byte limit");
  }

  std::vector<std::byte> encoded;
  encoded.reserve(static_cast<std::size_t>(sample_bytes + header_bytes));
  SignalDescriptor output_descriptor = selection.source_descriptor;
  output_descriptor.byte_offset = header_bytes;
  const auto exported_range = SampleRange::from_count(0U, selection.samples.size());
  if (!exported_range)
    return exported_range.error();
  output_descriptor.requested_sample_range = exported_range.value();

  if (selection.source_format == SourceFormat::wav) {
    if (selection.source_descriptor.component_layout == ComponentLayout::planar || sample_bytes > 0xffffffffU) {
      return data_error(core::ErrorReason::invalid_argument,
                        "WAV export requires interleaved data below RIFF size limits");
    }
    const auto scalar = selection.source_descriptor.scalar_bytes();
    if (!scalar)
      return scalar.error();
    const auto channels = selection.source_descriptor.signal_kind == SignalKind::complex ? 2U : 1U;
    const auto format = selection.source_descriptor.scalar_type == ScalarType::float32 ||
                                selection.source_descriptor.scalar_type == ScalarType::float64
                            ? 3U
                            : 1U;
    append_text(encoded, "RIFF");
    append_little_u32(encoded, static_cast<std::uint32_t>(36U + sample_bytes));
    append_text(encoded, "WAVEfmt ");
    append_little_u32(encoded, 16U);
    append_little_u16(encoded, static_cast<std::uint16_t>(format));
    append_little_u16(encoded, static_cast<std::uint16_t>(channels));
    append_little_u32(encoded, static_cast<std::uint32_t>(selection.source_descriptor.sample_rate_hz));
    append_little_u32(
        encoded, static_cast<std::uint32_t>(selection.source_descriptor.sample_rate_hz * channels * scalar.value()));
    append_little_u16(encoded, static_cast<std::uint16_t>(channels * scalar.value()));
    append_little_u16(encoded, static_cast<std::uint16_t>(scalar.value() * 8U));
    append_text(encoded, "data");
    append_little_u32(encoded, static_cast<std::uint32_t>(sample_bytes));
  }

  const auto append_component = [&](double value) {
    return encode_scalar(encoded, value, selection.source_descriptor);
  };
  if (selection.samples.kind() == SignalKind::real) {
    for (const double value : selection.samples.real_values()) {
      const auto status = append_component(value);
      if (!status)
        return status;
    }
  } else if (selection.source_descriptor.component_layout == ComponentLayout::interleaved) {
    for (const auto value : selection.samples.complex_values()) {
      auto status =
          append_component(selection.source_descriptor.component_order == ComponentOrder::iq ? value.real : value.imag);
      if (!status)
        return status;
      status =
          append_component(selection.source_descriptor.component_order == ComponentOrder::iq ? value.imag : value.real);
      if (!status)
        return status;
    }
  } else {
    for (const auto value : selection.samples.complex_values()) {
      const auto status =
          append_component(selection.source_descriptor.component_order == ComponentOrder::iq ? value.real : value.imag);
      if (!status)
        return status;
    }
    for (const auto value : selection.samples.complex_values()) {
      const auto status =
          append_component(selection.source_descriptor.component_order == ComponentOrder::iq ? value.imag : value.real);
      if (!status)
        return status;
    }
  }
  if (encoded.size() != sample_bytes + header_bytes) {
    return data_error(core::ErrorReason::internal_failure, "Bounded export encoded an unexpected byte count");
  }
  const auto write = atomic_write(destination, encoded);
  if (!write)
    return write;
  return ExportReceipt{destination, selection.source_range, std::move(output_descriptor), selection.source_format,
                       static_cast<std::uint64_t>(encoded.size())};
}

} // namespace signal::data
