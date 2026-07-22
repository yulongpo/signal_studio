#include "signal_studio/core/error.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace signal::core {
namespace {
std::string escape_json(std::string_view value) {
  std::ostringstream output;
  for (const unsigned char character : value) {
    switch (character) {
      case '\"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (character < 0x20U) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character);
        } else {
          output << static_cast<char>(character);
        }
    }
  }
  return output.str();
}

void append_strings(std::ostringstream& output, const std::vector<std::string>& values) {
  output << '[';
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) output << ',';
    output << '\"' << escape_json(values[index]) << '\"';
  }
  output << ']';
}

enum class StatusInvariant {
  valid,
  success_code,
  invalid_domain,
  invalid_reason,
  category_mismatch,
  invalid_severity,
  missing_user_message,
  missing_recovery_action,
  empty_recovery_action,
  invalid_retryability,
  cause_depth_exceeded,
  invalid_cause_code,
  cause_category_mismatch,
  missing_cause_details,
  empty_metrics_reference,
};

constexpr bool valid_domain(ErrorDomain domain) noexcept {
  const auto value = static_cast<std::uint16_t>(domain);
  return value >= static_cast<std::uint16_t>(ErrorDomain::core) &&
         value <= static_cast<std::uint16_t>(ErrorDomain::dataset);
}

constexpr bool valid_reason(ErrorReason reason) noexcept {
  const auto value = static_cast<std::uint16_t>(reason);
  return value >= static_cast<std::uint16_t>(ErrorReason::invalid_argument) &&
         value <= static_cast<std::uint16_t>(ErrorReason::internal_failure);
}

constexpr bool valid_severity(ErrorSeverity severity) noexcept {
  const auto value = static_cast<std::uint8_t>(severity);
  return value >= static_cast<std::uint8_t>(ErrorSeverity::info) &&
         value <= static_cast<std::uint8_t>(ErrorSeverity::critical);
}

constexpr ErrorCategory category_for(ErrorReason reason) noexcept {
  switch (reason) {
    case ErrorReason::invalid_argument: return ErrorCategory::contract;
    case ErrorReason::unavailable: return ErrorCategory::resource;
    case ErrorReason::cancelled: return ErrorCategory::cancellation;
    case ErrorReason::internal_failure: return ErrorCategory::adapter;
    case ErrorReason::none: return ErrorCategory::contract;
  }
  return ErrorCategory::contract;
}

StatusInvariant validate_failure_invariants(const ErrorCode& code, const ErrorDetails& details) noexcept {
  if (code.is_success()) return StatusInvariant::success_code;
  if (!valid_domain(code.domain)) return StatusInvariant::invalid_domain;
  if (!valid_reason(code.reason)) return StatusInvariant::invalid_reason;
  if (details.category != category_for(code.reason)) return StatusInvariant::category_mismatch;
  if (!valid_severity(details.severity)) return StatusInvariant::invalid_severity;
  if (details.user_message.empty()) return StatusInvariant::missing_user_message;
  if (details.recovery_actions.empty()) return StatusInvariant::missing_recovery_action;
  for (const auto& action : details.recovery_actions) {
    if (action.empty()) return StatusInvariant::empty_recovery_action;
  }
  if (details.retryable && code.reason != ErrorReason::unavailable &&
      code.reason != ErrorReason::internal_failure) {
    return StatusInvariant::invalid_retryability;
  }
  if (details.cause_chain.size() > max_error_cause_depth) return StatusInvariant::cause_depth_exceeded;
  for (const auto& cause : details.cause_chain) {
    if (cause.code.is_success() || !valid_domain(cause.code.domain) || !valid_reason(cause.code.reason)) {
      return StatusInvariant::invalid_cause_code;
    }
    if (cause.category != category_for(cause.code.reason)) return StatusInvariant::cause_category_mismatch;
    if (cause.technical_details.empty()) return StatusInvariant::missing_cause_details;
  }
  for (const auto& reference : details.metrics_refs) {
    if (reference.empty()) return StatusInvariant::empty_metrics_reference;
  }
  return StatusInvariant::valid;
}

std::string_view invariant_message(StatusInvariant invariant) noexcept {
  switch (invariant) {
    case StatusInvariant::valid: return "valid";
    case StatusInvariant::success_code: return "failure status requires a non-success code";
    case StatusInvariant::invalid_domain: return "error domain is outside BL1.0";
    case StatusInvariant::invalid_reason: return "error reason is outside BL1.0";
    case StatusInvariant::category_mismatch: return "error category does not match the stable reason";
    case StatusInvariant::invalid_severity: return "error severity is outside BL1.0";
    case StatusInvariant::missing_user_message: return "user_message is required";
    case StatusInvariant::missing_recovery_action: return "recovery_actions is required";
    case StatusInvariant::empty_recovery_action: return "recovery_actions cannot contain an empty action";
    case StatusInvariant::invalid_retryability: return "only unavailable or internal failures may be retryable";
    case StatusInvariant::cause_depth_exceeded: return "cause_chain exceeds the BL1.0 depth limit";
    case StatusInvariant::invalid_cause_code: return "cause_chain contains an invalid failure code";
    case StatusInvariant::cause_category_mismatch: return "cause_chain category does not match its stable reason";
    case StatusInvariant::missing_cause_details: return "cause_chain technical details are required";
    case StatusInvariant::empty_metrics_reference: return "metrics_refs cannot contain an empty reference";
  }
  return "invalid structured error";
}
}  // namespace

std::string ErrorCode::stable_text() const {
  if (is_success()) return "SS-OK";
  std::ostringstream output;
  output << "SS-" << error_domain_token(domain) << "-E" << std::setw(3) << std::setfill('0')
         << static_cast<unsigned>(reason);
  return output.str();
}

Status Status::success() noexcept { return {}; }

Status validate_error(const ErrorCode& code, const ErrorDetails& details) {
  const auto invariant = validate_failure_invariants(code, details);
  return invariant == StatusInvariant::valid
             ? Status::success()
             : Status::failure({ErrorDomain::core, ErrorReason::invalid_argument},
                               "Structured error validation failed", std::string{invariant_message(invariant)});
}

Status Status::failure(ErrorCode code, ErrorDetails details) {
  const auto invariant = validate_failure_invariants(code, details);
  if (invariant != StatusInvariant::valid) throw std::invalid_argument(std::string{invariant_message(invariant)});
  return {code, std::move(details)};
}

Status Status::failure(ErrorCode code, std::string message, std::string diagnostic) {
  ErrorDetails details;
  details.category = category_for(code.reason);
  details.severity = ErrorSeverity::error;
  details.user_message = std::move(message);
  details.technical_details = std::move(diagnostic);
  details.recovery_actions.emplace_back("Inspect details and correct the input");
  return failure(code, std::move(details));
}

Status::Status(ErrorCode code, ErrorDetails details) : code_(code), details_(std::move(details)) {}
bool Status::ok() const noexcept { return code_.is_success(); }
Status::operator bool() const noexcept { return ok(); }
ErrorCode Status::code() const noexcept { return code_; }
const ErrorDetails& Status::details() const noexcept { return details_; }
std::string_view Status::message() const noexcept { return details_.user_message; }
std::string_view Status::diagnostic() const noexcept { return details_.technical_details; }

Status Status::with_context(std::string_view context) const {
  if (ok() || context.empty()) return *this;
  ErrorDetails propagated = details_;
  propagated.cause_chain.push_back(
      {code_, details_.category, details_.technical_details.empty() ? details_.user_message : details_.technical_details});
  propagated.technical_details = std::string{context} + (details_.technical_details.empty() ? "" : ": " + details_.technical_details);
  return failure(code_, std::move(propagated));
}

std::string Status::serialize_json() const {
  if (ok()) return R"({"code":"SS-OK"})";
  const auto invariant = validate_failure_invariants(code_, details_);
  if (invariant != StatusInvariant::valid) {
    throw std::logic_error(std::string{"invalid Status cannot be serialized: "} +
                           std::string{invariant_message(invariant)});
  }
  std::ostringstream output;
  output << "{\"code\":\"" << code_.stable_text() << "\",\"category\":\"" << error_category_name(details_.category)
         << "\",\"severity\":\"" << error_severity_name(details_.severity) << "\",\"userMessage\":\""
         << escape_json(details_.user_message) << "\",\"technicalDetails\":\"" << escape_json(details_.technical_details)
         << "\",\"recoveryActions\":";
  append_strings(output, details_.recovery_actions);
  output << ",\"retryable\":" << (details_.retryable ? "true" : "false")
         << ",\"taskId\":\"" << escape_json(details_.task_id) << "\",\"objectId\":\"" << escape_json(details_.object_id)
         << "\",\"dataSourceVersionId\":\"" << escape_json(details_.data_source_version_id) << "\",\"causeChain\":[";
  for (std::size_t index = 0; index < details_.cause_chain.size(); ++index) {
    if (index != 0) output << ',';
    output << "{\"code\":\"" << details_.cause_chain[index].code.stable_text() << "\",\"category\":\""
           << error_category_name(details_.cause_chain[index].category) << "\",\"technicalDetails\":\""
           << escape_json(details_.cause_chain[index].technical_details) << "\"}";
  }
  output << "],\"metricsRef\":";
  append_strings(output, details_.metrics_refs);
  output << '}';
  return output.str();
}

std::string_view error_domain_name(ErrorDomain domain) noexcept {
  switch (domain) {
    case ErrorDomain::none: return "none"; case ErrorDomain::core: return "core"; case ErrorDomain::data: return "data";
    case ErrorDomain::dsp: return "dsp"; case ErrorDomain::compute: return "compute"; case ErrorDomain::task_runtime: return "task-runtime";
    case ErrorDomain::visualization: return "visualization"; case ErrorDomain::workbench: return "workbench";
    case ErrorDomain::plugin_sdk: return "plugin-sdk"; case ErrorDomain::model_runtime: return "model-runtime"; case ErrorDomain::dataset: return "dataset";
  }
  return "unknown";
}
std::string_view error_domain_token(ErrorDomain domain) noexcept {
  switch (domain) {
    case ErrorDomain::core: return "CORE"; case ErrorDomain::data: return "DATA"; case ErrorDomain::dsp: return "DSP";
    case ErrorDomain::compute: return "COMPUTE"; case ErrorDomain::task_runtime: return "TASK"; case ErrorDomain::visualization: return "VIS";
    case ErrorDomain::workbench: return "WB"; case ErrorDomain::plugin_sdk: return "PLG"; case ErrorDomain::model_runtime: return "MODEL";
    case ErrorDomain::dataset: return "DSET"; case ErrorDomain::none: return "OK";
  }
  return "UNKNOWN";
}
std::string_view error_reason_name(ErrorReason reason) noexcept {
  switch (reason) {
    case ErrorReason::none: return "none"; case ErrorReason::invalid_argument: return "invalid-argument";
    case ErrorReason::unavailable: return "unavailable"; case ErrorReason::cancelled: return "cancelled";
    case ErrorReason::internal_failure: return "internal-failure";
  }
  return "unknown";
}
std::string_view error_category_name(ErrorCategory category) noexcept {
  switch (category) { case ErrorCategory::contract: return "contract"; case ErrorCategory::resource: return "resource";
    case ErrorCategory::cancellation: return "cancellation"; case ErrorCategory::adapter: return "adapter"; }
  return "unknown";
}
std::string_view error_severity_name(ErrorSeverity severity) noexcept {
  switch (severity) { case ErrorSeverity::info: return "info"; case ErrorSeverity::warning: return "warning";
    case ErrorSeverity::error: return "error"; case ErrorSeverity::critical: return "critical"; }
  return "unknown";
}

}  // namespace signal::core
