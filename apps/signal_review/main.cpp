// Signal Review: a second thin application built from public platform modules only, proving the
// ten-module platform is reusable independent of the Signal Studio GUI app. Headless CLI: import
// a WAV or SC16 file, compute bounded-window statistics, emit a JSON review report.
//
// Links only: SignalStudio::Data SignalStudio::DSP SignalStudio::Compute SignalStudio::Core
// No Qt, no private headers, no apps/signal_studio dependency.

#include "signal_studio/core/result.hpp"
#include "signal_studio/core/version.hpp"
#include "signal_studio/data/signal.hpp"
#include "signal_studio/dsp/statistics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <regex>
#include <string>

namespace {

using signal::core::Result;
using signal::core::Status;
using signal::data::FileDataSource;
using signal::data::ReadRequest;
using signal::data::SampleRange;
using signal::data::SignalBuffer;
using signal::data::SignalDescriptor;
using signal::data::SignalKind;
using signal::data::SourceFormat;

struct ReviewOutcome {
  std::string path;
  std::string format;
  double sample_rate_hz{};
  std::uint64_t total_samples{};
  std::uint64_t reviewed_samples{};
  bool complex{};
  double mean{};
  double rms{};
  double peak{};
  double magnitude_mean{};
  std::string error;
};

std::string json_escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  return out;
}

void emit_json(const ReviewOutcome& o) {
  std::printf("{");
  std::printf("\"path\":\"%s\"", json_escape(o.path).c_str());
  std::printf(",\"format\":\"%s\"", json_escape(o.format).c_str());
  std::printf(",\"sampleRateHz\":%.6f", o.sample_rate_hz);
  std::printf(",\"totalSamples\":%llu", static_cast<unsigned long long>(o.total_samples));
  std::printf(",\"reviewedSamples\":%llu", static_cast<unsigned long long>(o.reviewed_samples));
  std::printf(",\"complex\":%s", o.complex ? "true" : "false");
  std::printf(",\"mean\":%.9f", o.mean);
  std::printf(",\"rms\":%.9f", o.rms);
  std::printf(",\"peak\":%.9f", o.peak);
  std::printf(",\"magnitudeMean\":%.9f", o.magnitude_mean);
  if (!o.error.empty()) {
    std::printf(",\"error\":\"%s\"", json_escape(o.error).c_str());
  }
  std::printf("}\n");
}

// Minimal filename hint parser (independent of signal_studio app, proving reuse).
struct FilenameHint {
  double cf_hz{};
  double sr_hz{};
  bool has_cf{};
  bool has_sr{};
};
FilenameHint parse_filename(const std::filesystem::path& p) {
  FilenameHint h;
  const std::string n = p.filename().string();
  std::smatch m;
  static const std::regex cf_re("cf([0-9]+(?:\\.[0-9]+)?)(GHz|MHz|kHz|Hz)");
  if (std::regex_search(n, m, cf_re)) {
    double scale = (m[2] == "GHz") ? 1e9 : (m[2] == "MHz") ? 1e6 : (m[2] == "kHz") ? 1e3 : 1.0;
    h.cf_hz = std::stod(m[1].str()) * scale;
    h.has_cf = true;
  }
  static const std::regex sr_re("sr([0-9]+(?:\\.[0-9]+)?)(GSps|MSps|kSps|Sps)");
  if (std::regex_search(n, m, sr_re)) {
    double scale = (m[2] == "GSps") ? 1e9 : (m[2] == "MSps") ? 1e6 : (m[2] == "kSps") ? 1e3 : 1.0;
    h.sr_hz = std::stod(m[1].str()) * scale;
    h.has_sr = true;
  }
  return h;
}

Result<ReviewOutcome> review_wav(const std::filesystem::path& path) {
  auto wd = signal::data::read_wav_descriptor(path, true);
  if (!wd.ok())
    return Status(wd.error());
  auto source = FileDataSource::open_wav(path, "review:" + path.filename().string(), true);
  if (!source.ok())
    return Status(source.error());
  ReviewOutcome o;
  o.path = path.generic_string();
  o.format = "wav";
  o.sample_rate_hz = wd->descriptor.sample_rate_hz;
  o.complex = (wd->descriptor.signal_kind == SignalKind::complex);
  std::error_code ec;
  const std::uint64_t file_bytes = std::filesystem::file_size(path, ec);
  if (!ec && wd->descriptor.frame_bytes().ok() && *wd->descriptor.frame_bytes() > 0) {
    const std::uint64_t usable =
        wd->data_bytes > 0 ? wd->data_bytes
                           : (file_bytes > wd->descriptor.byte_offset ? file_bytes - wd->descriptor.byte_offset : 0);
    o.total_samples = usable / *wd->descriptor.frame_bytes();
  }
  const std::uint64_t count = std::min<std::uint64_t>(o.total_samples, 8192);
  o.reviewed_samples = count;
  auto range = SampleRange::from_count(0, count);
  if (!range.ok())
    return Status(range.error());
  ReadRequest req;
  req.range = *range;
  req.maximum_read_bytes = 64 * 1024 * 1024;
  auto r = (*source)->read(req);
  if (!r.ok())
    return Status(r.error());
  auto slice = r->samples.view();
  if (o.complex) {
    auto st = signal::dsp::compute_complex_stats(slice);
    if (!st.ok())
      return Status(st.error());
    o.magnitude_mean = st->magnitude_mean;
    o.peak = st->magnitude_peak;
    o.rms = st->magnitude_rms;
  } else {
    auto st = signal::dsp::compute_real_stats(slice);
    if (!st.ok())
      return Status(st.error());
    o.mean = st->mean;
    o.rms = st->rms;
    o.peak = st->peak;
  }
  return o;
}

Result<ReviewOutcome> review_sc16(const std::filesystem::path& path) {
  const auto hint = parse_filename(path);
  if (!hint.has_sr) {
    return Status::failure(
        signal::core::ErrorCode{signal::core::ErrorDomain::data, signal::core::ErrorReason::invalid_argument},
        "SC16 review requires sample rate in filename (sr..Sps) or use --sr");
  }
  SignalDescriptor d;
  d.signal_kind = SignalKind::complex;
  d.scalar_type = signal::data::ScalarType::int16;
  d.component_layout = signal::data::ComponentLayout::interleaved;
  d.component_order = signal::data::ComponentOrder::iq;
  d.endianness = signal::data::Endianness::little;
  d.sample_rate_hz = hint.sr_hz;
  if (hint.has_cf)
    d.center_frequency_hz = hint.cf_hz;
  auto validate = d.validate();
  if (!validate.ok())
    return validate;
  std::error_code ec;
  const std::uint64_t file_bytes = std::filesystem::file_size(path, ec);
  if (ec) {
    return Status::failure(
        signal::core::ErrorCode{signal::core::ErrorDomain::data, signal::core::ErrorReason::invalid_argument},
        "cannot size SC16 source");
  }
  auto facts = signal::data::calculate_data_facts(file_bytes, d, 1U);
  if (!facts.ok())
    return Status(facts.error());
  auto range = SampleRange::from_count(0, facts->available_frames);
  if (!range.ok())
    return Status(range.error());
  d.requested_sample_range = *range;
  auto source = FileDataSource::open_raw(path, d, "review:" + path.filename().string());
  if (!source.ok())
    return Status(source.error());
  ReviewOutcome o;
  o.path = path.generic_string();
  o.format = "sc16";
  o.sample_rate_hz = d.sample_rate_hz;
  o.complex = true;
  o.total_samples = facts->available_frames;
  const std::uint64_t count = std::min<std::uint64_t>(o.total_samples, 8192);
  o.reviewed_samples = count;
  auto count_range = SampleRange::from_count(0, count);
  if (!count_range.ok())
    return Status(count_range.error());
  ReadRequest req;
  req.range = *count_range;
  req.maximum_read_bytes = 64 * 1024 * 1024;
  auto r = (*source)->read(req);
  if (!r.ok())
    return Status(r.error());
  auto st = signal::dsp::compute_complex_stats(r->samples.view());
  if (!st.ok())
    return Status(st.error());
  o.magnitude_mean = st->magnitude_mean;
  o.peak = st->magnitude_peak;
  o.rms = st->magnitude_rms;
  return o;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: signal_review <file> [--version]\n");
    return 2;
  }
  if (std::string_view{argv[1]} == "--version") {
    const auto& bi = signal::core::build_info();
    std::printf("signal_review %u.%u.%u (platform %.*s)\n", bi.version.major, bi.version.minor, bi.version.patch,
                static_cast<int>(bi.product.size()), bi.product.data());
    return 0;
  }
  const std::filesystem::path path(argv[1]);
  if (!std::filesystem::exists(path)) {
    ReviewOutcome o;
    o.path = path.generic_string();
    o.error = "file not found";
    emit_json(o);
    return 1;
  }
  const std::string ext = path.extension().string();
  Result<ReviewOutcome> result = (ext == ".wav")                     ? review_wav(path)
                                 : (ext == ".sc16" || ext == ".raw") ? review_sc16(path)
                                                                     : review_wav(path);
  if (!result.ok()) {
    ReviewOutcome o;
    o.path = path.generic_string();
    o.error = std::string(result.error().message());
    emit_json(o);
    return 1;
  }
  emit_json(*result);
  return 0;
}
