#include "signal_studio/core/services.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <variant>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace signal::core {
namespace {

Status failure(ErrorReason reason, std::string message, std::string diagnostic = {}) {
  return Status::failure({ErrorDomain::core, reason}, std::move(message), std::move(diagnostic));
}

std::string path_utf8(const std::filesystem::path& path) {
  const auto text = path.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path utf8_path(std::string_view text) {
  const auto* begin = reinterpret_cast<const char8_t*>(text.data());
  return std::filesystem::path{std::u8string{begin, begin + text.size()}};
}

std::string quote_json(std::string_view text) {
  std::ostringstream output;
  output << '"';
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char character : text) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20U) {
        output << "\\u00" << hex[character >> 4U] << hex[character & 0x0fU];
      } else {
        output << static_cast<char>(character);
      }
    }
  }
  output << '"';
  return output.str();
}

struct JsonValue final {
  using Object = std::map<std::string, JsonValue>;
  using Array = std::vector<JsonValue>;
  std::variant<std::nullptr_t, bool, std::uint64_t, std::string, Object, Array> value{nullptr};
};

class JsonParser final {
public:
  explicit JsonParser(std::string_view input) : input_(input) {}

  Result<JsonValue> parse() {
    skip_space();
    auto value = parse_value(0);
    if (!value)
      return value.error();
    skip_space();
    if (position_ != input_.size())
      return invalid("JSON contains trailing data");
    return std::move(value).value();
  }

private:
  Result<JsonValue> invalid(std::string message) const {
    return failure(ErrorReason::invalid_argument, "Workspace JSON is invalid",
                   std::move(message) + " at byte " + std::to_string(position_));
  }

  void skip_space() {
    while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
      ++position_;
    }
  }

  Result<JsonValue> parse_value(std::size_t depth) {
    if (depth > 64U)
      return invalid("JSON nesting exceeds 64 levels");
    skip_space();
    if (position_ >= input_.size())
      return invalid("unexpected end of JSON");
    const char token = input_[position_];
    if (token == '{')
      return parse_object(depth + 1U);
    if (token == '[')
      return parse_array(depth + 1U);
    if (token == '"') {
      auto string = parse_string();
      if (!string)
        return string.error();
      return JsonValue{std::move(string).value()};
    }
    if (token >= '0' && token <= '9')
      return parse_number();
    if (consume("true"))
      return JsonValue{true};
    if (consume("false"))
      return JsonValue{false};
    if (consume("null"))
      return JsonValue{};
    return invalid("unexpected JSON token");
  }

  Result<JsonValue> parse_object(std::size_t depth) {
    ++position_;
    JsonValue::Object object;
    skip_space();
    if (position_ < input_.size() && input_[position_] == '}') {
      ++position_;
      return JsonValue{std::move(object)};
    }
    while (position_ < input_.size()) {
      auto key = parse_string();
      if (!key)
        return key.error();
      skip_space();
      if (position_ >= input_.size() || input_[position_] != ':')
        return invalid("object key lacks colon");
      ++position_;
      auto item = parse_value(depth);
      if (!item)
        return item.error();
      if (!object.emplace(std::move(key).value(), std::move(item).value()).second) {
        return invalid("duplicate object key");
      }
      skip_space();
      if (position_ >= input_.size())
        return invalid("unterminated object");
      if (input_[position_] == '}') {
        ++position_;
        return JsonValue{std::move(object)};
      }
      if (input_[position_] != ',')
        return invalid("object member lacks comma");
      ++position_;
      skip_space();
    }
    return invalid("unterminated object");
  }

  Result<JsonValue> parse_array(std::size_t depth) {
    ++position_;
    JsonValue::Array array;
    skip_space();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return JsonValue{std::move(array)};
    }
    while (position_ < input_.size()) {
      auto item = parse_value(depth);
      if (!item)
        return item.error();
      array.push_back(std::move(item).value());
      skip_space();
      if (position_ >= input_.size())
        return invalid("unterminated array");
      if (input_[position_] == ']') {
        ++position_;
        return JsonValue{std::move(array)};
      }
      if (input_[position_] != ',')
        return invalid("array item lacks comma");
      ++position_;
    }
    return invalid("unterminated array");
  }

  Result<std::string> parse_string() {
    skip_space();
    if (position_ >= input_.size() || input_[position_] != '"')
      return invalid("expected JSON string").error();
    ++position_;
    std::string result;
    while (position_ < input_.size()) {
      const unsigned char character = static_cast<unsigned char>(input_[position_++]);
      if (character == '"')
        return result;
      if (character < 0x20U)
        return invalid("unescaped control character").error();
      if (character != '\\') {
        result.push_back(static_cast<char>(character));
        continue;
      }
      if (position_ >= input_.size())
        return invalid("unterminated escape").error();
      const char escaped = input_[position_++];
      switch (escaped) {
      case '"':
        result.push_back('"');
        break;
      case '\\':
        result.push_back('\\');
        break;
      case '/':
        result.push_back('/');
        break;
      case 'b':
        result.push_back('\b');
        break;
      case 'f':
        result.push_back('\f');
        break;
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      case 'u': {
        if (position_ + 4U > input_.size())
          return invalid("short Unicode escape").error();
        unsigned value = 0;
        for (unsigned index = 0; index < 4U; ++index) {
          const char digit = input_[position_++];
          value <<= 4U;
          if (digit >= '0' && digit <= '9')
            value += static_cast<unsigned>(digit - '0');
          else if (digit >= 'a' && digit <= 'f')
            value += static_cast<unsigned>(digit - 'a' + 10);
          else if (digit >= 'A' && digit <= 'F')
            value += static_cast<unsigned>(digit - 'A' + 10);
          else
            return invalid("invalid Unicode escape").error();
        }
        if (value <= 0x7fU)
          result.push_back(static_cast<char>(value));
        else if (value <= 0x7ffU) {
          result.push_back(static_cast<char>(0xc0U | (value >> 6U)));
          result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
        } else {
          result.push_back(static_cast<char>(0xe0U | (value >> 12U)));
          result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
          result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
        }
        break;
      }
      default:
        return invalid("invalid string escape").error();
      }
    }
    return invalid("unterminated string").error();
  }

  Result<JsonValue> parse_number() {
    const std::size_t begin = position_;
    while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9')
      ++position_;
    std::uint64_t number = 0;
    const auto converted = std::from_chars(input_.data() + begin, input_.data() + position_, number);
    if (converted.ec != std::errc{} || converted.ptr != input_.data() + position_) {
      return invalid("invalid unsigned integer");
    }
    return JsonValue{number};
  }

  bool consume(std::string_view token) {
    if (input_.substr(position_, token.size()) != token)
      return false;
    position_ += token.size();
    return true;
  }

  std::string_view input_;
  std::size_t position_{};
};

std::string serialize_json(const JsonValue& value) {
  if (std::holds_alternative<std::nullptr_t>(value.value))
    return "null";
  if (const auto* boolean = std::get_if<bool>(&value.value))
    return *boolean ? "true" : "false";
  if (const auto* number = std::get_if<std::uint64_t>(&value.value))
    return std::to_string(*number);
  if (const auto* string = std::get_if<std::string>(&value.value))
    return quote_json(*string);
  if (const auto* object = std::get_if<JsonValue::Object>(&value.value)) {
    std::string output{"{"};
    bool first = true;
    for (const auto& [key, item] : *object) {
      if (!first)
        output += ',';
      first = false;
      output += quote_json(key) + ':' + serialize_json(item);
    }
    return output + '}';
  }
  const auto& array = std::get<JsonValue::Array>(value.value);
  std::string output{"["};
  for (std::size_t index = 0; index < array.size(); ++index) {
    if (index != 0U)
      output += ',';
    output += serialize_json(array[index]);
  }
  return output + ']';
}

Result<JsonValue> parse_embedded_json(std::string_view text) {
  return JsonParser{text}.parse();
}

const JsonValue* member(const JsonValue::Object& object, std::string_view key) {
  const auto found = object.find(std::string{key});
  return found == object.end() ? nullptr : &found->second;
}

Result<std::string> required_string(const JsonValue::Object& object, std::string_view key) {
  const auto* value = member(object, key);
  if (value == nullptr || !std::holds_alternative<std::string>(value->value)) {
    return failure(ErrorReason::invalid_argument, "Workspace field is invalid", std::string{key});
  }
  return std::get<std::string>(value->value);
}

Result<std::uint64_t> required_uint(const JsonValue::Object& object, std::string_view key) {
  const auto* value = member(object, key);
  if (value == nullptr || !std::holds_alternative<std::uint64_t>(value->value)) {
    return failure(ErrorReason::invalid_argument, "Workspace field is invalid", std::string{key});
  }
  return std::get<std::uint64_t>(value->value);
}

Result<bool> required_bool(const JsonValue::Object& object, std::string_view key) {
  const auto* value = member(object, key);
  if (value == nullptr || !std::holds_alternative<bool>(value->value)) {
    return failure(ErrorReason::invalid_argument, "Workspace field is invalid", std::string{key});
  }
  return std::get<bool>(value->value);
}

const JsonValue::Object* as_object(const JsonValue* value) {
  return value == nullptr ? nullptr : std::get_if<JsonValue::Object>(&value->value);
}

const JsonValue::Array* as_array(const JsonValue* value) {
  return value == nullptr ? nullptr : std::get_if<JsonValue::Array>(&value->value);
}

JsonValue embedded_value(std::string_view text) {
  auto parsed = parse_embedded_json(text);
  return parsed ? std::move(parsed).value() : JsonValue{std::string{text}};
}

JsonValue workspace_object_json(const WorkspaceObject& object) {
  JsonValue::Object attributes;
  for (const auto& [key, value] : object.attributes)
    attributes.emplace(key, JsonValue{value});
  JsonValue::Array relations;
  for (const auto& relation : object.relations)
    relations.push_back(JsonValue{relation});
  return JsonValue{JsonValue::Object{
      {"attributes", JsonValue{std::move(attributes)}},
      {"dataSourceVersionId", JsonValue{object.data_source_version_id}},
      {"id", JsonValue{object.id}},
      {"kind", JsonValue{object.kind}},
      {"relations", JsonValue{std::move(relations)}},
  }};
}

JsonValue workspace_json(const Workspace& workspace) {
  JsonValue::Array resources;
  for (const auto& source : workspace.data_sources) {
    resources.push_back(JsonValue{JsonValue::Object{
        {"descriptor", embedded_value(source.descriptor_json)},
        {"id", JsonValue{source.id}},
        {"loadedBegin", JsonValue{source.loaded_begin}},
        {"loadedEnd", JsonValue{source.loaded_end}},
        {"readOnly", JsonValue{source.read_only}},
        {"uri", JsonValue{source.relative_uri}},
        {"versionId", JsonValue{source.version_id}},
    }});
  }
  JsonValue::Array objects;
  for (const auto& object : workspace.objects)
    objects.push_back(workspace_object_json(object));
  JsonValue::Array tasks;
  for (const auto& task : workspace.tasks)
    tasks.push_back(workspace_object_json(task));
  JsonValue::Array results;
  for (const auto& result : workspace.results)
    results.push_back(workspace_object_json(result));
  JsonValue::Object extensions;
  for (const auto& [key, value] : workspace.extensions)
    extensions.emplace(key, embedded_value(value));
  const std::string schema = "signal.workspace/" + std::to_string(workspace.schema_version.major) + '.' +
                             std::to_string(workspace.schema_version.minor);
  return JsonValue{JsonValue::Object{
      {"extensions", JsonValue{std::move(extensions)}},
      {"objects", JsonValue{std::move(objects)}},
      {"resources", JsonValue{std::move(resources)}},
      {"results", JsonValue{std::move(results)}},
      {"schema", JsonValue{schema}},
      {"tasks", JsonValue{std::move(tasks)}},
      {"workspaceId", JsonValue{workspace.project_id}},
  }};
}

Status validate_workspace(const Workspace& workspace, bool saving) {
  if (workspace.project_id.empty())
    return failure(ErrorReason::invalid_argument, "Workspace ID cannot be empty");
  if (workspace.schema_version.major != WorkspaceStore::supported_schema.major) {
    return failure(ErrorReason::unavailable, "Workspace schema major version is unsupported");
  }
  if (saving && (workspace.read_only || workspace.schema_version != WorkspaceStore::supported_schema ||
                 workspace.loaded_schema_version.minor > WorkspaceStore::supported_schema.minor)) {
    return failure(ErrorReason::unavailable, "Read-only or newer workspace cannot be downgraded");
  }
  std::map<std::string, bool> source_ids;
  std::map<std::string, bool> source_versions;
  for (const auto& source : workspace.data_sources) {
    if (source.id.empty() || source.version_id.empty() || !source_ids.emplace(source.id, true).second ||
        !source_versions.emplace(source.version_id, true).second) {
      return failure(ErrorReason::invalid_argument, "Workspace data-source identity is invalid");
    }
    if (source.loaded_begin > source.loaded_end) {
      return failure(ErrorReason::invalid_argument, "Workspace loaded range is invalid");
    }
    const auto path_status = validate_relative_resource_path(utf8_path(source.relative_uri));
    if (!path_status)
      return path_status.with_context("workspace resource URI");
  }
  std::map<std::string, bool> graph_ids;
  const std::array collections{&workspace.objects, &workspace.tasks, &workspace.results};
  for (const auto* collection : collections) {
    for (const auto& object : *collection) {
      if (object.id.empty() || object.kind.empty() || object.data_source_version_id.empty() ||
          !graph_ids.emplace(object.id, true).second) {
        return failure(ErrorReason::invalid_argument, "Workspace graph object identity is invalid");
      }
    }
  }
  for (const auto* collection : collections) {
    for (const auto& object : *collection) {
      if (!source_versions.contains(object.data_source_version_id)) {
        return failure(ErrorReason::invalid_argument,
                       "Workspace graph object references an unknown data-source version", object.id);
      }
      for (const auto& relation : object.relations) {
        if (!graph_ids.contains(relation)) {
          return failure(ErrorReason::invalid_argument, "Workspace graph contains a dangling relation",
                         object.id + " -> " + relation);
        }
      }
    }
  }
  return Status::success();
}

Result<SchemaVersion> parse_schema(std::string_view schema) {
  constexpr std::string_view prefix = "signal.workspace/";
  if (!schema.starts_with(prefix))
    return failure(ErrorReason::invalid_argument, "Workspace schema is invalid");
  schema.remove_prefix(prefix.size());
  const auto dot = schema.find('.');
  if (dot == std::string_view::npos)
    return failure(ErrorReason::invalid_argument, "Workspace schema is invalid");
  SchemaVersion result;
  const auto major = std::from_chars(schema.data(), schema.data() + dot, result.major);
  const auto minor = std::from_chars(schema.data() + dot + 1U, schema.data() + schema.size(), result.minor);
  if (major.ec != std::errc{} || major.ptr != schema.data() + dot || minor.ec != std::errc{} ||
      minor.ptr != schema.data() + schema.size()) {
    return failure(ErrorReason::invalid_argument, "Workspace schema is invalid");
  }
  return result;
}

Result<Workspace> parse_workspace(std::string_view text, bool requested_read_only) {
  auto parsed = JsonParser{text}.parse();
  if (!parsed)
    return parsed.error();
  const auto* root = std::get_if<JsonValue::Object>(&parsed.value().value);
  if (root == nullptr)
    return failure(ErrorReason::invalid_argument, "Workspace root must be an object");
  auto schema_text = required_string(*root, "schema");
  if (!schema_text)
    return schema_text.error();
  auto loaded_schema = parse_schema(schema_text.value());
  if (!loaded_schema)
    return loaded_schema.error();
  if (loaded_schema.value().major != WorkspaceStore::supported_schema.major) {
    return failure(ErrorReason::unavailable, "Workspace schema major version is unsupported", schema_text.value());
  }
  auto workspace_id = required_string(*root, "workspaceId");
  if (!workspace_id)
    return workspace_id.error();
  Workspace workspace;
  workspace.loaded_schema_version = loaded_schema.value();
  workspace.schema_version = WorkspaceStore::supported_schema;
  workspace.project_id = std::move(workspace_id).value();
  workspace.read_only = requested_read_only || loaded_schema.value().minor > WorkspaceStore::supported_schema.minor;

  const auto* resources = as_array(member(*root, "resources"));
  const auto* objects = as_array(member(*root, "objects"));
  const auto* tasks = as_array(member(*root, "tasks"));
  const auto* results = as_array(member(*root, "results"));
  const auto* extensions = as_object(member(*root, "extensions"));
  if (resources == nullptr || extensions == nullptr || (member(*root, "objects") != nullptr && objects == nullptr) ||
      (member(*root, "tasks") != nullptr && tasks == nullptr) ||
      (member(*root, "results") != nullptr && results == nullptr)) {
    return failure(ErrorReason::invalid_argument, "Workspace collections are missing or invalid");
  }
  for (const auto& item : *resources) {
    const auto* object = std::get_if<JsonValue::Object>(&item.value);
    if (object == nullptr)
      return failure(ErrorReason::invalid_argument, "Workspace resource must be an object");
    WorkspaceDataSource source;
    auto id = required_string(*object, "id");
    auto version = required_string(*object, "versionId");
    auto uri = required_string(*object, "uri");
    auto begin = required_uint(*object, "loadedBegin");
    auto end = required_uint(*object, "loadedEnd");
    auto read_only = required_bool(*object, "readOnly");
    if (!id)
      return id.error();
    if (!version)
      return version.error();
    if (!uri)
      return uri.error();
    if (!begin)
      return begin.error();
    if (!end)
      return end.error();
    if (!read_only)
      return read_only.error();
    const auto* descriptor = member(*object, "descriptor");
    if (descriptor == nullptr)
      return failure(ErrorReason::invalid_argument, "Workspace descriptor is missing");
    source.id = std::move(id).value();
    source.version_id = std::move(version).value();
    source.relative_uri = std::move(uri).value();
    source.loaded_begin = begin.value();
    source.loaded_end = end.value();
    source.read_only = read_only.value();
    source.descriptor_json = serialize_json(*descriptor);
    workspace.data_sources.push_back(std::move(source));
  }
  const auto parse_objects = [](const JsonValue::Array* source, std::vector<WorkspaceObject>& destination) -> Status {
    if (source == nullptr)
      return Status::success();
    for (const auto& item : *source) {
      const auto* object = std::get_if<JsonValue::Object>(&item.value);
      if (object == nullptr)
        return failure(ErrorReason::invalid_argument, "Workspace object must be an object");
      WorkspaceObject workspace_object;
      auto id = required_string(*object, "id");
      auto kind = required_string(*object, "kind");
      auto version = required_string(*object, "dataSourceVersionId");
      if (!id)
        return id.error();
      if (!kind)
        return kind.error();
      if (!version)
        return version.error();
      workspace_object.id = std::move(id).value();
      workspace_object.kind = std::move(kind).value();
      workspace_object.data_source_version_id = std::move(version).value();
      const auto* attributes = as_object(member(*object, "attributes"));
      const auto* relations = as_array(member(*object, "relations"));
      if (attributes == nullptr || relations == nullptr) {
        return failure(ErrorReason::invalid_argument, "Workspace object collections are invalid");
      }
      for (const auto& [key, value] : *attributes) {
        const auto* string = std::get_if<std::string>(&value.value);
        if (string == nullptr)
          return failure(ErrorReason::invalid_argument, "Workspace attribute must be a string");
        workspace_object.attributes.emplace(key, *string);
      }
      for (const auto& relation : *relations) {
        const auto* string = std::get_if<std::string>(&relation.value);
        if (string == nullptr)
          return failure(ErrorReason::invalid_argument, "Workspace relation must be a string");
        workspace_object.relations.push_back(*string);
      }
      destination.push_back(std::move(workspace_object));
    }
    return Status::success();
  };
  auto objects_status = parse_objects(objects, workspace.objects);
  if (!objects_status)
    return objects_status;
  auto tasks_status = parse_objects(tasks, workspace.tasks);
  if (!tasks_status)
    return tasks_status;
  auto results_status = parse_objects(results, workspace.results);
  if (!results_status)
    return results_status;
  for (const auto& [key, value] : *extensions)
    workspace.extensions.emplace(key, serialize_json(value));
  const auto status = validate_workspace(workspace, false);
  if (!status)
    return status;
  return workspace;
}

class Sha256 final {
public:
  void update(std::span<const std::byte> input) {
    for (const std::byte value : input) {
      buffer_[buffer_size_++] = static_cast<std::uint8_t>(value);
      ++total_bytes_;
      if (buffer_size_ == buffer_.size()) {
        transform(buffer_);
        buffer_size_ = 0;
      }
    }
  }

  Hash256 finish() {
    const std::uint64_t bit_count = total_bytes_ * 8U;
    buffer_[buffer_size_++] = 0x80U;
    if (buffer_size_ > 56U) {
      while (buffer_size_ < buffer_.size())
        buffer_[buffer_size_++] = 0;
      transform(buffer_);
      buffer_size_ = 0;
    }
    while (buffer_size_ < 56U)
      buffer_[buffer_size_++] = 0;
    for (unsigned index = 0; index < 8U; ++index) {
      buffer_[63U - index] = static_cast<std::uint8_t>(bit_count >> (index * 8U));
    }
    transform(buffer_);
    Hash256 result;
    for (std::size_t index = 0; index < state_.size(); ++index) {
      result.bytes[index * 4U] = static_cast<std::uint8_t>(state_[index] >> 24U);
      result.bytes[index * 4U + 1U] = static_cast<std::uint8_t>(state_[index] >> 16U);
      result.bytes[index * 4U + 2U] = static_cast<std::uint8_t>(state_[index] >> 8U);
      result.bytes[index * 4U + 3U] = static_cast<std::uint8_t>(state_[index]);
    }
    return result;
  }

private:
  static constexpr std::uint32_t rotate(std::uint32_t value, unsigned amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
  }

  void transform(const std::array<std::uint8_t, 64>& block) {
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      words[index] = (static_cast<std::uint32_t>(block[index * 4U]) << 24U) |
                     (static_cast<std::uint32_t>(block[index * 4U + 1U]) << 16U) |
                     (static_cast<std::uint32_t>(block[index * 4U + 2U]) << 8U) |
                     static_cast<std::uint32_t>(block[index * 4U + 3U]);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
      const std::uint32_t s0 =
          rotate(words[index - 15U], 7U) ^ rotate(words[index - 15U], 18U) ^ (words[index - 15U] >> 3U);
      const std::uint32_t s1 =
          rotate(words[index - 2U], 17U) ^ rotate(words[index - 2U], 19U) ^ (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }
    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const std::uint32_t sum1 = rotate(e, 6U) ^ rotate(e, 11U) ^ rotate(e, 25U);
      const std::uint32_t choice = (e & f) ^ ((~e) & g);
      const std::uint32_t temporary1 = h + sum1 + choice + constants[index] + words[index];
      const std::uint32_t sum0 = rotate(a, 2U) ^ rotate(a, 13U) ^ rotate(a, 22U);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_size_{};
  std::uint64_t total_bytes_{};
};

std::vector<std::byte> as_bytes(std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  return bytes;
}

} // namespace

std::string Hash256::hex() const {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(64U, '0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    result[index * 2U] = digits[bytes[index] >> 4U];
    result[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
  }
  return result;
}

Result<Hash256> hash_bytes(std::span<const std::byte> bytes) {
  Sha256 hasher;
  hasher.update(bytes);
  return hasher.finish();
}

Result<Hash256> hash_file(const std::filesystem::path& path, std::size_t chunk_bytes) {
  if (chunk_bytes == 0U || chunk_bytes > 64U * 1024U * 1024U) {
    return failure(ErrorReason::invalid_argument, "Hash chunk size is outside the supported range");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return failure(ErrorReason::unavailable, "Source file cannot be opened", path_utf8(path));
  std::vector<std::byte> buffer(chunk_bytes);
  Sha256 hasher;
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0)
      hasher.update(std::span<const std::byte>{buffer.data(), static_cast<std::size_t>(count)});
  }
  if (!input.eof())
    return failure(ErrorReason::internal_failure, "Source file read failed", path_utf8(path));
  return hasher.finish();
}

void MemoryLogger::write(const LogEvent& event) noexcept {
  try {
    std::lock_guard lock{mutex_};
    events_.push_back(event);
  } catch (...) {
  }
}

std::vector<LogEvent> MemoryLogger::snapshot() const {
  std::lock_guard lock{mutex_};
  return events_;
}

Status Configuration::set(std::string key, std::string value) {
  if (key.empty() || key.size() > 256U || value.size() > 1024U * 1024U ||
      !std::all_of(key.begin(), key.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '.' || character == '_' || character == '-';
      })) {
    return failure(ErrorReason::invalid_argument, "Configuration key or value is invalid");
  }
  std::unique_lock lock{mutex_};
  values_.insert_or_assign(std::move(key), std::move(value));
  return Status::success();
}

std::optional<std::string> Configuration::get(std::string_view key) const {
  std::shared_lock lock{mutex_};
  const auto found = values_.find(std::string{key});
  return found == values_.end() ? std::nullopt : std::optional<std::string>{found->second};
}

Result<std::uint64_t> Configuration::get_uint64(std::string_view key, std::uint64_t minimum,
                                                std::uint64_t maximum) const {
  if (minimum > maximum)
    return failure(ErrorReason::invalid_argument, "Configuration numeric range is invalid");
  const auto value = get(key);
  if (!value)
    return failure(ErrorReason::unavailable, "Configuration key is not set", std::string{key});
  std::uint64_t number = 0;
  const auto parsed = std::from_chars(value->data(), value->data() + value->size(), number);
  if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size() || number < minimum || number > maximum) {
    return failure(ErrorReason::invalid_argument, "Configuration value is outside the required range",
                   std::string{key});
  }
  return number;
}

std::map<std::string, std::string> Configuration::snapshot() const {
  std::shared_lock lock{mutex_};
  return values_;
}

Status validate_relative_resource_path(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return failure(ErrorReason::invalid_argument, "Resource path must be relative");
  }
  const std::string text = path_utf8(path);
  if (text.find(':') != std::string::npos || text.starts_with("//") || text.starts_with("\\\\")) {
    return failure(ErrorReason::invalid_argument, "Resource path contains a device, URI, or UNC prefix");
  }
  for (const auto& component : path) {
    if (component == "..")
      return failure(ErrorReason::invalid_argument, "Resource path traversal is forbidden");
  }
  return Status::success();
}

Result<std::filesystem::path> resolve_relative_resource(const std::filesystem::path& project_file,
                                                        const std::filesystem::path& resource) {
  const auto status = validate_relative_resource_path(resource);
  if (!status)
    return status;
  std::error_code error;
  const auto parent = std::filesystem::weakly_canonical(project_file.parent_path(), error);
  if (error)
    return failure(ErrorReason::invalid_argument, "Project directory cannot be resolved", error.message());
  const auto resolved = std::filesystem::weakly_canonical(parent / resource, error);
  if (error)
    return failure(ErrorReason::invalid_argument, "Resource path cannot be resolved", error.message());
  const auto relative = resolved.lexically_relative(parent);
  if (relative.empty() || relative.begin() == relative.end() || *relative.begin() == "..") {
    return failure(ErrorReason::invalid_argument, "Resolved resource escapes the project directory");
  }
  return resolved;
}

Status AtomicFileStore::write(const std::filesystem::path& destination, std::span<const std::byte> content) const {
  if (destination.empty())
    return failure(ErrorReason::invalid_argument, "Destination path is empty");
  std::error_code error;
  if (!destination.parent_path().empty())
    std::filesystem::create_directories(destination.parent_path(), error);
  if (error)
    return failure(ErrorReason::unavailable, "Destination directory cannot be created", error.message());
  auto temporary = destination;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
      return failure(ErrorReason::unavailable, "Temporary file cannot be opened", path_utf8(temporary));
    output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output)
      return failure(ErrorReason::internal_failure, "Temporary file write failed", path_utf8(temporary));
  }
#ifdef _WIN32
  const HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return failure(ErrorReason::internal_failure, "Temporary file sync open failed");
  const BOOL flushed = FlushFileBuffers(handle);
  CloseHandle(handle);
  if (flushed == FALSE)
    return failure(ErrorReason::internal_failure, "Temporary file sync failed");
  if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
      FALSE) {
    return failure(ErrorReason::internal_failure, "Atomic file replacement failed", std::to_string(GetLastError()));
  }
#else
  std::filesystem::rename(temporary, destination, error);
  if (error)
    return failure(ErrorReason::internal_failure, "Atomic file replacement failed", error.message());
#endif
  return Status::success();
}

Result<std::vector<std::byte>> AtomicFileStore::read(const std::filesystem::path& source,
                                                     std::uint64_t maximum_bytes) const {
  std::error_code error;
  const auto size = std::filesystem::file_size(source, error);
  if (error)
    return failure(ErrorReason::unavailable, "File size cannot be read", error.message());
  if (size > maximum_bytes || size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return failure(ErrorReason::invalid_argument, "File exceeds the bounded read limit");
  }
  std::vector<std::byte> content(static_cast<std::size_t>(size));
  std::ifstream input(source, std::ios::binary);
  if (!input)
    return failure(ErrorReason::unavailable, "File cannot be opened", path_utf8(source));
  input.read(reinterpret_cast<char*>(content.data()), static_cast<std::streamsize>(content.size()));
  if (!input && !input.eof())
    return failure(ErrorReason::internal_failure, "File read failed", path_utf8(source));
  return content;
}

Status AtomicFileStore::recover(const std::filesystem::path& destination) const {
  auto temporary = destination;
  temporary += ".tmp";
  std::error_code error;
  if (!std::filesystem::exists(temporary, error))
    return Status::success();
  if (error)
    return failure(ErrorReason::internal_failure, "Recovery file cannot be inspected", error.message());
  if (std::filesystem::exists(destination, error)) {
    std::filesystem::remove(temporary, error);
    return error ? failure(ErrorReason::internal_failure, "Stale recovery file cannot be removed", error.message())
                 : Status::success();
  }
#ifdef _WIN32
  if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) {
    return failure(ErrorReason::internal_failure, "Recovery file promotion failed", std::to_string(GetLastError()));
  }
#else
  std::filesystem::rename(temporary, destination, error);
  if (error)
    return failure(ErrorReason::internal_failure, "Recovery file promotion failed", error.message());
#endif
  return Status::success();
}

Result<SourceFingerprint> fingerprint_source(const std::filesystem::path& path, std::size_t sample_bytes) {
  if (sample_bytes == 0U || sample_bytes > 4U * 1024U * 1024U) {
    return failure(ErrorReason::invalid_argument, "Fingerprint sample size is outside the supported range");
  }
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error)
    return failure(ErrorReason::unavailable, "Source path cannot be canonicalized", error.message());
  const auto size = std::filesystem::file_size(canonical, error);
  if (error)
    return failure(ErrorReason::unavailable, "Source size cannot be read", error.message());
  const auto modified = std::filesystem::last_write_time(canonical, error);
  if (error)
    return failure(ErrorReason::unavailable, "Source modification time cannot be read", error.message());
  std::ifstream input(canonical, std::ios::binary);
  if (!input)
    return failure(ErrorReason::unavailable, "Source cannot be opened", path_utf8(canonical));
  Sha256 sample_hasher;
  std::vector<std::byte> buffer(std::min<std::uint64_t>(sample_bytes, size));
  const std::array<std::uint64_t, 3> offsets{
      0U,
      size > buffer.size() ? (size - buffer.size()) / 2U : 0U,
      size > buffer.size() ? size - buffer.size() : 0U,
  };
  std::uint64_t previous = std::numeric_limits<std::uint64_t>::max();
  for (const auto offset : offsets) {
    if (offset == previous || buffer.empty())
      continue;
    previous = offset;
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count != static_cast<std::streamsize>(buffer.size())) {
      return failure(ErrorReason::internal_failure, "Source fingerprint sampling failed", path_utf8(canonical));
    }
    sample_hasher.update(std::span<const std::byte>{buffer.data(), static_cast<std::size_t>(count)});
  }
  SourceFingerprint fingerprint;
  fingerprint.canonical_path = path_utf8(canonical);
  fingerprint.size_bytes = size;
  fingerprint.modified_ticks = static_cast<std::int64_t>(modified.time_since_epoch().count());
  fingerprint.sampled_hash = sample_hasher.finish();
  const std::string identity = fingerprint.canonical_path + '\n' + std::to_string(fingerprint.size_bytes) + '\n' +
                               std::to_string(fingerprint.modified_ticks) + '\n' + fingerprint.sampled_hash.hex();
  auto identity_hash = hash_bytes(as_bytes(identity));
  if (!identity_hash)
    return identity_hash.error();
  fingerprint.version_id = identity_hash.value().hex();
  return fingerprint;
}

Result<Workspace> WorkspaceStore::create(ProjectId project_id) const {
  Workspace workspace;
  workspace.schema_version = supported_schema;
  workspace.loaded_schema_version = supported_schema;
  workspace.project_id = std::move(project_id);
  const auto status = validate_workspace(workspace, false);
  if (!status)
    return status;
  return workspace;
}

Status WorkspaceStore::save(const std::filesystem::path& path, const Workspace& workspace) const {
  const auto status = validate_workspace(workspace, true);
  if (!status)
    return status;
  const std::string text = serialize_json(workspace_json(workspace)) + '\n';
  return AtomicFileStore{}.write(path, as_bytes(text));
}

Result<Workspace> WorkspaceStore::load(const std::filesystem::path& path, bool read_only) const {
  const auto recovered = AtomicFileStore{}.recover(path);
  if (!recovered)
    return recovered;
  auto content = AtomicFileStore{}.read(path, 64U * 1024U * 1024U);
  if (!content)
    return content.error();
  const auto* characters = reinterpret_cast<const char*>(content.value().data());
  return parse_workspace(std::string_view{characters, content.value().size()}, read_only);
}

Status WorkspaceStore::autosave(const std::filesystem::path& project_path, const Workspace& workspace) const {
  auto autosave_path = project_path;
  autosave_path += ".autosave";
  return save(autosave_path, workspace);
}

Result<Workspace> WorkspaceStore::recover_autosave(const std::filesystem::path& project_path) const {
  auto autosave_path = project_path;
  autosave_path += ".autosave";
  return load(autosave_path, false);
}

Status WorkspaceStore::close(Workspace& workspace) const noexcept {
  workspace = Workspace{};
  return Status::success();
}

Status WorkspaceStore::relocate(Workspace& workspace, const std::filesystem::path& old_root,
                                const std::filesystem::path& new_root) const {
  if (workspace.read_only)
    return failure(ErrorReason::unavailable, "Read-only workspace cannot be relocated");
  std::error_code error;
  if (!std::filesystem::is_directory(old_root, error) || !std::filesystem::is_directory(new_root, error)) {
    return failure(ErrorReason::invalid_argument, "Relocation roots must be existing directories");
  }
  for (const auto& source : workspace.data_sources) {
    const auto relative = utf8_path(source.relative_uri);
    const auto path_status = validate_relative_resource_path(relative);
    if (!path_status)
      return path_status;
    if (!std::filesystem::is_regular_file(new_root / relative, error)) {
      return failure(ErrorReason::unavailable, "Relocated resource is missing", path_utf8(new_root / relative));
    }
  }
  return Status::success();
}

RecentProjectStore::RecentProjectStore(std::filesystem::path storage_path) : storage_path_(std::move(storage_path)) {}

Status RecentProjectStore::record(const std::filesystem::path& project_path) {
  std::vector<std::filesystem::path> projects;
  auto loaded = load();
  if (!loaded)
    return loaded.error();
  projects = std::move(loaded).value();
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(project_path, error);
  if (error)
    return failure(ErrorReason::invalid_argument, "Recent project path cannot be resolved", error.message());
  projects.erase(std::remove(projects.begin(), projects.end(), canonical), projects.end());
  projects.insert(projects.begin(), canonical);
  if (projects.size() > 16U)
    projects.resize(16U);
  std::string text;
  for (const auto& project : projects)
    text += path_utf8(project) + '\n';
  return AtomicFileStore{}.write(storage_path_, as_bytes(text));
}

Result<std::vector<std::filesystem::path>> RecentProjectStore::load() const {
  std::error_code error;
  if (!std::filesystem::exists(storage_path_, error))
    return std::vector<std::filesystem::path>{};
  auto content = AtomicFileStore{}.read(storage_path_, 1024U * 1024U);
  if (!content)
    return content.error();
  const auto* characters = reinterpret_cast<const char*>(content.value().data());
  std::istringstream input(std::string{characters, content.value().size()});
  std::vector<std::filesystem::path> projects;
  std::string line;
  while (std::getline(input, line) && projects.size() < 16U) {
    if (!line.empty())
      projects.push_back(utf8_path(line));
  }
  return projects;
}

Status CurrentContextStore::switch_to(CurrentContext context) {
  if (context.project_id.empty() || context.data_source_id.empty() || context.data_source_version_id.empty()) {
    return failure(ErrorReason::invalid_argument, "Current project and data-source identities are required");
  }
  std::unique_lock lock{mutex_};
  context.generation = current_.generation + 1U;
  current_ = std::move(context);
  return Status::success();
}

CurrentContext CurrentContextStore::snapshot() const {
  std::shared_lock lock{mutex_};
  return current_;
}

Status CurrentContextStore::validate_consistency(std::span<const CurrentContext> consumers) const {
  const auto expected = snapshot();
  for (const auto& consumer : consumers) {
    if (consumer != expected) {
      return failure(ErrorReason::invalid_argument, "Current context consumers are inconsistent",
                     "analysis must be blocked until project, data source, version, and generation match");
    }
  }
  return Status::success();
}

Result<std::vector<WorkspaceObject>> CurrentContextStore::select_current_objects(const Workspace& workspace,
                                                                                 std::string_view kind) const {
  const auto current = snapshot();
  if (current.project_id.empty() || current.data_source_version_id.empty() ||
      current.project_id != workspace.project_id) {
    return failure(ErrorReason::invalid_argument, "Workspace and current context do not identify the same project");
  }
  const auto source = std::find_if(
      workspace.data_sources.begin(), workspace.data_sources.end(), [&](const WorkspaceDataSource& candidate) {
        return candidate.id == current.data_source_id && candidate.version_id == current.data_source_version_id;
      });
  if (source == workspace.data_sources.end()) {
    return failure(ErrorReason::invalid_argument,
                   "Current data source and version are not present in the workspace graph");
  }
  std::vector<WorkspaceObject> selected;
  const std::array collections{&workspace.objects, &workspace.tasks, &workspace.results};
  for (const auto* collection : collections) {
    for (const auto& object : *collection) {
      if (object.data_source_version_id == current.data_source_version_id && (kind.empty() || object.kind == kind)) {
        selected.push_back(object);
      }
    }
  }
  return selected;
}

} // namespace signal::core
