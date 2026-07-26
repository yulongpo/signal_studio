#pragma once

#include "signal_studio/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace signal::data {

class BoundedFileReader final {
public:
  explicit BoundedFileReader(std::filesystem::path path, std::uint64_t maximum_request_bytes);
  [[nodiscard]] core::Result<std::uint64_t> size() const;
  [[nodiscard]] core::Result<std::vector<std::byte>> read(std::uint64_t offset, std::uint64_t byte_count) const;
  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }
  [[nodiscard]] std::uint64_t maximum_request_bytes() const noexcept {
    return maximum_request_bytes_;
  }

private:
  std::filesystem::path path_;
  std::uint64_t maximum_request_bytes_{};
};

class MappedFileWindow final {
public:
  MappedFileWindow() = default;
  [[nodiscard]] static core::Result<MappedFileWindow> open(const std::filesystem::path& path, std::uint64_t offset,
                                                           std::uint64_t byte_count,
                                                           std::uint64_t maximum_window_bytes);
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
  [[nodiscard]] std::uint64_t file_offset() const noexcept {
    return file_offset_;
  }

private:
  std::shared_ptr<const void> lease_;
  const std::byte* data_{};
  std::size_t size_{};
  std::uint64_t file_offset_{};
};

} // namespace signal::data
