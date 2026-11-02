#include "domain/detection_refined_fusion.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <tuple>
#include <utility>

namespace signalstudio {
namespace {

constexpr std::size_t kMaximumEvents = 100000;
constexpr std::uint64_t kMaximumComparisons = 4000000;
constexpr std::uint64_t kMaximumWorkingBytes = 32 * 1024 * 1024;

struct FusionNode {
  ViewRange range;
  FrequencyQuality frequencyQuality = FrequencyQuality::ModelEstimated;
  SampleIndex uncertaintySamples = 0;
  std::size_t firstMember = 0;
  std::size_t lastMember = 0;
  std::size_t memberCount = 1;
  bool clippedAtRoi = false;
  bool active = true;
};

bool fail(std::string& error, const char* message) {
  error = message;
  return false;
}

bool cancelled(const DetectionRefinedFusionOptions& options) {
  return options.cancelled && options.cancelled();
}

bool addBytes(std::uint64_t amount, std::uint64_t budget,
              std::uint64_t& total) {
  if (amount > budget || total > budget - amount) return false;
  total += amount;
  return true;
}

bool validEvent(const DetectionRefinedEvent& event,
                std::size_t maximumIdBytes) {
  const auto& segment = event.segment;
  const auto frequency = segment.range.frequency;
  return !event.stableId.empty() && event.stableId.size() <= maximumIdBytes &&
         segment.id.size() <= maximumIdBytes &&
         segment.range.time.begin < segment.range.time.end &&
         std::isfinite(frequency.lowerHz) && std::isfinite(frequency.upperHz) &&
         frequency.lowerHz < frequency.upperHz &&
         std::isfinite(frequency.upperHz - frequency.lowerHz) &&
         segment.timeQuality == TimeQuality::AlgorithmEstimated &&
         (segment.frequencyQuality == FrequencyQuality::ModelEstimated ||
          segment.frequencyQuality == FrequencyQuality::Refined) &&
         (segment.frequencyFrame == FrequencyFrame::RF ||
          segment.frequencyFrame == FrequencyFrame::Baseband ||
          segment.frequencyFrame == FrequencyFrame::RealOneSided);
}

struct CarrierMatch {
  bool matched = false;
  double similarity = 0;
  double centerDistance = 0;
};

CarrierMatch sameCarrier(FrequencyRange a, FrequencyRange b,
                         const DetectionRefinedFusionOptions& options) {
  const double overlap =
      std::min(a.upperHz, b.upperHz) - std::max(a.lowerHz, b.lowerHz);
  if (overlap <= 0) return {};
  const double aWidth = a.upperHz - a.lowerHz;
  const double bWidth = b.upperHz - b.lowerHz;
  const double scale = std::max(aWidth, bWidth);
  const double overlapScaled = overlap / scale;
  const double iou =
      overlapScaled / (aWidth / scale + bWidth / scale - overlapScaled);
  const double narrowWidth = std::min(aWidth, bWidth);
  const double containment = overlap / narrowWidth;
  const double aCenter = a.lowerHz * .5 + a.upperHz * .5;
  const double bCenter = b.lowerHz * .5 + b.upperHz * .5;
  const double distance = std::abs(aCenter - bCenter);
  const double limit = narrowWidth * .5 + 2 * options.binWidthHz;
  return {iou >= options.matchIou ||
              (containment >= options.matchIou && distance <= limit),
          std::max(iou, containment), distance};
}

using PairKey =
    std::tuple<double, double, SampleIndex, std::size_t, std::size_t>;

}  // namespace

bool fuseRefinedDetectionEvents(std::span<const DetectionRefinedEvent> input,
                                const DetectionRefinedFusionOptions& options,
                                std::vector<DetectionRefinedFusion>& output,
                                DetectionRefinedFusionSummary& summary,
                                std::string& error) {
  if (!std::isfinite(options.matchIou) || options.matchIou < 0 ||
      options.matchIou > 1 || !std::isfinite(options.binWidthHz) ||
      options.binWidthHz <= 0 || !std::isfinite(2 * options.binWidthHz) ||
      !options.maximumInputEvents ||
      options.maximumInputEvents > kMaximumEvents || !options.maximumIdBytes ||
      options.maximumIdBytes > 256 || !options.maximumPairComparisons ||
      options.maximumPairComparisons > kMaximumComparisons ||
      !options.workingByteBudget ||
      options.workingByteBudget > kMaximumWorkingBytes ||
      input.size() > options.maximumInputEvents)
    return fail(error, "Invalid refined-fusion configuration or input limit");
  if (cancelled(options)) return fail(error, "Refined fusion cancelled");

  // Reserve for nodes, lineage links, identity/output ordering, and worst-case
  // atomic output. Caller-owned input is borrowed; existing output remains
  // resident until the final swap and is included in this transaction budget.
  // Include bounded stack/control storage and per-event allocator slack in
  // addition to array payloads and string contents.
  std::uint64_t workingBytes = 4096;
  const std::uint64_t bytesPerEvent = sizeof(FusionNode) +
                                      sizeof(DetectionRefinedFusion) +
                                      7 * sizeof(std::size_t);
  if (!addBytes(input.size() * bytesPerEvent, options.workingByteBudget,
                workingBytes) ||
      output.capacity() >
          options.workingByteBudget / sizeof(DetectionRefinedFusion) ||
      !addBytes(output.capacity() * sizeof(DetectionRefinedFusion),
                options.workingByteBudget, workingBytes))
    return fail(error, "Refined-fusion working-byte budget exceeded");
  for (const auto& existing : output)
    if (existing.memberEventIndices.capacity() >
            options.workingByteBudget / sizeof(std::size_t) ||
        !addBytes(existing.memberEventIndices.capacity() * sizeof(std::size_t),
                  options.workingByteBudget, workingBytes) ||
        !addBytes(existing.segment.id.capacity() + 1, options.workingByteBudget,
                  workingBytes))
      return fail(error, "Refined-fusion working-byte budget exceeded");

  const auto frame =
      input.empty() ? FrequencyFrame::RF : input.front().segment.frequencyFrame;
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (!(i % 512) && cancelled(options))
      return fail(error, "Refined fusion cancelled");
    if (!validEvent(input[i], options.maximumIdBytes) ||
        input[i].segment.frequencyFrame != frame)
      return fail(error, "Invalid refined event, quality, or frequency frame");
    if (!addBytes(2 * (std::max(input[i].stableId.size(),
                                input[i].segment.id.size()) +
                       1),
                  options.workingByteBudget, workingBytes))
      return fail(error, "Refined-fusion working-byte budget exceeded");
  }

  std::vector<std::size_t> order(input.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return input[a].stableId < input[b].stableId;
  });
  for (std::size_t i = 1; i < order.size(); ++i)
    if (input[order[i - 1]].stableId == input[order[i]].stableId)
      return fail(error, "Duplicate refined-event stable identity");
  if (cancelled(options)) return fail(error, "Refined fusion cancelled");

  std::vector<FusionNode> nodes(input.size());
  std::vector<std::size_t> nextMember(input.size(), input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    const auto& segment = input[i].segment;
    nodes[i] = {segment.range,
                segment.frequencyQuality,
                segment.uncertaintySamples,
                i,
                i,
                1,
                segment.clippedAtRoi,
                true};
  }
  DetectionRefinedFusionSummary tentativeSummary;
  tentativeSummary.inputEvents = input.size();
  tentativeSummary.estimatedWorkingBytes = workingBytes;
  while (true) {
    bool found = false;
    PairKey bestKey;
    std::size_t bestFirst = 0, bestSecond = 0;
    // Reuse the already-budgeted ordering vector. Rebuild after every merge
    // because an envelope may acquire neighbors that were previously disjoint.
    order.clear();
    for (std::size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].active) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      return std::tie(nodes[a].range.frequency.lowerHz, a) <
             std::tie(nodes[b].range.frequency.lowerHz, b);
    });
    for (std::size_t position = 0; position < order.size(); ++position) {
      if (!(position % 512) && cancelled(options))
        return fail(error, "Refined fusion cancelled");
      const auto left = order[position];
      for (std::size_t other = position + 1; other < order.size(); ++other) {
        const auto right = order[other];
        if (nodes[right].range.frequency.lowerHz >= nodes[left].range.frequency.upperHz)
          break;  // sameCarrier requires strictly positive frequency overlap.
        const auto i = std::min(left, right);
        const auto j = std::max(left, right);
        if (tentativeSummary.pairComparisons == options.maximumPairComparisons)
          return fail(error,
                      "Refined-fusion cumulative comparison budget exceeded");
        ++tentativeSummary.pairComparisons;
        if (!(tentativeSummary.pairComparisons % 512) && cancelled(options))
          return fail(error, "Refined fusion cancelled");
        const auto& a = nodes[i].range;
        const auto& b = nodes[j].range;
        if (std::max(a.time.begin, b.time.begin) >
            std::min(a.time.end, b.time.end))
          continue;
        const auto match = sameCarrier(a.frequency, b.frequency, options);
        if (!match.matched) continue;
        const PairKey key{-match.similarity, match.centerDistance,
                          std::min(a.time.begin, b.time.begin), i, j};
        if (!found || key < bestKey) {
          found = true;
          bestKey = key;
          bestFirst = i;
          bestSecond = j;
        }
      }
    }
    if (cancelled(options)) return fail(error, "Refined fusion cancelled");
    if (!found) break;
    auto& a = nodes[bestFirst];
    auto& b = nodes[bestSecond];
    const FrequencyRange frequency{
        std::min(a.range.frequency.lowerHz, b.range.frequency.lowerHz),
        std::max(a.range.frequency.upperHz, b.range.frequency.upperHz)};
    if (!std::isfinite(frequency.upperHz - frequency.lowerHz))
      return fail(error, "Fused frequency range overflow");
    a.range = {{std::min(a.range.time.begin, b.range.time.begin),
                std::max(a.range.time.end, b.range.time.end)},
               frequency};
    a.uncertaintySamples = std::max(a.uncertaintySamples, b.uncertaintySamples);
    if (a.frequencyQuality != b.frequencyQuality)
      a.frequencyQuality = FrequencyQuality::ModelEstimated;
    a.clippedAtRoi |= b.clippedAtRoi;
    nextMember[a.lastMember] = b.firstMember;
    a.lastMember = b.lastMember;
    a.memberCount += b.memberCount;
    b.active = false;
    ++tentativeSummary.mergedPairs;
  }

  order.clear();
  for (std::size_t i = 0; i < nodes.size(); ++i)
    if (nodes[i].active) order.push_back(i);
  std::sort(order.begin(), order.end(), [&](std::size_t i, std::size_t j) {
    const auto& a = nodes[i].range;
    const auto& b = nodes[j].range;
    return std::tie(a.frequency.lowerHz, a.time.begin, a.frequency.upperHz, i) <
           std::tie(b.frequency.lowerHz, b.time.begin, b.frequency.upperHz, j);
  });
  std::vector<DetectionRefinedFusion> tentative;
  tentative.reserve(input.size());
  for (const auto index : order) {
    if (cancelled(options)) return fail(error, "Refined fusion cancelled");
    const auto& node = nodes[index];
    DetectionRefinedFusion fused;
    fused.segment = input[node.firstMember].segment;
    fused.segment.id = input[node.firstMember].stableId;
    fused.segment.range = node.range;
    fused.segment.timeQuality = TimeQuality::AlgorithmEstimated;
    fused.segment.frequencyQuality = node.frequencyQuality;
    fused.segment.uncertaintySamples = node.uncertaintySamples;
    fused.segment.clippedAtRoi = node.clippedAtRoi;
    if (node.memberCount > 1) fused.segment.confirmed = false;
    fused.memberEventIndices.reserve(node.memberCount);
    for (auto member = node.firstMember; member != input.size();
         member = nextMember[member])
      fused.memberEventIndices.push_back(member);
    std::sort(fused.memberEventIndices.begin(), fused.memberEventIndices.end());
    tentative.push_back(std::move(fused));
  }
  if (cancelled(options)) return fail(error, "Refined fusion cancelled");
  tentativeSummary.outputEvents = tentative.size();
  output.swap(tentative);
  summary = std::move(tentativeSummary);
  error.clear();
  return true;
}

}  // namespace signalstudio
