#pragma once

#include "signal_studio/data/signal.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace signal::dsp {

enum class NodeKind : std::uint8_t {
  remove_dc,
  gain,
  iq_correction,
  frequency_shift,
  fir_filter,
  iir_filter,
  resample,
};
enum class FilterShape : std::uint8_t { lowpass, highpass, bandpass, bandstop, custom };
enum class BoundaryPolicy : std::uint8_t { zero_pad, preserve_state };
enum class RealToComplexMode : std::uint8_t { forbidden, analytic_signal, quadrature_mixer };

struct NodeContract final {
  bool accepts_real{true};
  bool accepts_complex{true};
  bool produces_complex{};
  double sample_rate_numerator{1.0};
  double sample_rate_denominator{1.0};
  std::string input_unit{"linear"};
  std::string output_unit{"linear"};
};

struct NodeSpec final {
  std::string id;
  NodeKind kind{NodeKind::gain};
  std::string implementation_id{"signal.dsp.builtin/1.0"};
  bool enabled{true};
  NodeContract contract;
  FilterShape filter_shape{FilterShape::custom};
  RealToComplexMode real_to_complex{RealToComplexMode::forbidden};
  double gain{1.0};
  double additive_offset{};
  double iq_gain_balance{1.0};
  double iq_phase_radians{};
  double frequency_shift_hz{};
  std::vector<double> numerator;
  std::vector<double> denominator;
  std::uint32_t resample_numerator{1};
  std::uint32_t resample_denominator{1};
  double anti_alias_cutoff_hz{};
  double anti_alias_stopband_db{};
  std::map<std::string, double, std::less<>> parameters;
};

struct ChainSnapshot final {
  std::string schema{"signal-processing-chain/1.0"};
  std::string version{"1.0.0"};
  std::vector<NodeSpec> nodes;
};

struct ChannelSnapshot final {
  std::string channel_id;
  data::SampleRange input_selection;
  std::string data_source_version_id;
  data::SignalDescriptor output_descriptor;
  ChainSnapshot processing_chain;
};

class ProcessingChain final {
public:
  [[nodiscard]] core::Status append(NodeSpec node);
  [[nodiscard]] core::Status set_enabled(std::string_view id, bool enabled);
  [[nodiscard]] core::Status move(std::string_view id, std::size_t destination);
  [[nodiscard]] core::Status duplicate(std::string_view id, std::string duplicate_id);
  [[nodiscard]] core::Status erase(std::string_view id);
  [[nodiscard]] core::Status apply_preset(std::string_view id, std::map<std::string, double, std::less<>> values);
  [[nodiscard]] ChainSnapshot snapshot() const;
  [[nodiscard]] std::vector<std::string> invalidate_downstream(std::string_view changed_id) const;

private:
  std::vector<NodeSpec> nodes_;
};

struct FilterState final {
  std::vector<data::ComplexSample> input_history;
  std::vector<data::ComplexSample> output_history;
  double oscillator_phase_radians{};
  std::uint64_t resample_phase{};
  std::uint64_t processed_samples{};
  std::shared_ptr<void> backend_state;
};

struct FilterCoefficients final {
  std::vector<double> numerator;
  std::vector<double> denominator;
};

struct ResampleRatio final {
  std::uint32_t numerator{1};
  std::uint32_t denominator{1};
};

class IFilter {
public:
  virtual ~IFilter() = default;
  [[nodiscard]] virtual core::Result<std::vector<data::ComplexSample>>
  process(const NodeSpec& specification, double sample_rate_hz, std::span<const data::ComplexSample> input,
          FilterState& state, BoundaryPolicy boundary) = 0;
};

class IResampler {
public:
  virtual ~IResampler() = default;
  [[nodiscard]] virtual core::Result<std::vector<data::ComplexSample>>
  process(ResampleRatio ratio, std::span<const data::ComplexSample> input,
          std::span<const double> anti_alias_coefficients, FilterState& state, bool end_of_input = true) = 0;
};

class ISignalKernelBackend {
public:
  virtual ~ISignalKernelBackend() = default;
  [[nodiscard]] virtual std::string_view backend_id() const noexcept = 0;
  [[nodiscard]] virtual core::Result<std::vector<data::ComplexSample>>
  analytic_signal(std::span<const double> input) = 0;
  [[nodiscard]] virtual core::Result<std::vector<data::ComplexSample>>
  convolve(std::span<const data::ComplexSample> input, std::span<const double> coefficients, FilterState& state,
           BoundaryPolicy boundary) = 0;
  [[nodiscard]] virtual core::Result<std::vector<data::ComplexSample>>
  solve_iir(std::span<const data::ComplexSample> input, std::span<const double> numerator,
            std::span<const double> denominator, FilterState& state, BoundaryPolicy boundary) = 0;
  [[nodiscard]] virtual core::Result<std::vector<data::ComplexSample>>
  resample(std::span<const data::ComplexSample> input, std::uint32_t numerator, std::uint32_t denominator,
           std::span<const double> anti_alias_coefficients, FilterState& state, bool end_of_input = true) = 0;
};

[[nodiscard]] core::Result<std::shared_ptr<ISignalKernelBackend>> make_cpu_signal_kernel_backend();
[[nodiscard]] core::Result<std::shared_ptr<ISignalKernelBackend>> make_auto_signal_kernel_backend();
[[nodiscard]] core::Result<std::shared_ptr<IFilter>> make_filter(std::shared_ptr<ISignalKernelBackend> backend);
[[nodiscard]] core::Result<std::shared_ptr<IResampler>> make_resampler(std::shared_ptr<ISignalKernelBackend> backend);
[[nodiscard]] core::Result<FilterCoefficients> resolve_filter_coefficients(const NodeSpec& node, double sample_rate_hz);
[[nodiscard]] core::Status validate_anti_alias_filter(std::span<const double> coefficients, double input_sample_rate_hz,
                                                      ResampleRatio ratio, double cutoff_hz,
                                                      double required_stopband_db);

struct ProcessRequest final {
  data::SignalSlice samples;
  data::SignalDescriptor descriptor;
  ChainSnapshot chain;
  BoundaryPolicy boundary{BoundaryPolicy::preserve_state};
  std::shared_ptr<const std::atomic_bool> cancellation;
};

struct ProcessResult final {
  data::SignalBuffer samples;
  data::SignalDescriptor descriptor;
  std::vector<FilterState> states;
  std::vector<std::string> applied_node_ids;
  std::string backend_id;
};

[[nodiscard]] core::Status validate_node(const NodeSpec& node, const data::SignalDescriptor& input);
[[nodiscard]] core::Result<ProcessResult> process_chain(ISignalKernelBackend& backend, const ProcessRequest& request,
                                                        std::span<const FilterState> initial_states);

struct NodePreview final {
  data::SignalBuffer before;
  data::SignalBuffer after;
  std::vector<double> response_frequency_hz;
  std::vector<double> response_magnitude_db;
  double group_delay_samples{};
};

[[nodiscard]] core::Result<NodePreview> preview_node(ISignalKernelBackend& backend, const data::SignalSlice& samples,
                                                     const data::SignalDescriptor& descriptor, const NodeSpec& node);

struct SignalSummary final {
  data::SignalKind kind{data::SignalKind::real};
  std::uint64_t sample_count{};
  double sample_rate_hz{};
  std::string unit{"linear"};
};

class ProcessingProvenance final {
public:
  [[nodiscard]] std::string_view source_fingerprint() const noexcept;
  [[nodiscard]] const data::SampleRange& source_range() const noexcept;
  [[nodiscard]] std::string_view data_source_version_id() const noexcept;
  [[nodiscard]] const ChainSnapshot& chain() const noexcept;
  [[nodiscard]] std::string_view backend_id() const noexcept;
  [[nodiscard]] const SignalSummary& input_summary() const noexcept;
  [[nodiscard]] const SignalSummary& output_summary() const noexcept;

private:
  friend core::Result<ProcessingProvenance> make_processing_provenance(std::string, data::SampleRange, std::string,
                                                                       const ChainSnapshot&, std::string, SignalSummary,
                                                                       SignalSummary);

  ProcessingProvenance(std::string source_fingerprint, data::SampleRange source_range,
                       std::string data_source_version_id, ChainSnapshot chain, std::string backend_id,
                       SignalSummary input_summary, SignalSummary output_summary);

  std::string source_fingerprint_;
  data::SampleRange source_range_;
  std::string data_source_version_id_;
  ChainSnapshot chain_;
  std::string backend_id_;
  SignalSummary input_summary_;
  SignalSummary output_summary_;
};

[[nodiscard]] core::Result<ProcessingProvenance>
make_processing_provenance(std::string source_fingerprint, data::SampleRange source_range,
                           std::string data_source_version_id, const ChainSnapshot& chain, std::string backend_id,
                           SignalSummary input_summary, SignalSummary output_summary);
[[nodiscard]] core::Result<std::string> serialize_processing_provenance(const ProcessingProvenance& provenance);

class ProcessingCache final {
public:
  [[nodiscard]] core::Status put(std::string node_id, data::SignalBuffer output);
  [[nodiscard]] bool contains(std::string_view node_id) const;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::vector<std::string> invalidate_downstream(const ChainSnapshot& chain, std::string_view changed_id);

private:
  std::map<std::string, data::SignalBuffer, std::less<>> entries_;
};

[[nodiscard]] core::Result<std::string> export_chain_template(const ChainSnapshot& chain);
[[nodiscard]] core::Result<ChainSnapshot> import_chain_template(std::string_view text,
                                                                std::span<const std::string> available_implementations);
[[nodiscard]] core::Result<std::vector<std::byte>> export_bit_exact_bypass(std::span<const std::byte> original_bytes,
                                                                           std::uint64_t frame_bytes,
                                                                           const data::SampleRange& range);

} // namespace signal::dsp
