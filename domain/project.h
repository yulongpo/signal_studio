#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/annotation.h"
#include "domain/detection.h"
#include "domain/sample_format.h"

namespace signalstudio {

enum class MainMode { TimeFrequency, Waterfall };
enum class AuxiliaryMode { Waveform, Psd };
enum class WaveformMode { I, Q, IqRms, Envelope };
enum class Palette {
  Turbo,
  Viridis,
  Gray,
  Plasma,
  Inferno,
  Magma,
  Cividis,
  CoolEditClassic
};

enum class SpectralMethod { Periodogram, Bartlett, Welch, Multitaper, Burg };
enum class SpectrumStatistic { Mean, Maximum, Minimum };
enum class SpectralWindow {
  Rectangular,
  Hann,
  Hamming,
  Blackman,
  BlackmanHarris,
  FlatTop,
  Kaiser
};
enum class PsdScope { Visible, SourceMark, Whole };
struct SpectralParameters {
  SpectralMethod method = SpectralMethod::Welch;
  SpectralWindow window = SpectralWindow::Hann;
  double overlap = .5;
  double segmentMilliseconds = 0;  // Zero selects N/B automatically.
  double kaiserBeta = 8.6, timeBandwidth = 3.5;
  int tapers = 6, burgOrder = 16;
  bool removeMean = false;
  bool operator==(const SpectralParameters&) const = default;
};
struct PsdSettings {
  SpectralParameters parameters;
  SpectrumStatistic statistic = SpectrumStatistic::Mean;
  PsdScope scope = PsdScope::Visible;
  bool operator==(const PsdSettings&) const = default;
};
struct SpectrogramSettings {
  SpectralParameters parameters;
  bool operator==(const SpectrogramSettings&) const = default;
};
enum class LoadStatus { Ready, Loading, Partial, Failed };
struct FileAvailability {
  SampleIndex availableSamples = 0;
  LoadStatus status = LoadStatus::Ready;
  std::uint64_t generation = 0;
  std::string fingerprint, error;
};
struct EnvelopePoint {
  SampleIndex begin = 0, end = 0, peakSample = 0;
  float peak = 0;
};

enum class ChannelFilter { FastPreview, Standard, HighRejection };
enum class ChannelProcessingState {
  Ready,
  LegacyNeedsReview,
  SourceMissing,
  Invalid
};
enum class NarrowbandPage { Observe, Modulation, DeepLearning, Demodulation };
enum class NarrowbandWaveform { IQ, Magnitude, Phase, Envelope };

struct DisplaySettings {
  MainMode mainMode = MainMode::TimeFrequency;
  AuxiliaryMode auxiliaryMode = AuxiliaryMode::Waveform;
  WaveformMode waveformMode = WaveformMode::IqRms;
  Palette palette = Palette::CoolEditClassic;
  int stftSize = 2048;
  int psdSize = 4096;
  PsdSettings psd;
  SpectrogramSettings spectrogram;
  double dynamicRangeDb = 80;
  double referenceLevelDb = 0;
  bool absoluteFrequency = true;
  bool grid = true;
  bool colorScale = false;
  bool psdFromSelection = false;
  // Compatibility fields expose the active mode; Session maintains both saved
  // ranges.
  double auxiliaryMin = -32768;
  double auxiliaryMax = 32768;
  double waveformMin = -32768;
  double waveformMax = 32768;
  bool waveformAutoFit = true;
  double psdMin = -100;
  double psdMax = 0;
};

struct FileMetadata {
  std::string id;
  std::string name;
  std::string path;
  double sampleRateHz = 0;
  double centerFrequencyHz = 0;
  SampleIndex sampleCount = 0;
  bool demo = true;
  int demoSeed = 1;
  double declaredBandwidthHz = 0;
  // The centered band used by analysis. Zero means the complete sampled band.
  double effectiveBandwidthHz = 0;
  FileAvailability availability;
  SampleFormat sampleFormat;
};
struct Channel {
  std::string id;
  std::string name;
  std::string sourceMarkId;
  double centerFrequencyHz = 0;
  double bandwidthHz = 0;
  TimeRange sourceTime;
  double outputSampleRateHz = 4e6;
  ChannelFilter filter = ChannelFilter::Standard;
  ChannelProcessingState processingState =
      ChannelProcessingState::LegacyNeedsReview;
  std::uint64_t configVersion = 1;
  bool wholeSource = false;
  bool preserveSourceTime = true;
  NarrowbandPage page = NarrowbandPage::Observe;
  TimeRange visibleSourceTime;
  FrequencyRange visibleBasebandFrequency;
  int psdFftSize = 4096;
  int stftFftSize = 2048;
  PsdSettings psd{SpectralParameters{}, SpectrumStatistic::Mean,
                  PsdScope::Whole};
  SpectrogramSettings spectrogram;
  NarrowbandWaveform waveform = NarrowbandWaveform::IQ;
  double waveformAxisMinimum = -32768.0;
  double waveformAxisMaximum = 32768.0;
  bool waveformAutoScale = true;
  double psdAxisMinimum = -120.0;
  double psdAxisMaximum = 0.0;
  double symbolRate = 250e3;
  int eyePeriods = 2;
  int eyeTraces = 64;
  int eyeComponent = 0;
  int selectedBit = -1;
  double constellationMinimum = -1.0;
  double constellationMaximum = 1.0;
  bool relativeTime = false;
  bool absoluteFrequencyLabels = false;
  std::string annotationId, segmentId, revisionId;
  std::shared_ptr<const AnnotationChannelSnapshot> sourceSnapshot;
  SourceIdentity sourceIdentity;
  ChannelSourceRelation sourceRelation =
      ChannelSourceRelation::LegacyNeedsReview;
};

struct ChannelViewSnapshot {
  std::string channelId;
  TimeRange sourceTime;
  FrequencyRange basebandFrequency;
  double waveformAxisMinimum = -32768.0;
  double waveformAxisMaximum = 32768.0;
  bool waveformAutoScale = true;
  double psdAxisMinimum = -120.0;
  double psdAxisMaximum = 0.0;
  bool operator==(const ChannelViewSnapshot&) const = default;
};
struct AnnotationViewFilter {
  int source = -1;
  int review = -1;
  std::string runId;
  double minimumMean = 0;
};

struct FileState {
  FileMetadata metadata;
  ViewRange view;
  DisplaySettings display;
  std::vector<Mark> marks;
  std::vector<Channel> channels;
  std::vector<std::string> selectedMarkIds;
  std::string activeMarkId;
  // Session-only selection context. Views project these values; their native
  // item selection models never own a separate annotation selection.
  std::string selectionAnchorId, hoveredMarkId, hoveredSegmentId;
  std::optional<std::uint32_t> selectionSampleChannel;
  std::vector<EnvelopePoint>
      navigationEnvelope;  // Session-only; rebuilt by actual source scanning.
  std::vector<AnnotationSourceSet> sourceSets;
  std::vector<DetectionRun> detectionRuns;
  std::string activeSegmentId, revisionEditingAnnotationId,
      revisionEditingBaseRevisionId;
  std::array<bool, 3> annotationLayers{true, true, true};
  AnnotationViewFilter annotationFilter;
};
inline bool annotationInContext(const FileState& file,
                                const Annotation& value) {
  if (value.deleted ||
      (!value.fileId.empty() && value.fileId != file.metadata.id))
    return false;
  for (const auto& source : file.sourceSets)
    if (source.id == value.sourceSetId)
      return !source.deleted &&
             source.sampleChannel == file.metadata.sampleFormat.selectedChannel;
  return value.sourceSetId.empty();  // Unmigrated legacy projection.
}
inline bool annotationVisible(const FileState& file, const Annotation& value) {
  const auto& filter = file.annotationFilter;
  return annotationInContext(file, value) &&
         file.annotationLayers[static_cast<int>(value.sourceKind)] &&
         (filter.source < 0 ||
          filter.source == static_cast<int>(value.sourceKind)) &&
         (filter.review < 0 ||
          filter.review == static_cast<int>(value.reviewStatus)) &&
         (filter.runId.empty() ||
          (filter.runId == "__no_run__" &&
           (!value.originEvidence || value.originEvidence->runId.empty())) ||
          (value.originEvidence &&
           value.originEvidence->runId == filter.runId)) &&
         (filter.minimumMean <= 0 ||
          (value.originEvidence && value.originEvidence->scoreMean &&
           *value.originEvidence->scoreMean >= filter.minimumMean));
}
struct AnnotationCounts {
  std::size_t total = 0, matching = 0, selected = 0;
};
inline AnnotationCounts annotationCounts(const FileState& file) {
  AnnotationCounts counts;
  for (const auto& value : file.marks) {
    if (annotationInContext(file, value)) ++counts.total;
    if (annotationVisible(file, value)) ++counts.matching;
  }
  counts.selected = file.selectedMarkIds.size();
  return counts;
}
struct Project {
  std::string name = "未命名工程";
  std::string activeFileId;
  std::string activeChannelId;
  bool narrowbandWorkspaceOpen = false;
  std::vector<FileState> files;
};

struct ViewSnapshot {
  std::string fileId;
  ViewRange view;
  double waveformMin = -32768;
  double waveformMax = 32768;
  double psdMin = -100;
  double psdMax = 0;
  bool waveformAutoFit = true;
  bool operator==(const ViewSnapshot&) const = default;
};

ViewRange fullRange(const FileMetadata& metadata);
double analysisFrequencyOffset(const FileMetadata& metadata);
SampleIndex availableSamples(const FileMetadata& metadata);
ViewRange clampRange(ViewRange range, const FileMetadata& metadata,
                     int stftSize, int psdSize = 0);
Mark* findMark(FileState& file, const std::string& id);
const Mark* findMark(const FileState& file, const std::string& id);

}  // namespace signalstudio
