#include "signal_studio/data/io.hpp"

#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace signal::data {
namespace {

core::Status data_error(core::ErrorReason reason, std::string message, std::string detail = {}) {
  return core::Status::failure({core::ErrorDomain::data, reason}, std::move(message), std::move(detail));
}

core::Result<std::uint64_t> checked_file_size(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::status(path, error);
  if (error || !std::filesystem::is_regular_file(status)) {
    return data_error(core::ErrorReason::unavailable, "Source is not an accessible regular file", error.message());
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return data_error(core::ErrorReason::unavailable, "Cannot query source file size", error.message());
  }
  return static_cast<std::uint64_t>(size);
}

} // namespace

BoundedFileReader::BoundedFileReader(std::filesystem::path path, std::uint64_t maximum_request_bytes)
    : path_(std::move(path)), maximum_request_bytes_(maximum_request_bytes) {}

core::Result<std::uint64_t> BoundedFileReader::size() const {
  return checked_file_size(path_);
}

core::Result<std::vector<std::byte>> BoundedFileReader::read(std::uint64_t offset, std::uint64_t byte_count) const {
  if (maximum_request_bytes_ == 0U || byte_count > maximum_request_bytes_) {
    return data_error(core::ErrorReason::invalid_argument, "File read exceeds its explicit request bound");
  }
  const auto source_size = size();
  if (!source_size)
    return source_size.error();
  if (offset > source_size.value() || byte_count > source_size.value() - offset) {
    return data_error(core::ErrorReason::invalid_argument, "File read exceeds the source bounds");
  }
  if (byte_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    return data_error(core::ErrorReason::unavailable, "File read exceeds this process address space");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(byte_count));
  if (bytes.empty())
    return bytes;
  std::ifstream input{path_, std::ios::binary};
  if (!input)
    return data_error(core::ErrorReason::unavailable, "Cannot open source for bounded reading");
  input.seekg(static_cast<std::streamoff>(offset));
  if (!input)
    return data_error(core::ErrorReason::unavailable, "Cannot seek to the bounded read offset");
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
    return data_error(core::ErrorReason::unavailable, "Bounded read returned fewer bytes than requested");
  }
  return bytes;
}

core::Result<MappedFileWindow> MappedFileWindow::open(const std::filesystem::path& path, std::uint64_t offset,
                                                      std::uint64_t byte_count, std::uint64_t maximum_window_bytes) {
  if (maximum_window_bytes == 0U || byte_count > maximum_window_bytes) {
    return data_error(core::ErrorReason::invalid_argument, "Mapped window exceeds its explicit byte bound");
  }
  const auto source_size = checked_file_size(path);
  if (!source_size)
    return source_size.error();
  if (offset > source_size.value() || byte_count > source_size.value() - offset) {
    return data_error(core::ErrorReason::invalid_argument, "Mapped window exceeds the source bounds");
  }
  MappedFileWindow result;
  result.file_offset_ = offset;
  if (byte_count == 0U)
    return result;
  if (byte_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return data_error(core::ErrorReason::unavailable, "Mapped window exceeds this process address space");
  }

#if defined(_WIN32)
  SYSTEM_INFO information{};
  GetSystemInfo(&information);
  const auto granularity = static_cast<std::uint64_t>(information.dwAllocationGranularity);
  const auto aligned_offset = offset - (offset % granularity);
  const auto delta = offset - aligned_offset;
  if (byte_count > std::numeric_limits<std::uint64_t>::max() - delta) {
    return data_error(core::ErrorReason::invalid_argument, "Mapped window alignment overflows uint64");
  }
  const auto mapped_bytes = byte_count + delta;
  if (mapped_bytes > static_cast<std::uint64_t>(std::numeric_limits<SIZE_T>::max())) {
    return data_error(core::ErrorReason::unavailable, "Aligned mapped window exceeds addressable memory");
  }
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return data_error(core::ErrorReason::unavailable, "Cannot open source for read-only mapping",
                      std::to_string(GetLastError()));
  }
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0U, 0U, nullptr);
  if (mapping == nullptr) {
    const auto code = GetLastError();
    CloseHandle(file);
    return data_error(core::ErrorReason::unavailable, "Cannot create read-only file mapping", std::to_string(code));
  }
  const DWORD high = static_cast<DWORD>(aligned_offset >> 32U);
  const DWORD low = static_cast<DWORD>(aligned_offset & 0xffffffffU);
  void* base = MapViewOfFile(mapping, FILE_MAP_READ, high, low, static_cast<SIZE_T>(mapped_bytes));
  if (base == nullptr) {
    const auto code = GetLastError();
    CloseHandle(mapping);
    CloseHandle(file);
    return data_error(core::ErrorReason::unavailable, "Cannot map requested read-only window", std::to_string(code));
  }
  result.lease_ = std::shared_ptr<const void>{base, [mapping, file](const void* pointer) {
                                                UnmapViewOfFile(pointer);
                                                CloseHandle(mapping);
                                                CloseHandle(file);
                                              }};
  result.data_ = static_cast<const std::byte*>(base) + static_cast<std::size_t>(delta);
#else
  const long queried_page_size = sysconf(_SC_PAGE_SIZE);
  if (queried_page_size <= 0) {
    return data_error(core::ErrorReason::unavailable, "Cannot query the system mapping page size");
  }
  const auto page_size = static_cast<std::uint64_t>(queried_page_size);
  const auto aligned_offset = offset - (offset % page_size);
  const auto delta = offset - aligned_offset;
  if (byte_count > std::numeric_limits<std::uint64_t>::max() - delta) {
    return data_error(core::ErrorReason::invalid_argument, "Mapped window alignment overflows uint64");
  }
  const auto mapped_bytes = byte_count + delta;
  if (mapped_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      aligned_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    return data_error(core::ErrorReason::unavailable, "Aligned mapped window exceeds addressable memory");
  }
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return data_error(core::ErrorReason::unavailable, "Cannot open source for read-only mapping",
                      std::to_string(errno));
  }
  void* base = mmap(nullptr, static_cast<std::size_t>(mapped_bytes), PROT_READ, MAP_PRIVATE, descriptor,
                    static_cast<off_t>(aligned_offset));
  if (base == MAP_FAILED) {
    const auto code = errno;
    ::close(descriptor);
    return data_error(core::ErrorReason::unavailable, "Cannot map requested read-only window", std::to_string(code));
  }
  result.lease_ =
      std::shared_ptr<const void>{base, [mapped_bytes, descriptor](const void* pointer) {
                                    munmap(const_cast<void*>(pointer), static_cast<std::size_t>(mapped_bytes));
                                    ::close(descriptor);
                                  }};
  result.data_ = static_cast<const std::byte*>(base) + static_cast<std::size_t>(delta);
#endif
  result.size_ = static_cast<std::size_t>(byte_count);
  return result;
}

std::span<const std::byte> MappedFileWindow::bytes() const noexcept {
  return {data_, size_};
}

} // namespace signal::data
