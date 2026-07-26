#pragma once

#include "signal_studio/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace signal::data {

class SampleRange final {
public:
  SampleRange() noexcept = default;
  [[nodiscard]] static core::Result<SampleRange> make(std::uint64_t begin, std::uint64_t end);
  [[nodiscard]] static core::Result<SampleRange> from_count(std::uint64_t begin, std::uint64_t count);
  [[nodiscard]] std::uint64_t begin() const noexcept {
    return begin_;
  }
  [[nodiscard]] std::uint64_t end() const noexcept {
    return end_;
  }
  [[nodiscard]] std::uint64_t size() const noexcept {
    return end_ - begin_;
  }
  [[nodiscard]] bool empty() const noexcept {
    return begin_ == end_;
  }
  [[nodiscard]] bool contains(std::uint64_t sample) const noexcept;
  [[nodiscard]] bool contains(const SampleRange& other) const noexcept;
  friend bool operator==(const SampleRange&, const SampleRange&) = default;

private:
  SampleRange(std::uint64_t begin, std::uint64_t end) noexcept : begin_(begin), end_(end) {}
  std::uint64_t begin_{};
  std::uint64_t end_{};
};

struct ComplexSample final {
  double real{};
  double imag{};
  friend bool operator==(const ComplexSample&, const ComplexSample&) = default;
};

enum class SignalKind : std::uint8_t { real, complex };
enum class ScalarType : std::uint8_t { int8, uint8, int16, uint16, int24_packed, int32, float32, float64 };
enum class Endianness : std::uint8_t { not_applicable, little, big };
enum class ComponentLayout : std::uint8_t { real, interleaved, planar };
enum class ComponentOrder : std::uint8_t { not_applicable, iq, qi };
enum class FrequencyReference : std::uint8_t { baseband, absolute };
enum class SourceFormat : std::uint8_t { raw, wav };
enum class FieldOrigin : std::uint8_t { user, sidecar, wav_header, adapter, filename_hint };

struct FieldProvenance final {
  FieldOrigin origin{FieldOrigin::user};
  bool confirmed{};
  friend bool operator==(const FieldProvenance&, const FieldProvenance&) = default;
};

struct SignalDescriptor final {
  std::string schema{"signal.raw-descriptor/1.0"};
  SignalKind signal_kind{SignalKind::real};
  ScalarType scalar_type{ScalarType::float32};
  ComponentLayout component_layout{ComponentLayout::real};
  ComponentOrder component_order{ComponentOrder::not_applicable};
  Endianness endianness{Endianness::little};
  double sample_rate_hz{};
  std::optional<double> center_frequency_hz;
  std::uint64_t byte_offset{};
  SampleRange requested_sample_range{};
  std::string amplitude_mode{"linear"};
  double scale_factor{1.0};
  double additive_offset{};
  double start_time_seconds{};
  std::map<std::string, FieldProvenance> provenance;

  [[nodiscard]] core::Status validate() const;
  [[nodiscard]] core::Result<std::uint64_t> scalar_bytes() const;
  [[nodiscard]] core::Result<std::uint64_t> frame_bytes() const;
};

struct DataFacts final {
  std::uint64_t file_bytes{};
  std::uint64_t remaining_bytes{};
  std::uint64_t frame_bytes{};
  std::uint64_t available_frames{};
  double duration_seconds{};
  std::uint64_t initial_target_bytes{};
};

[[nodiscard]] core::Result<DataFacts> calculate_data_facts(std::uint64_t file_bytes, const SignalDescriptor& descriptor,
                                                           std::uint64_t configured_initial_bytes);
[[nodiscard]] core::Result<double> display_frequency(double baseband_frequency_hz, const SignalDescriptor& descriptor,
                                                     FrequencyReference reference);

class SignalSlice final {
public:
  SignalSlice() = default;
  [[nodiscard]] SignalKind kind() const noexcept {
    return kind_;
  }
  [[nodiscard]] std::uint64_t size() const noexcept {
    return size_;
  }
  [[nodiscard]] std::span<const double> real_values() const noexcept;
  [[nodiscard]] std::span<const ComplexSample> complex_values() const noexcept;
  [[nodiscard]] core::Result<SignalSlice> slice(std::uint64_t offset, std::uint64_t count) const;

private:
  friend class SignalBuffer;
  SignalKind kind_{SignalKind::real};
  std::shared_ptr<const std::vector<double>> real_owner_;
  std::shared_ptr<const std::vector<ComplexSample>> complex_owner_;
  std::uint64_t offset_{};
  std::uint64_t size_{};
};

class SignalBuffer final {
public:
  [[nodiscard]] static SignalBuffer from_real(std::vector<double> values);
  [[nodiscard]] static SignalBuffer from_complex(std::vector<ComplexSample> values);
  [[nodiscard]] SignalKind kind() const noexcept {
    return kind_;
  }
  [[nodiscard]] std::uint64_t size() const noexcept;
  [[nodiscard]] SignalSlice view() const noexcept;

private:
  SignalKind kind_{SignalKind::real};
  std::shared_ptr<const std::vector<double>> real_;
  std::shared_ptr<const std::vector<ComplexSample>> complex_;
};

struct RawReadResult final {
  SignalBuffer samples;
  SampleRange range;
  std::uint64_t bytes_read{};
};

struct ReadRequest final {
  SampleRange range;
  std::uint64_t maximum_read_bytes{};
  std::function<bool()> cancellation_requested;
};

class IDataSource {
public:
  virtual ~IDataSource() = default;
  [[nodiscard]] virtual const SignalDescriptor& descriptor() const noexcept = 0;
  [[nodiscard]] virtual std::string_view data_source_version_id() const noexcept = 0;
  [[nodiscard]] virtual SourceFormat source_format() const noexcept = 0;
  /// 执行一次显式字节上界内的低延迟读取；预计超过 50 ms 的循环必须交由 ITaskService 调度。
  [[nodiscard]] virtual core::Result<RawReadResult> read(const ReadRequest& request) const = 0;
};

class FileDataSource final : public IDataSource {
public:
  [[nodiscard]] static core::Result<std::shared_ptr<FileDataSource>>
  open_raw(std::filesystem::path path, SignalDescriptor descriptor, std::string data_source_version_id);
  [[nodiscard]] static core::Result<std::shared_ptr<FileDataSource>>
  open_wav(std::filesystem::path path, std::string data_source_version_id, bool confirm_stereo_iq,
           ComponentOrder confirmed_order = ComponentOrder::iq);
  [[nodiscard]] const SignalDescriptor& descriptor() const noexcept override {
    return descriptor_;
  }
  [[nodiscard]] std::string_view data_source_version_id() const noexcept override {
    return data_source_version_id_;
  }
  [[nodiscard]] SourceFormat source_format() const noexcept override {
    return source_format_;
  }
  [[nodiscard]] core::Result<RawReadResult> read(const ReadRequest& request) const override;

private:
  FileDataSource(std::filesystem::path path, SignalDescriptor descriptor, std::string data_source_version_id,
                 SourceFormat source_format);
  std::filesystem::path path_;
  SignalDescriptor descriptor_;
  std::string data_source_version_id_;
  SourceFormat source_format_{SourceFormat::raw};
};

struct SignalSelection final {
  SampleRange source_range;
  SignalSlice samples;
  SignalDescriptor source_descriptor;
  SourceFormat source_format{SourceFormat::raw};
  std::string data_source_version_id;
};

[[nodiscard]] core::Result<SignalSelection> select_samples(const RawReadResult& browsed, const SampleRange& selection,
                                                           const IDataSource& source);

struct ExportReceipt final {
  std::filesystem::path path;
  SampleRange source_range;
  SignalDescriptor exported_descriptor;
  SourceFormat format{SourceFormat::raw};
  std::uint64_t bytes_written{};
};

/// 执行显式字节上界内的导出；预计超过 50 ms 的导出必须在 ITaskService 任务处理器中调用。
[[nodiscard]] core::Result<ExportReceipt> export_selection(const SignalSelection& selection,
                                                           const std::filesystem::path& destination,
                                                           std::uint64_t maximum_export_bytes);

[[nodiscard]] core::Result<RawReadResult> read_raw_samples(const std::filesystem::path& path,
                                                           const SignalDescriptor& descriptor, const SampleRange& range,
                                                           std::uint64_t maximum_read_bytes);

struct WavDescriptor final {
  SignalDescriptor descriptor;
  std::uint64_t data_bytes{};
};

[[nodiscard]] core::Result<WavDescriptor> read_wav_descriptor(const std::filesystem::path& path, bool confirm_stereo_iq,
                                                              ComponentOrder confirmed_order = ComponentOrder::iq);

[[nodiscard]] core::Result<std::string> serialize_sidecar(const SignalDescriptor& descriptor);
[[nodiscard]] core::Result<SignalDescriptor> parse_sidecar(std::string_view json);

struct AdapterDescriptor final {
  SignalDescriptor descriptor;
  std::string adapter_id;
  bool descriptor_only{true};
};

class IFormatAdapter {
public:
  virtual ~IFormatAdapter() = default;
  [[nodiscard]] virtual std::string_view id() const noexcept = 0;
  [[nodiscard]] virtual bool probe(const std::filesystem::path& path) const = 0;
  [[nodiscard]] virtual core::Result<AdapterDescriptor> describe(const std::filesystem::path& path) const = 0;
};

using DescriptorFactory = std::function<core::Result<SignalDescriptor>(const std::filesystem::path&)>;

class DescriptorFormatAdapter final : public IFormatAdapter {
public:
  DescriptorFormatAdapter(std::string id, std::vector<std::string> extensions, DescriptorFactory factory);
  [[nodiscard]] std::string_view id() const noexcept override;
  [[nodiscard]] bool probe(const std::filesystem::path& path) const override;
  [[nodiscard]] core::Result<AdapterDescriptor> describe(const std::filesystem::path& path) const override;

private:
  std::string id_;
  std::vector<std::string> extensions_;
  DescriptorFactory factory_;
};

class FormatAdapterRegistry final {
public:
  [[nodiscard]] core::Status add(std::shared_ptr<IFormatAdapter> adapter);
  [[nodiscard]] std::shared_ptr<const IFormatAdapter> find(std::string_view id) const;
  [[nodiscard]] core::Result<AdapterDescriptor> describe(const std::filesystem::path& path) const;
  [[nodiscard]] std::vector<std::string> adapter_ids() const;

private:
  std::map<std::string, std::shared_ptr<IFormatAdapter>, std::less<>> adapters_;
};

[[nodiscard]] core::Status register_standard_descriptor_adapters(FormatAdapterRegistry& registry,
                                                                 DescriptorFactory mat_factory,
                                                                 DescriptorFactory numpy_factory,
                                                                 DescriptorFactory vendor_factory);

} // namespace signal::data
