#pragma once

#include "signal_studio/core/error.hpp"

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace signal::core {

template <typename T> class Result final {
public:
  static_assert(!std::is_void_v<T>, "use Result<void> specialization");
  Result(T value) : value_(std::move(value)) {}
  Result(Status error) : value_(std::move(error)) {
    if (std::get<Status>(value_).ok()) {
      throw std::invalid_argument("a successful Status cannot represent a failed Result");
    }
  }

  [[nodiscard]] bool ok() const noexcept {
    return std::holds_alternative<T>(value_);
  }
  [[nodiscard]] explicit operator bool() const noexcept {
    return ok();
  }
  [[nodiscard]] const T& value() const& {
    return std::get<T>(value_);
  }
  [[nodiscard]] T& value() & {
    return std::get<T>(value_);
  }
  [[nodiscard]] T&& value() && {
    return std::get<T>(std::move(value_));
  }
  [[nodiscard]] const Status& error() const& {
    return std::get<Status>(value_);
  }

private:
  std::variant<T, Status> value_;
};

template <> class Result<void> final {
public:
  Result() noexcept = default;
  Result(Status status) : status_(std::move(status)) {}

  [[nodiscard]] bool ok() const noexcept {
    return status_.ok();
  }
  [[nodiscard]] explicit operator bool() const noexcept {
    return ok();
  }
  [[nodiscard]] const Status& error() const noexcept {
    return status_;
  }

private:
  Status status_{};
};

} // namespace signal::core
