#include "domain/scn_detection_postprocess.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace signalstudio {
namespace {
bool fail(std::string& error, const char* message) {
  error = message;
  return false;
}
bool stop(const PostprocessCancellation& cancelled, std::string& error) {
  if (!cancelled || !cancelled()) return false;
  error = "scn-postprocess-cancelled";
  return true;
}
std::vector<DecodedCandidate> fuse(std::vector<DecodedCandidate> input) {
  std::sort(input.begin(), input.end(), [](const auto& a, const auto& b) {
    if (a.lowerBin != b.lowerBin) return a.lowerBin < b.lowerBin;
    if (a.upperBin != b.upperBin) return a.upperBin < b.upperBin;
    return a.peakIndex < b.peakIndex;
  });
  std::vector<DecodedCandidate> result;
  for (const auto& value : input) {
    if (result.empty()) {
      result.push_back(value);
      continue;
    }
    auto& previous = result.back();
    const double overlap =
        std::max(0.0, std::min(previous.upperBin, value.upperBin) -
                          std::max(previous.lowerBin, value.lowerBin));
    const double width = previous.upperBin - previous.lowerBin;
    const double otherWidth = value.upperBin - value.lowerBin;
    const double unionWidth = width + otherWidth - overlap;
    if (overlap / unionWidth < .1 &&
        overlap / std::min(width, otherWidth) < .5) {
      result.push_back(value);
      continue;
    }
    previous.lowerBin = std::min(previous.lowerBin, value.lowerBin);
    previous.upperBin = std::max(previous.upperBin, value.upperBin);
    if (value.score > previous.score) previous.peakIndex = value.peakIndex;
    previous.score = std::max(previous.score, value.score);
  }
  return result;
}
}  // namespace

bool postprocessScnWindow(std::span<const float> values, std::size_t frames,
                          std::size_t bins,
                          std::span<const DecodedCandidate> candidates,
                          double firstBinHz, double binWidthHz,
                          ScnWindowPostprocess& target, std::string& error,
                          const PostprocessCancellation& cancelled,
                          std::size_t modelPeakCount,
                          DetectionSpectrumMode spectrumMode) {
  const auto peakLimit = modelPeakCount ? modelPeakCount : bins / 4;
  if (!frames || bins < 4 || bins > 262144 || values.size() != frames * bins ||
      candidates.size() > 512 || !std::isfinite(firstBinHz) ||
      !std::isfinite(binWidthHz) || binWidthHz <= 0)
    return fail(error, "invalid-scn-window-shape");
  std::vector<float> selected;
  if (spectrumMode != DetectionSpectrumMode::ModelDefault) {
    if (stop(cancelled, error) ||
        !selectDetectionSpectrum(values, bins, spectrumMode, selected, error))
      return false;
    values = selected;
    frames = 1;
  }
  std::vector<float> maximum(bins, -std::numeric_limits<float>::infinity());
  std::vector<float> average(bins, 0);
  std::vector<double> sums(bins, 0), prefix(bins + 1, 0);
  for (std::size_t row = 0; row < frames; ++row) {
    if (stop(cancelled, error)) return false;
    for (std::size_t bin = 0; bin < bins; ++bin) {
      const auto value = values[row * bins + bin];
      if (!std::isfinite(value))
        return fail(error, "nonfinite-scn-physical-spectrum");
      maximum[bin] = std::max(maximum[bin], value);
      sums[bin] += value;
    }
  }
  for (std::size_t bin = 0; bin < bins; ++bin) {
    average[bin] = static_cast<float>(sums[bin] / frames);
    prefix[bin + 1] = prefix[bin] + maximum[bin];
  }
  ScnWindowPostprocess output;
  std::vector<DecodedCandidate> all, retained;
  for (const auto& candidate : candidates) {
    if (stop(cancelled, error)) return false;
    if (!std::isfinite(candidate.lowerBin) ||
        !std::isfinite(candidate.upperBin) || candidate.lowerBin < 0 ||
        candidate.upperBin >= bins ||
        candidate.upperBin <= candidate.lowerBin ||
        !std::isfinite(candidate.score) || candidate.score < 0 ||
        candidate.score > 1 || candidate.peakIndex >= peakLimit)
      return fail(error, "invalid-scn-raw-candidate");
    const auto begin = static_cast<std::size_t>(candidate.lowerBin);
    const auto end = static_cast<std::size_t>(candidate.upperBin);
    if (end <= begin) continue;
    const auto width = end - begin;
    const auto trim = static_cast<std::size_t>(std::round(width / 5.0));
    const auto signalBegin = std::min(end - 1, begin + trim);
    const auto signalEnd = std::max(signalBegin + 1, end - trim);
    const double signal = (prefix[signalEnd] - prefix[signalBegin]) /
                          static_cast<double>(signalEnd - signalBegin);
    const auto neighborWidth = std::max<std::size_t>(1, width / 10);
    const auto left = begin - std::min(begin, neighborWidth);
    const auto right = end + std::min(bins - end, neighborWidth);
    const auto count = begin - left + right - end;
    const double noise =
        count
            ? ((prefix[begin] - prefix[left]) + (prefix[right] - prefix[end])) /
                  count
            : signal;
    const double cnr = signal - noise;
    if (!std::isfinite(cnr)) return fail(error, "nonfinite-scn-cnr");
    output.measurements.push_back({candidate, signal, noise, cnr, cnr >= 3});
    auto measured = candidate;
    measured.lowerBin = static_cast<double>(begin);
    measured.upperBin = static_cast<double>(end);
    all.push_back(measured);
    if (cnr >= 3) retained.push_back(measured);
  }
  output.unfilteredFused = fuse(std::move(all));
  output.filteredFused = fuse(std::move(retained));
  std::vector<FrequencyRange> occupancy, filtered;
  const auto physical = [&](const auto& candidate) {
    return FrequencyRange{firstBinHz + candidate.lowerBin * binWidthHz,
                          firstBinHz + candidate.upperBin * binWidthHz};
  };
  for (const auto& candidate : output.unfilteredFused)
    occupancy.push_back(physical(candidate));
  for (const auto& candidate : output.filteredFused)
    filtered.push_back(physical(candidate));
  if (!refineScnFrequencyBands(filtered, occupancy, average, maximum,
                               firstBinHz, binWidthHz, 3, output.refinements,
                               error, cancelled))
    return false;
  for (std::size_t index = 0; index < output.filteredFused.size(); ++index) {
    auto candidate = output.filteredFused[index];
    const auto& refined = output.refinements[index].frequency;
    candidate.lowerBin = (refined.lowerHz - firstBinHz) / binWidthHz;
    candidate.upperBin = (refined.upperHz - firstBinHz) / binWidthHz;
    if (candidate.lowerBin < candidate.upperBin)
      output.candidates.push_back(candidate);
  }
  std::stable_sort(
      output.candidates.begin(), output.candidates.end(),
      [](const auto& a, const auto& b) { return a.score > b.score; });
  if (output.candidates.size() > 150) output.candidates.resize(150);
  if (stop(cancelled, error)) return false;
  target = std::move(output);
  error.clear();
  return true;
}
}  // namespace signalstudio
