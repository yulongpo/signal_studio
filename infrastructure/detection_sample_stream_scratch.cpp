#include "infrastructure/detection_sample_stream_scratch.h"
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace signalstudio::sample_stream::detail {
namespace { constexpr qint64 headerBytes = 64; }
bool fail(Failure& f, ErrorCode code, const char* stage, QString text) {
  f.code = code; f.stage = QString::fromLatin1(stage); f.detail = std::move(text);
  return false;
}
bool cancelled(const Context& c, Failure& f, const char* stage) {
  return c.options.cancelled && c.options.cancelled()
      ? fail(f, ErrorCode::Cancelled, stage, "Sample refinement cancelled") : true;
}
Scratch::Scratch(const Runtime& r, Statistics& s) : runtime_(r), statistics_(s) {}
Scratch::~Scratch() { Failure ignored; close(ignored); }
bool Scratch::open(const Context& c, const Config& cfg, const Plan& p, Failure& f) {
  if (!runtime_.reserveScratch || !runtime_.releaseScratch)
    return fail(f, ErrorCode::Integration, "scratch", "Shared scratch lease callbacks required");
  QString why;
  if (!scratch_recovery::prepareTrustedRoot(runtime_.privateScratchRoot, why))
    return fail(f, ErrorCode::Integration, "scratch-root", why);
  QFileInfo root(runtime_.privateScratchRoot);
  if (!root.isDir() || root.isSymLink() || root.canonicalFilePath().isEmpty())
    return fail(f, ErrorCode::Integration, "scratch", "Existing trusted private scratch directory required");
  scratch_recovery::Limits limits;
  limits.cancelled = c.options.cancelled;
  scratch_recovery::Report recovered;
  if (!scratch_recovery::recoverOrphans(runtime_.privateScratchRoot, limits, recovered, why))
    return fail(f, ErrorCode::ScratchIo, "scratch-recovery", why);
  if (recovered.cancelled)
    return fail(f, ErrorCode::Cancelled, "scratch-recovery", "Cancelled during bounded orphan recovery");
  if (!recovered.supported || !recovered.safeToAdmit)
    return fail(f, ErrorCode::ResourceLimit, "scratch-recovery",
                QString("Sample scratch root has retained/unknown/incomplete owned resources; activeOther=%1 invalid=%2 unknown=%3 complete=%4; %5")
                    .arg(recovered.activeOtherProcessOwners).arg(recovered.invalidOrForeignSkipped)
                    .arg(recovered.unknownProcessSkipped).arg(recovered.complete)
                    .arg(recovered.diagnostics.value(0)));
  reservation_ = p.scratchReservationBytes; count_ = p.count;
  if (reservation_ > cfg.scratchBudgetBytes)
    return fail(f, ErrorCode::ResourceLimit, "scratch", "Sample scratch quota exceeded");
  const QStorageInfo volume(root.canonicalFilePath());
  if (!volume.isValid() || !volume.isReady() || volume.bytesAvailable() < 0)
    return fail(f, ErrorCode::ScratchIo, "scratch", "Scratch volume capacity unavailable");
  const auto available = static_cast<std::uint64_t>(volume.bytesAvailable());
  if (available < cfg.scratchFreeFloorBytes ||
      reservation_ > available - cfg.scratchFreeFloorBytes)
    return fail(f, ErrorCode::ResourceLimit, "scratch", "Scratch volume free-space floor not met");
  if (!runtime_.reserveScratch(reservation_, why))
    return fail(f, ErrorCode::ResourceLimit, "scratch", why);
  leased_ = true;
  dir_ = std::make_unique<QTemporaryDir>(root.canonicalFilePath() + "/sample-XXXXXX");
  dir_->setAutoRemove(false); // explicit cleanup reports failure and retains lease
  if (!dir_->isValid())
    return fail(f, ErrorCode::ScratchIo, "scratch", dir_->errorString());
  ownedPath_ = dir_->path();
  owner_ = std::make_unique<scratch_recovery::OwnerGuard>();
  if (!owner_->begin(runtime_.privateScratchRoot, ownedPath_, count_, c.searchSamples.begin, reservation_, why))
    return fail(f, ErrorCode::ScratchIo, "scratch-owner", why);
  liveBytes_ = owner_->metadataBytes();
  if (liveBytes_ > reservation_ || 128 > reservation_ - liveBytes_)
    return fail(f, ErrorCode::ResourceLimit, "scratch-owner", "Owned metadata exceeds reserved scratch bytes");
  statistics_.scratchBytesWritten += liveBytes_;
  energy.setFileName(ownedPath_ + "/energy.f32");
  smooth.setFileName(ownedPath_ + "/smooth.f32");
  for (QFile* file : {&energy, &smooth}) {
    if (!cancelled(c, f, "scratch")) return false;
    if (!file->open(QIODevice::ReadWrite | QIODevice::Truncate))
      return fail(f, ErrorCode::ScratchIo, "scratch", file->errorString());
    QByteArray header(headerBytes, '\0');
    std::memcpy(header.data(), "SSSE0001", 8);
    qToLittleEndian<quint64>(count_, header.data() + 8);
    qToLittleEndian<quint64>(c.searchSamples.begin, header.data() + 16);
    if (file->write(header) != header.size())
      return fail(f, ErrorCode::ScratchIo, "scratch", "Scratch header short write");
    liveBytes_ += headerBytes;
    statistics_.scratchBytesWritten += headerBytes;
  }
  statistics_.scratchPeakBytes = liveBytes_;
  return true;
}
bool Scratch::append(QFile& file, std::span<const float> values, Failure& f) {
  const auto bytes = static_cast<std::uint64_t>(values.size()) * 4;
  if (liveBytes_ > reservation_ || bytes > reservation_ - liveBytes_ ||
      file.size() < headerBytes || file.pos() != file.size() ||
      static_cast<std::uint64_t>(file.size() - headerBytes) + bytes > count_ * 4)
    return fail(f, ErrorCode::ResourceLimit, "scratch-write", "Scratch append exceeds exact count/quota");
  QByteArray buffer(static_cast<qsizetype>(bytes), '\0');
  for (std::size_t i = 0; i < values.size(); ++i)
    qToLittleEndian<quint32>(std::bit_cast<std::uint32_t>(values[i]), buffer.data() + 4*i);
  if (file.write(buffer) != buffer.size())
    return fail(f, ErrorCode::ScratchIo, "scratch-write", "Scratch data short write");
  liveBytes_ += bytes; statistics_.scratchBytesWritten += bytes;
  statistics_.scratchPeakBytes = std::max(statistics_.scratchPeakBytes, liveBytes_);
  return true;
}
bool Scratch::seal(QFile& file, Failure& f) {
  if (file.size() != headerBytes + static_cast<qint64>(count_ * 4) || !file.flush())
    return fail(f, ErrorCode::ScratchIo, "scratch-seal", "Incomplete scratch length or flush failure");
  return true;
}
bool Scratch::read(QFile& file, SampleIndex first, std::span<float> values, Failure& f) {
  if (first > count_ || values.size() > count_ - first ||
      file.size() != headerBytes + static_cast<qint64>(count_ * 4))
    return fail(f, ErrorCode::ScratchIo, "scratch-read", "Scratch exact count mismatch");
  if (!file.seek(0)) return fail(f, ErrorCode::ScratchIo, "scratch-read", file.errorString());
  const auto header = file.read(headerBytes);
  if (header.size() != headerBytes || std::memcmp(header.constData(), "SSSE0001", 8) ||
      qFromLittleEndian<quint64>(header.constData() + 8) != count_)
    return fail(f, ErrorCode::ScratchIo, "scratch-read", "Scratch header invalid");
  if (!file.seek(headerBytes + static_cast<qint64>(first * 4)))
    return fail(f, ErrorCode::ScratchIo, "scratch-read", file.errorString());
  const auto data = file.read(static_cast<qint64>(values.size() * 4));
  if (data.size() != static_cast<qsizetype>(values.size() * 4))
    return fail(f, ErrorCode::ScratchIo, "scratch-read", "Scratch short read");
  statistics_.scratchBytesRead += headerBytes + data.size();
  for (std::size_t i = 0; i < values.size(); ++i)
    values[i] = std::bit_cast<float>(qFromLittleEndian<quint32>(data.constData() + 4*i));
  return true;
}
bool Scratch::removeEnergy(Failure& f) {
  const auto bytes = energy.size(); energy.close();
  if (!energy.remove()) return fail(f, ErrorCode::Cleanup, "scratch-cleanup", energy.errorString());
  liveBytes_ -= static_cast<std::uint64_t>(bytes);
  return true;
}
bool Scratch::close(Failure& f) {
  energy.close(); smooth.close();
  if (dir_ && dir_->isValid() && QFileInfo::exists(ownedPath_)) {
    QString why;
    if (!owner_ || !owner_->ownsDirectory() || !owner_->cleanup(why)) {
      f.cleanupFailure = ownedPath_;
      if (f.code == ErrorCode::None)
        fail(f, ErrorCode::Cleanup, "scratch-cleanup", why.isEmpty() ? "Unproven scratch ownership" : why);
      return false; // no release while files may still occupy the shared lease
    }
  }
  owner_.reset(); dir_.reset(); ownedPath_.clear(); liveBytes_ = 0;
  if (leased_) { runtime_.releaseScratch(reservation_); leased_ = false; }
  return true;
}

bool exactQuantiles(Scratch& files, const Context& c, const Plan& p,
                    double& noise, double& burst, Failure& f,
                    gpu_dsp::Session* gpu) {
  struct Target { std::uint64_t rank; std::uint32_t prefix = 0; };
  std::array<Target,4> targets{};
  std::array<double,2> fractions{c.options.sampleNoiseQuantile,c.options.sampleBurstQuantile};
  std::array<float,2> weights{};
  for (std::size_t j = 0; j < 2; ++j) {
    const double position = fractions[j] * static_cast<double>(p.count - 1);
    const auto a = static_cast<std::uint64_t>(std::floor(position));
    const auto b = static_cast<std::uint64_t>(std::ceil(position));
    targets[2*j].rank = a; targets[2*j+1].rank = b;
    weights[j] = static_cast<float>(position - static_cast<double>(a));
  }
  if (gpu && p.count > 0 && p.count <= gpu_dsp::Limits{}.maximumElements &&
      p.count <= std::numeric_limits<unsigned>::max()) {
    std::vector<float> values(static_cast<std::size_t>(p.count));
    std::array<std::uint64_t, 4> ranks{};
    for (std::size_t i = 0; i < ranks.size(); ++i) ranks[i] = targets[i].rank;
    for (SampleIndex first = 0; first < p.count;) {
      if (!cancelled(c, f, "radix")) return false;
      const auto count = static_cast<std::size_t>(std::min<SampleIndex>(p.outputBlockSamples, p.count - first));
      if (!files.read(files.smooth, first, std::span<float>(values).subspan(static_cast<std::size_t>(first), count), f))
        return false;
      first += count;
    }
    std::array<float, 4> ends{};
    gpu_dsp::Error gpuError;
    if (gpu->selectOrderStatistics(values, ranks, ends, gpuError)) {
      const auto interpolate = [&](std::size_t j) {
        const float a = ends[2 * j], b = ends[2 * j + 1], w = weights[j];
        volatile float difference = b - a;
        if (w < .5f) { volatile float product = difference * w; volatile float v = a + product; return static_cast<double>(v); }
        volatile float inverse = 1.f - w;
        volatile float product = difference * inverse;
        volatile float v = b - product; return static_cast<double>(v);
      };
      noise = interpolate(0); burst = interpolate(1);
      if (!std::isfinite(noise) || !std::isfinite(burst))
        return fail(f, ErrorCode::Numerical, "radix", "Exact interpolation overflow");
      return true;
    }
  }
  std::vector<float> block(p.outputBlockSamples);
  for (const unsigned shift : {24U,16U,8U,0U}) {
    if (!cancelled(c, f, "radix")) return false;
    std::array<std::uint32_t,4> prefixes{};
    std::array<std::array<std::uint64_t,256>,4> hist{};
    std::size_t groups = 0;
    for (const auto& t : targets)
      if (std::find(prefixes.begin(), prefixes.begin()+groups, t.prefix) == prefixes.begin()+groups)
        prefixes[groups++] = t.prefix;
    const std::uint32_t mask = shift == 24 ? 0 : (0xffffffffU << (shift+8));
    for (SampleIndex first = 0; first < p.count;) {
      const auto count = static_cast<std::size_t>(std::min<SampleIndex>(block.size(), p.count-first));
      const auto view = std::span<float>(block).first(count);
      if (!files.read(files.smooth, first, view, f)) return false;
      for (std::size_t i = 0; i < count; ++i) {
        if (!(i % 4096) && !cancelled(c, f, "radix")) return false;
        const auto v = block[i];
        if (!std::isfinite(v)) return fail(f, ErrorCode::Numerical, "radix", "Nonfinite smoothed cell");
        const auto bits = std::bit_cast<std::uint32_t>(v);
        const auto key = bits & 0x80000000U ? ~bits : bits ^ 0x80000000U;
        for (std::size_t g = 0; g < groups; ++g)
          if ((key & mask) == prefixes[g]) ++hist[g][(key >> shift) & 255U];
      }
      first += count;
    }
    for (auto& t : targets) {
      const auto g = static_cast<std::size_t>(std::find(prefixes.begin(),prefixes.begin()+groups,t.prefix)-prefixes.begin());
      unsigned bucket = 0;
      while (bucket < 256 && t.rank >= hist[g][bucket]) { t.rank -= hist[g][bucket]; ++bucket; }
      if (bucket == 256) return fail(f, ErrorCode::Numerical, "radix", "Exact rank population mismatch");
      t.prefix |= bucket << shift;
    }
  }
  std::array<float,4> ends{};
  for (std::size_t i = 0; i < 4; ++i) {
    const auto key = targets[i].prefix;
    ends[i] = std::bit_cast<float>(key & 0x80000000U ? key ^ 0x80000000U : ~key);
  }
  const auto interpolate = [&](std::size_t j) {
    const float a = ends[2*j], b = ends[2*j+1], w = weights[j];
    // Explicit stores avoid new contraction/reassociation in this draft.
    volatile float difference = b-a;
    if (w < .5f) { volatile float product = difference*w; volatile float v = a+product; return static_cast<double>(v); }
    volatile float inverse = 1.f-w;
    volatile float product = difference*inverse;
    volatile float v = b-product; return static_cast<double>(v);
  };
  noise = interpolate(0); burst = interpolate(1);
  if (!std::isfinite(noise) || !std::isfinite(burst))
    return fail(f, ErrorCode::Numerical, "radix", "Exact interpolation overflow");
  return true;
}
} // namespace signalstudio::sample_stream::detail
