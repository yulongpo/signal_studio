#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace signal::core {

enum class ErrorDomain : std::uint16_t {
  none = 0, core = 1, data = 2, dsp = 3, compute = 4, task_runtime = 5,
  visualization = 6, workbench = 7, plugin_sdk = 8, model_runtime = 9, dataset = 10,
};

enum class ErrorReason : std::uint16_t {
  none = 0, invalid_argument = 1, unavailable = 2, cancelled = 3, internal_failure = 4,
};

enum class ErrorCategory : std::uint8_t { contract = 1, resource = 2, cancellation = 3, adapter = 4 };
enum class ErrorSeverity : std::uint8_t { info = 1, warning = 2, error = 3, critical = 4 };

struct ErrorCode final {
  ErrorDomain domain{ErrorDomain::none};
  ErrorReason reason{ErrorReason::none};

  [[nodiscard]] constexpr std::uint32_t stable_value() const noexcept {
    return static_cast<std::uint32_t>(domain) * 1'000U + static_cast<std::uint32_t>(reason);
  }
  [[nodiscard]] constexpr bool is_success() const noexcept {
    return domain == ErrorDomain::none && reason == ErrorReason::none;
  }
  [[nodiscard]] std::string stable_text() const;
  friend constexpr bool operator==(const ErrorCode&, const ErrorCode&) = default;
};

struct ErrorCause final {
  ErrorCode code;
  ErrorCategory category{ErrorCategory::contract};
  std::string technical_details;
};

struct ErrorDetails final {
  ErrorCategory category{ErrorCategory::contract};
  ErrorSeverity severity{ErrorSeverity::error};
  std::string user_message;
  std::string technical_details;
  std::vector<std::string> recovery_actions;
  bool retryable{};
  std::string task_id;
  std::string object_id;
  std::string data_source_version_id;
  std::vector<ErrorCause> cause_chain;
  std::vector<std::string> metrics_refs;
};

class Status final {
 public:
  Status() noexcept = default;
  [[nodiscard]] static Status success() noexcept;
  [[nodiscard]] static Status failure(ErrorCode code, ErrorDetails details);
  [[nodiscard]] static Status failure(ErrorCode code, std::string message, std::string diagnostic = {});

  [[nodiscard]] bool ok() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] ErrorCode code() const noexcept;
  [[nodiscard]] const ErrorDetails& details() const noexcept;
  [[nodiscard]] std::string_view message() const noexcept;
  [[nodiscard]] std::string_view diagnostic() const noexcept;
  [[nodiscard]] Status with_context(std::string_view context) const;
  [[nodiscard]] std::string serialize_json() const;

 private:
  Status(ErrorCode code, ErrorDetails details);
  ErrorCode code_{};
  ErrorDetails details_{};
};

[[nodiscard]] std::string_view error_domain_name(ErrorDomain domain) noexcept;
[[nodiscard]] std::string_view error_domain_token(ErrorDomain domain) noexcept;
[[nodiscard]] std::string_view error_reason_name(ErrorReason reason) noexcept;
[[nodiscard]] std::string_view error_category_name(ErrorCategory category) noexcept;
[[nodiscard]] std::string_view error_severity_name(ErrorSeverity severity) noexcept;
[[nodiscard]] Status validate_error(const ErrorCode& code, const ErrorDetails& details);

inline constexpr std::size_t max_error_cause_depth = 8;

}  // namespace signal::core
