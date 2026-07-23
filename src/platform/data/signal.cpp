#include "signal_studio/data/signal.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace signal::data {
namespace {

core::Status data_error(core::ErrorReason reason, std::string message, std::string detail = {}) {
  return core::Status::failure({core::ErrorDomain::data, reason}, std::move(message), std::move(detail));
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return value;
}

std::string json_escape(std::string_view value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string output;
  output.reserve(value.size());
  for (const char character : value) {
    switch (character) {
    case '\"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(character) < 0x20U) {
        output += "\\u00";
        output += digits[(static_cast<unsigned char>(character) >> 4U) & 0x0fU];
        output += digits[static_cast<unsigned char>(character) & 0x0fU];
      } else {
        output += character;
      }
      break;
    }
  }
  return output;
}

struct JsonValue final {
  enum class Kind : std::uint8_t { null_value, boolean, number, string, object, array };
  Kind kind{Kind::null_value};
  bool boolean{};
  std::string text;
  std::map<std::string, JsonValue, std::less<>> object;
  std::vector<JsonValue> array;
};

class JsonParser final {
public:
  explicit JsonParser(std::string_view input) : input_(input) {}

  bool parse(JsonValue& output) {
    skip_whitespace();
    if (!parse_value(output, 0U))
      return false;
    skip_whitespace();
    if (cursor_ != input_.size())
      return fail("Trailing content follows the JSON value");
    return true;
  }

  [[nodiscard]] const std::string& error() const noexcept {
    return error_;
  }

private:
  bool fail(std::string message) {
    if (error_.empty())
      error_ = std::move(message) + " at byte " + std::to_string(cursor_);
    return false;
  }

  void skip_whitespace() {
    while (cursor_ < input_.size() &&
           (input_[cursor_] == ' ' || input_[cursor_] == '\t' || input_[cursor_] == '\r' || input_[cursor_] == '\n'))
      ++cursor_;
  }

  bool parse_value(JsonValue& output, std::size_t depth) {
    if (depth > 64U)
      return fail("JSON nesting exceeds the supported limit");
    skip_whitespace();
    if (cursor_ >= input_.size())
      return fail("JSON value is missing");
    switch (input_[cursor_]) {
    case '{':
      return parse_object(output, depth + 1U);
    case '[':
      return parse_array(output, depth + 1U);
    case '"':
      output.kind = JsonValue::Kind::string;
      return parse_string(output.text);
    case 't':
      return parse_literal("true", JsonValue::Kind::boolean, output, true);
    case 'f':
      return parse_literal("false", JsonValue::Kind::boolean, output, false);
    case 'n':
      return parse_literal("null", JsonValue::Kind::null_value, output, false);
    default:
      if (input_[cursor_] == '-' || (input_[cursor_] >= '0' && input_[cursor_] <= '9')) {
        output.kind = JsonValue::Kind::number;
        return parse_number(output.text);
      }
      return fail("Unexpected JSON token");
    }
  }

  bool parse_literal(std::string_view literal, JsonValue::Kind kind, JsonValue& output, bool value) {
    if (input_.substr(cursor_, literal.size()) != literal)
      return fail("Invalid JSON literal");
    cursor_ += literal.size();
    output.kind = kind;
    output.boolean = value;
    return true;
  }

  static void append_utf8(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7fU)
      output.push_back(static_cast<char>(code_point));
    else if (code_point <= 0x7ffU) {
      output.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
      output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else if (code_point <= 0xffffU) {
      output.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
      output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else {
      output.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
      output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    }
  }

  bool parse_hex4(std::uint32_t& value) {
    if (cursor_ > input_.size() || 4U > input_.size() - cursor_)
      return fail("Truncated Unicode escape");
    value = 0U;
    for (std::size_t index = 0; index < 4U; ++index) {
      const char character = input_[cursor_++];
      std::uint32_t digit{};
      if (character >= '0' && character <= '9')
        digit = static_cast<std::uint32_t>(character - '0');
      else if (character >= 'a' && character <= 'f')
        digit = static_cast<std::uint32_t>(character - 'a' + 10);
      else if (character >= 'A' && character <= 'F')
        digit = static_cast<std::uint32_t>(character - 'A' + 10);
      else
        return fail("Invalid Unicode escape");
      value = value * 16U + digit;
    }
    return true;
  }

  bool parse_string(std::string& output) {
    if (input_[cursor_++] != '"')
      return fail("JSON string is missing an opening quote");
    output.clear();
    while (cursor_ < input_.size()) {
      const unsigned char character = static_cast<unsigned char>(input_[cursor_++]);
      if (character == '"')
        return true;
      if (character < 0x20U)
        return fail("JSON string contains an unescaped control character");
      if (character != '\\') {
        output.push_back(static_cast<char>(character));
        continue;
      }
      if (cursor_ >= input_.size())
        return fail("JSON escape is truncated");
      const char escape = input_[cursor_++];
      switch (escape) {
      case '"':
        output.push_back('"');
        break;
      case '\\':
        output.push_back('\\');
        break;
      case '/':
        output.push_back('/');
        break;
      case 'b':
        output.push_back('\b');
        break;
      case 'f':
        output.push_back('\f');
        break;
      case 'n':
        output.push_back('\n');
        break;
      case 'r':
        output.push_back('\r');
        break;
      case 't':
        output.push_back('\t');
        break;
      case 'u': {
        std::uint32_t code_point{};
        if (!parse_hex4(code_point))
          return false;
        if (code_point >= 0xd800U && code_point <= 0xdbffU) {
          if (cursor_ + 2U > input_.size() || input_[cursor_] != '\\' || input_[cursor_ + 1U] != 'u') {
            return fail("High surrogate is not followed by a low surrogate");
          }
          cursor_ += 2U;
          std::uint32_t low{};
          if (!parse_hex4(low))
            return false;
          if (low < 0xdc00U || low > 0xdfffU)
            return fail("Unicode low surrogate is invalid");
          code_point = 0x10000U + ((code_point - 0xd800U) << 10U) + (low - 0xdc00U);
        } else if (code_point >= 0xdc00U && code_point <= 0xdfffU) {
          return fail("Unpaired Unicode low surrogate");
        }
        append_utf8(output, code_point);
        break;
      }
      default:
        return fail("Unknown JSON escape");
      }
    }
    return fail("JSON string is missing a closing quote");
  }

  bool parse_number(std::string& output) {
    const auto begin = cursor_;
    if (input_[cursor_] == '-')
      ++cursor_;
    if (cursor_ >= input_.size())
      return fail("JSON number is truncated");
    if (input_[cursor_] == '0')
      ++cursor_;
    else if (input_[cursor_] >= '1' && input_[cursor_] <= '9') {
      while (cursor_ < input_.size() && input_[cursor_] >= '0' && input_[cursor_] <= '9')
        ++cursor_;
    } else
      return fail("JSON number integer part is invalid");
    if (cursor_ < input_.size() && input_[cursor_] == '.') {
      ++cursor_;
      const auto fraction = cursor_;
      while (cursor_ < input_.size() && input_[cursor_] >= '0' && input_[cursor_] <= '9')
        ++cursor_;
      if (fraction == cursor_)
        return fail("JSON number fraction is empty");
    }
    if (cursor_ < input_.size() && (input_[cursor_] == 'e' || input_[cursor_] == 'E')) {
      ++cursor_;
      if (cursor_ < input_.size() && (input_[cursor_] == '+' || input_[cursor_] == '-'))
        ++cursor_;
      const auto exponent = cursor_;
      while (cursor_ < input_.size() && input_[cursor_] >= '0' && input_[cursor_] <= '9')
        ++cursor_;
      if (exponent == cursor_)
        return fail("JSON number exponent is empty");
    }
    output = std::string{input_.substr(begin, cursor_ - begin)};
    return true;
  }

  bool parse_object(JsonValue& output, std::size_t depth) {
    ++cursor_;
    output.kind = JsonValue::Kind::object;
    output.object.clear();
    skip_whitespace();
    if (cursor_ < input_.size() && input_[cursor_] == '}') {
      ++cursor_;
      return true;
    }
    while (cursor_ < input_.size()) {
      if (input_[cursor_] != '"')
        return fail("JSON object key must be a string");
      std::string key;
      if (!parse_string(key))
        return false;
      skip_whitespace();
      if (cursor_ >= input_.size() || input_[cursor_++] != ':')
        return fail("JSON object key lacks a colon");
      JsonValue value;
      if (!parse_value(value, depth))
        return false;
      if (!output.object.emplace(std::move(key), std::move(value)).second) {
        return fail("JSON object contains a duplicate key");
      }
      skip_whitespace();
      if (cursor_ >= input_.size())
        return fail("JSON object is truncated");
      const char separator = input_[cursor_++];
      if (separator == '}')
        return true;
      if (separator != ',')
        return fail("JSON object member separator is invalid");
      skip_whitespace();
    }
    return fail("JSON object is truncated");
  }

  bool parse_array(JsonValue& output, std::size_t depth) {
    ++cursor_;
    output.kind = JsonValue::Kind::array;
    output.array.clear();
    skip_whitespace();
    if (cursor_ < input_.size() && input_[cursor_] == ']') {
      ++cursor_;
      return true;
    }
    while (cursor_ < input_.size()) {
      JsonValue value;
      if (!parse_value(value, depth))
        return false;
      output.array.push_back(std::move(value));
      skip_whitespace();
      if (cursor_ >= input_.size())
        return fail("JSON array is truncated");
      const char separator = input_[cursor_++];
      if (separator == ']')
        return true;
      if (separator != ',')
        return fail("JSON array element separator is invalid");
      skip_whitespace();
    }
    return fail("JSON array is truncated");
  }

  std::string_view input_;
  std::size_t cursor_{};
  std::string error_;
};

const JsonValue* member(const JsonValue& object, std::string_view key, JsonValue::Kind kind) {
  if (object.kind != JsonValue::Kind::object)
    return nullptr;
  const auto iterator = object.object.find(key);
  return iterator != object.object.end() && iterator->second.kind == kind ? &iterator->second : nullptr;
}

std::optional<double> json_double(const JsonValue& value) {
  if (value.kind != JsonValue::Kind::number)
    return std::nullopt;
  char* end{};
  const double parsed = std::strtod(value.text.c_str(), &end);
  return end == value.text.c_str() + value.text.size() && std::isfinite(parsed) ? std::optional<double>{parsed}
                                                                                : std::nullopt;
}

std::optional<std::uint64_t> json_uint64(const JsonValue& value) {
  if (value.kind != JsonValue::Kind::number || value.text.empty() || value.text.front() == '-' ||
      value.text.find_first_of(".eE") != std::string::npos)
    return std::nullopt;
  std::uint64_t parsed{};
  const auto result = std::from_chars(value.text.data(), value.text.data() + value.text.size(), parsed);
  return result.ec == std::errc{} && result.ptr == value.text.data() + value.text.size()
             ? std::optional<std::uint64_t>{parsed}
             : std::nullopt;
}

std::string_view signal_kind_name(SignalKind value) {
  return value == SignalKind::real ? "real" : "complex";
}

std::string_view scalar_type_name(ScalarType value) {
  switch (value) {
  case ScalarType::int8:
    return "int8";
  case ScalarType::uint8:
    return "uint8";
  case ScalarType::int16:
    return "int16";
  case ScalarType::uint16:
    return "uint16";
  case ScalarType::int24_packed:
    return "int24_packed";
  case ScalarType::int32:
    return "int32";
  case ScalarType::float32:
    return "float32";
  case ScalarType::float64:
    return "float64";
  }
  return "unknown";
}

std::string_view layout_name(ComponentLayout value) {
  switch (value) {
  case ComponentLayout::real:
    return "real";
  case ComponentLayout::interleaved:
    return "interleaved";
  case ComponentLayout::planar:
    return "planar";
  }
  return "unknown";
}

std::string_view order_name(ComponentOrder value) {
  switch (value) {
  case ComponentOrder::not_applicable:
    return "not_applicable";
  case ComponentOrder::iq:
    return "IQ";
  case ComponentOrder::qi:
    return "QI";
  }
  return "unknown";
}

std::string_view endian_name(Endianness value) {
  switch (value) {
  case Endianness::not_applicable:
    return "not_applicable";
  case Endianness::little:
    return "little";
  case Endianness::big:
    return "big";
  }
  return "unknown";
}

std::string_view origin_name(FieldOrigin value) {
  switch (value) {
  case FieldOrigin::user:
    return "user";
  case FieldOrigin::sidecar:
    return "sidecar";
  case FieldOrigin::wav_header:
    return "wav_header";
  case FieldOrigin::adapter:
    return "adapter";
  case FieldOrigin::filename_hint:
    return "filename_hint";
  }
  return "unknown";
}

std::optional<ScalarType> parse_scalar(std::string_view value) {
  if (value == "int8")
    return ScalarType::int8;
  if (value == "uint8")
    return ScalarType::uint8;
  if (value == "int16")
    return ScalarType::int16;
  if (value == "uint16")
    return ScalarType::uint16;
  if (value == "int24_packed")
    return ScalarType::int24_packed;
  if (value == "int32")
    return ScalarType::int32;
  if (value == "float32")
    return ScalarType::float32;
  if (value == "float64")
    return ScalarType::float64;
  return std::nullopt;
}

std::optional<FieldOrigin> parse_origin(std::string_view value) {
  if (value == "user")
    return FieldOrigin::user;
  if (value == "sidecar")
    return FieldOrigin::sidecar;
  if (value == "wav_header")
    return FieldOrigin::wav_header;
  if (value == "adapter")
    return FieldOrigin::adapter;
  if (value == "filename_hint")
    return FieldOrigin::filename_hint;
  return std::nullopt;
}

} // namespace

core::Result<SampleRange> SampleRange::make(std::uint64_t begin, std::uint64_t end) {
  if (end < begin) {
    return data_error(core::ErrorReason::invalid_argument, "Sample range end precedes begin");
  }
  return SampleRange{begin, end};
}

core::Result<SampleRange> SampleRange::from_count(std::uint64_t begin, std::uint64_t count) {
  if (count > std::numeric_limits<std::uint64_t>::max() - begin) {
    return data_error(core::ErrorReason::invalid_argument, "Sample range addition overflows uint64");
  }
  return SampleRange{begin, begin + count};
}

bool SampleRange::contains(std::uint64_t sample) const noexcept {
  return sample >= begin_ && sample < end_;
}
bool SampleRange::contains(const SampleRange& other) const noexcept {
  return other.begin_ >= begin_ && other.end_ <= end_;
}

core::Result<std::uint64_t> SignalDescriptor::scalar_bytes() const {
  switch (scalar_type) {
  case ScalarType::int8:
  case ScalarType::uint8:
    return std::uint64_t{1};
  case ScalarType::int16:
  case ScalarType::uint16:
    return std::uint64_t{2};
  case ScalarType::int24_packed:
    return std::uint64_t{3};
  case ScalarType::int32:
  case ScalarType::float32:
    return std::uint64_t{4};
  case ScalarType::float64:
    return std::uint64_t{8};
  }
  return data_error(core::ErrorReason::invalid_argument, "Unknown scalar type");
}

core::Result<std::uint64_t> SignalDescriptor::frame_bytes() const {
  auto bytes = scalar_bytes();
  if (!bytes)
    return bytes.error();
  return bytes.value() * (signal_kind == SignalKind::complex ? 2U : 1U);
}

core::Status SignalDescriptor::validate() const {
  if (schema != "signal.raw-descriptor/1.0") {
    return data_error(core::ErrorReason::invalid_argument, "Unsupported descriptor schema", schema);
  }
  if (!std::isfinite(sample_rate_hz) || sample_rate_hz <= 0.0) {
    return data_error(core::ErrorReason::invalid_argument, "Sample rate must be finite and positive");
  }
  if (!std::isfinite(scale_factor) || scale_factor == 0.0 || !std::isfinite(additive_offset) ||
      !std::isfinite(start_time_seconds)) {
    return data_error(core::ErrorReason::invalid_argument, "Amplitude and time fields must be finite");
  }
  if (center_frequency_hz && !std::isfinite(*center_frequency_hz)) {
    return data_error(core::ErrorReason::invalid_argument, "Center frequency must be finite when present");
  }
  if (amplitude_mode.empty()) {
    return data_error(core::ErrorReason::invalid_argument, "Amplitude mode is required");
  }
  const auto scalar_width = scalar_bytes();
  if (!scalar_width)
    return scalar_width.error();
  if (scalar_width.value() == 1U && endianness != Endianness::not_applicable) {
    return data_error(core::ErrorReason::invalid_argument, "Endianness is not applicable to one-byte scalar types");
  }
  if (scalar_width.value() > 1U && endianness == Endianness::not_applicable) {
    return data_error(core::ErrorReason::invalid_argument, "Endianness is required for multi-byte scalar types");
  }
  if (signal_kind == SignalKind::real) {
    if (component_layout != ComponentLayout::real || component_order != ComponentOrder::not_applicable) {
      return data_error(core::ErrorReason::invalid_argument, "Real signals require real layout and no component order");
    }
  } else if (component_layout == ComponentLayout::real || component_order == ComponentOrder::not_applicable) {
    return data_error(core::ErrorReason::invalid_argument,
                      "Complex signals require interleaved or planar IQ/QI components");
  }
  return core::Status::success();
}

core::Result<DataFacts> calculate_data_facts(std::uint64_t file_bytes, const SignalDescriptor& descriptor,
                                             std::uint64_t configured_initial_bytes) {
  const auto validation = descriptor.validate();
  if (!validation)
    return validation;
  if (descriptor.byte_offset > file_bytes) {
    return data_error(core::ErrorReason::invalid_argument, "Byte offset exceeds the source file");
  }
  const auto frame = descriptor.frame_bytes();
  if (!frame)
    return frame.error();
  const auto remaining = file_bytes - descriptor.byte_offset;
  if (remaining % frame.value() != 0U) {
    return data_error(core::ErrorReason::invalid_argument, "Source tail is not aligned to a complete sample frame");
  }
  DataFacts facts;
  facts.file_bytes = file_bytes;
  facts.remaining_bytes = remaining;
  facts.frame_bytes = frame.value();
  facts.available_frames = remaining / frame.value();
  facts.duration_seconds = static_cast<double>(facts.available_frames) / descriptor.sample_rate_hz;
  const auto aligned_remaining = facts.available_frames * frame.value();
  const auto desired = std::min(configured_initial_bytes, aligned_remaining);
  facts.initial_target_bytes = desired - (desired % frame.value());
  return facts;
}

core::Result<double> display_frequency(double baseband_frequency_hz, const SignalDescriptor& descriptor,
                                       FrequencyReference reference) {
  if (!std::isfinite(baseband_frequency_hz)) {
    return data_error(core::ErrorReason::invalid_argument, "Baseband frequency must be finite");
  }
  if (reference == FrequencyReference::baseband)
    return baseband_frequency_hz;
  if (!descriptor.center_frequency_hz) {
    return data_error(core::ErrorReason::invalid_argument, "Absolute frequency requires a confirmed center frequency");
  }
  return baseband_frequency_hz + *descriptor.center_frequency_hz;
}

std::span<const double> SignalSlice::real_values() const noexcept {
  if (kind_ != SignalKind::real || !real_owner_)
    return {};
  return {real_owner_->data() + static_cast<std::size_t>(offset_), static_cast<std::size_t>(size_)};
}

std::span<const ComplexSample> SignalSlice::complex_values() const noexcept {
  if (kind_ != SignalKind::complex || !complex_owner_)
    return {};
  return {complex_owner_->data() + static_cast<std::size_t>(offset_), static_cast<std::size_t>(size_)};
}

core::Result<SignalSlice> SignalSlice::slice(std::uint64_t offset, std::uint64_t count) const {
  if (offset > size_ || count > size_ - offset) {
    return data_error(core::ErrorReason::invalid_argument, "Zero-copy slice exceeds its owner");
  }
  SignalSlice result = *this;
  result.offset_ += offset;
  result.size_ = count;
  return result;
}

SignalBuffer SignalBuffer::from_real(std::vector<double> values) {
  SignalBuffer result;
  result.kind_ = SignalKind::real;
  result.real_ = std::make_shared<const std::vector<double>>(std::move(values));
  return result;
}

SignalBuffer SignalBuffer::from_complex(std::vector<ComplexSample> values) {
  SignalBuffer result;
  result.kind_ = SignalKind::complex;
  result.complex_ = std::make_shared<const std::vector<ComplexSample>>(std::move(values));
  return result;
}

std::uint64_t SignalBuffer::size() const noexcept {
  if (kind_ == SignalKind::real)
    return real_ ? static_cast<std::uint64_t>(real_->size()) : 0U;
  return complex_ ? static_cast<std::uint64_t>(complex_->size()) : 0U;
}

SignalSlice SignalBuffer::view() const noexcept {
  SignalSlice result;
  result.kind_ = kind_;
  result.real_owner_ = real_;
  result.complex_owner_ = complex_;
  result.size_ = size();
  return result;
}

core::Result<std::string> serialize_sidecar(const SignalDescriptor& descriptor) {
  const auto validation = descriptor.validate();
  if (!validation)
    return validation;
  std::ostringstream output;
  output.precision(17);
  output << "{\"schema\":\"" << descriptor.schema << "\",\"signalKind\":\"" << signal_kind_name(descriptor.signal_kind)
         << "\",\"scalarType\":\"" << scalar_type_name(descriptor.scalar_type) << "\",\"componentLayout\":\""
         << layout_name(descriptor.component_layout) << "\",\"componentOrder\":\""
         << order_name(descriptor.component_order) << "\",\"endianness\":\"" << endian_name(descriptor.endianness)
         << "\",\"sampleRateHz\":" << descriptor.sample_rate_hz << ",\"centerFrequencyHz\":";
  if (descriptor.center_frequency_hz)
    output << *descriptor.center_frequency_hz;
  else
    output << "null";
  output << ",\"byteOffset\":" << descriptor.byte_offset
         << ",\"requestedStart\":" << descriptor.requested_sample_range.begin()
         << ",\"requestedEnd\":" << descriptor.requested_sample_range.end() << ",\"amplitudeMode\":\""
         << json_escape(descriptor.amplitude_mode) << "\",\"scaleFactor\":" << descriptor.scale_factor
         << ",\"additiveOffset\":" << descriptor.additive_offset
         << ",\"startTimeSeconds\":" << descriptor.start_time_seconds << ",\"provenance\":{";
  bool first = true;
  for (const auto& [field, provenance] : descriptor.provenance) {
    if (!first)
      output << ',';
    first = false;
    output << '\"' << json_escape(field) << "\":{\"source\":\"" << origin_name(provenance.origin)
           << "\",\"confirmed\":" << (provenance.confirmed ? "true" : "false") << '}';
  }
  output << "}}";
  return output.str();
}

core::Result<SignalDescriptor> parse_sidecar(std::string_view json) {
  JsonValue document;
  JsonParser parser{json};
  if (!parser.parse(document) || document.kind != JsonValue::Kind::object) {
    return data_error(core::ErrorReason::invalid_argument, "Sidecar is not one complete JSON object", parser.error());
  }
  const auto require_string = [&](std::string_view key) -> core::Result<std::string> {
    const auto* value = member(document, key, JsonValue::Kind::string);
    if (!value)
      return data_error(core::ErrorReason::invalid_argument, "Sidecar string field is missing or mistyped",
                        std::string{key});
    return value->text;
  };
  const auto require_double = [&](std::string_view key) -> core::Result<double> {
    const auto* value = member(document, key, JsonValue::Kind::number);
    if (!value)
      return data_error(core::ErrorReason::invalid_argument, "Sidecar number field is missing or mistyped",
                        std::string{key});
    const auto parsed = json_double(*value);
    if (!parsed)
      return data_error(core::ErrorReason::invalid_argument, "Sidecar number is invalid", std::string{key});
    return *parsed;
  };
  const auto require_uint64 = [&](std::string_view key) -> core::Result<std::uint64_t> {
    const auto* value = member(document, key, JsonValue::Kind::number);
    if (!value)
      return data_error(core::ErrorReason::invalid_argument, "Sidecar integer field is missing or mistyped",
                        std::string{key});
    const auto parsed = json_uint64(*value);
    if (!parsed)
      return data_error(core::ErrorReason::invalid_argument, "Sidecar uint64 is invalid", std::string{key});
    return *parsed;
  };

  SignalDescriptor result;
  const auto schema = require_string("schema");
  if (!schema)
    return schema.error();
  result.schema = schema.value();
  const auto kind = require_string("signalKind");
  if (!kind)
    return kind.error();
  if (kind.value() == "real")
    result.signal_kind = SignalKind::real;
  else if (kind.value() == "complex")
    result.signal_kind = SignalKind::complex;
  else
    return data_error(core::ErrorReason::invalid_argument, "Unknown signalKind", kind.value());
  const auto scalar = require_string("scalarType");
  if (!scalar)
    return scalar.error();
  const auto parsed_scalar = parse_scalar(scalar.value());
  if (!parsed_scalar)
    return data_error(core::ErrorReason::invalid_argument, "Unknown scalarType", scalar.value());
  result.scalar_type = *parsed_scalar;
  const auto layout = require_string("componentLayout");
  if (!layout)
    return layout.error();
  if (layout.value() == "real")
    result.component_layout = ComponentLayout::real;
  else if (layout.value() == "interleaved")
    result.component_layout = ComponentLayout::interleaved;
  else if (layout.value() == "planar")
    result.component_layout = ComponentLayout::planar;
  else
    return data_error(core::ErrorReason::invalid_argument, "Unknown componentLayout", layout.value());
  const auto order = require_string("componentOrder");
  if (!order)
    return order.error();
  if (order.value() == "not_applicable")
    result.component_order = ComponentOrder::not_applicable;
  else if (order.value() == "IQ")
    result.component_order = ComponentOrder::iq;
  else if (order.value() == "QI")
    result.component_order = ComponentOrder::qi;
  else
    return data_error(core::ErrorReason::invalid_argument, "Unknown componentOrder", order.value());
  const auto endian = require_string("endianness");
  if (!endian)
    return endian.error();
  if (endian.value() == "not_applicable")
    result.endianness = Endianness::not_applicable;
  else if (endian.value() == "little")
    result.endianness = Endianness::little;
  else if (endian.value() == "big")
    result.endianness = Endianness::big;
  else
    return data_error(core::ErrorReason::invalid_argument, "Unknown endianness", endian.value());
  const auto sample_rate = require_double("sampleRateHz");
  const auto byte_offset = require_uint64("byteOffset");
  const auto range_begin = require_uint64("requestedStart");
  const auto range_end = require_uint64("requestedEnd");
  const auto mode = require_string("amplitudeMode");
  const auto scale = require_double("scaleFactor");
  const auto additive = require_double("additiveOffset");
  const auto start_time = require_double("startTimeSeconds");
  if (!sample_rate)
    return sample_rate.error();
  if (!byte_offset)
    return byte_offset.error();
  if (!range_begin)
    return range_begin.error();
  if (!range_end)
    return range_end.error();
  if (!mode)
    return mode.error();
  if (!scale)
    return scale.error();
  if (!additive)
    return additive.error();
  if (!start_time)
    return start_time.error();
  result.sample_rate_hz = sample_rate.value();
  result.byte_offset = byte_offset.value();
  const auto range = SampleRange::make(range_begin.value(), range_end.value());
  if (!range)
    return range.error();
  result.requested_sample_range = range.value();
  result.amplitude_mode = mode.value();
  result.scale_factor = scale.value();
  result.additive_offset = additive.value();
  result.start_time_seconds = start_time.value();
  const auto center = document.object.find("centerFrequencyHz");
  if (center == document.object.end()) {
    return data_error(core::ErrorReason::invalid_argument, "Sidecar centerFrequencyHz field is missing");
  }
  if (center->second.kind == JsonValue::Kind::number) {
    const auto parsed = json_double(center->second);
    if (!parsed)
      return data_error(core::ErrorReason::invalid_argument, "Center frequency is invalid");
    result.center_frequency_hz = *parsed;
  } else if (center->second.kind != JsonValue::Kind::null_value) {
    return data_error(core::ErrorReason::invalid_argument, "Center frequency must be a number or null");
  }

  const auto* provenance = member(document, "provenance", JsonValue::Kind::object);
  if (!provenance)
    return data_error(core::ErrorReason::invalid_argument, "Sidecar provenance must be an object");
  for (const auto& [field, value] : provenance->object) {
    if (value.kind != JsonValue::Kind::object) {
      return data_error(core::ErrorReason::invalid_argument, "Provenance entry must be an object", field);
    }
    const auto* source = member(value, "source", JsonValue::Kind::string);
    const auto* confirmed = member(value, "confirmed", JsonValue::Kind::boolean);
    if (!source || !confirmed || value.object.size() != 2U) {
      return data_error(core::ErrorReason::invalid_argument, "Provenance entry has an invalid structure", field);
    }
    const auto origin = parse_origin(source->text);
    if (!origin)
      return data_error(core::ErrorReason::invalid_argument, "Unknown provenance source");
    result.provenance.emplace(field, FieldProvenance{*origin, confirmed->boolean});
  }
  const auto validation = result.validate();
  if (!validation)
    return validation;
  return result;
}

DescriptorFormatAdapter::DescriptorFormatAdapter(std::string id, std::vector<std::string> extensions,
                                                 DescriptorFactory factory)
    : id_(std::move(id)), extensions_(std::move(extensions)), factory_(std::move(factory)) {
  for (auto& extension : extensions_)
    extension = lower_ascii(std::move(extension));
}

std::string_view DescriptorFormatAdapter::id() const noexcept {
  return id_;
}

bool DescriptorFormatAdapter::probe(const std::filesystem::path& path) const {
  const auto extension = lower_ascii(path.extension().string());
  return std::find(extensions_.begin(), extensions_.end(), extension) != extensions_.end();
}

core::Result<AdapterDescriptor> DescriptorFormatAdapter::describe(const std::filesystem::path& path) const {
  if (!probe(path))
    return data_error(core::ErrorReason::invalid_argument, "Adapter does not accept this extension");
  if (!factory_)
    return data_error(core::ErrorReason::unavailable, "Descriptor factory is unavailable");
  auto descriptor = factory_(path);
  if (!descriptor)
    return descriptor.error();
  const auto validation = descriptor.value().validate();
  if (!validation)
    return validation;
  return AdapterDescriptor{std::move(descriptor.value()), id_, true};
}

core::Status FormatAdapterRegistry::add(std::shared_ptr<IFormatAdapter> adapter) {
  if (!adapter || adapter->id().empty()) {
    return data_error(core::ErrorReason::invalid_argument, "Adapter and adapter id are required");
  }
  const std::string id{adapter->id()};
  if (adapters_.contains(id)) {
    return data_error(core::ErrorReason::invalid_argument, "Adapter id is already registered", id);
  }
  adapters_.emplace(id, std::move(adapter));
  return core::Status::success();
}

std::shared_ptr<const IFormatAdapter> FormatAdapterRegistry::find(std::string_view id) const {
  const auto iterator = adapters_.find(id);
  return iterator == adapters_.end() ? nullptr : iterator->second;
}

core::Result<AdapterDescriptor> FormatAdapterRegistry::describe(const std::filesystem::path& path) const {
  for (const auto& [id, adapter] : adapters_) {
    static_cast<void>(id);
    if (adapter->probe(path))
      return adapter->describe(path);
  }
  return data_error(core::ErrorReason::unavailable, "No registered format adapter accepts the source");
}

std::vector<std::string> FormatAdapterRegistry::adapter_ids() const {
  std::vector<std::string> ids;
  ids.reserve(adapters_.size());
  for (const auto& [id, adapter] : adapters_) {
    static_cast<void>(adapter);
    ids.push_back(id);
  }
  return ids;
}

core::Status register_standard_descriptor_adapters(FormatAdapterRegistry& registry, DescriptorFactory mat_factory,
                                                   DescriptorFactory numpy_factory, DescriptorFactory vendor_factory) {
  auto status = registry.add(std::make_shared<DescriptorFormatAdapter>(
      "mat-descriptor", std::vector<std::string>{".mat"}, std::move(mat_factory)));
  if (!status)
    return status;
  status = registry.add(std::make_shared<DescriptorFormatAdapter>(
      "numpy-descriptor", std::vector<std::string>{".npy", ".npz"}, std::move(numpy_factory)));
  if (!status)
    return status;
  return registry.add(std::make_shared<DescriptorFormatAdapter>(
      "vendor-descriptor", std::vector<std::string>{".sc16", ".shr", ".vendor"}, std::move(vendor_factory)));
}

} // namespace signal::data
