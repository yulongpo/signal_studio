#include "ui/detection/detection_resource_options.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <limits>
#include <optional>

#include "infrastructure/runtime_log.h"
#include "ui/parameter_input.h"

namespace signalstudio {
namespace {
constexpr qulonglong kMiB = 1024ULL * 1024ULL;
constexpr qulonglong kGiB = 1024ULL * kMiB;
constexpr int kEvidenceMinMiB = 1;
constexpr int kWorkingSetMinMiB =
    static_cast<int>(kDetectionWorkingSetMinBytes / kMiB);
constexpr int kScratchMinGiB = 1;
constexpr int kRawHeadMinGiB = 1;

const QString kRetainKey = QStringLiteral("detection/resources/retainRawModelOutput");
const QString kEvidenceKey = QStringLiteral("detection/resources/evidenceBudgetBytes");
const QString kWorkingSetKey = QStringLiteral("detection/resources/workingSetBudgetBytes");
const QString kScratchKey = QStringLiteral("detection/resources/sampleScratchBudgetBytes");
const QString kRawHeadKey = QStringLiteral("detection/resources/rawHeadBudgetBytes");
const QString kLogDirectoryKey = QStringLiteral("detection/resources/logDirectory");

int wholeUnits(std::uint64_t bytes, std::uint64_t unit) {
  if (unit == 0) return 0;
  const auto count = bytes / unit;
  const auto cap = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
  return static_cast<int>(std::min(count, cap));
}
void configureSpin(QSpinBox* box, int minimum, int maximum, int value) {
  if (maximum < 0) maximum = 0;
  if (minimum < 0) minimum = 0;
  if (minimum > maximum) minimum = maximum;
  box->setRange(minimum, maximum);
  box->setValue(std::clamp(value, box->minimum(), box->maximum()));
}
std::string volumePath(const QString& storagePath) {
  return storagePath.toStdString();
}

class NoWheelCheckBox final : public QCheckBox {
 public:
  using QCheckBox::QCheckBox;

 protected:
  void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

std::optional<std::uint64_t> storedBytes(const QSettings& settings,
                                         const QString& key,
                                         std::uint64_t minimum,
                                         std::uint64_t maximum,
                                         std::uint64_t unit) {
  if (!settings.contains(key)) return std::nullopt;
  bool ok = false;
  const auto value = settings.value(key).toString().toULongLong(&ok);
  if (!ok || value < minimum || value > maximum || value % unit != 0)
    return std::nullopt;
  return value;
}

QString defaultLogFile() {
  const auto root =
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  return QDir(root).filePath(QStringLiteral("logs/signal-studio.log"));
}
}  // namespace

DetectionResourceOptions DetectionResourceOptions::load(const QString& storagePath) {
  QSettings settings;
  const auto path = volumePath(storagePath);
  const auto diskLimit = detectionDiskBudgetLimitBytes(path);
  const auto diskDefault = detectionDiskBudgetDefaultBytes(path);
  const auto memoryLimit = detectionWorkingSetLimitBytes();
  DetectionResourceOptions options;
  options.evidenceBudgetBytes = diskDefault;
  options.sampleScratchBudgetBytes = diskDefault;
  options.rawHeadBudgetBytes = diskDefault;
  options.workingSetBudgetBytes = detectionWorkingSetDefaultBytes();
  if (settings.contains(kRetainKey)) {
    const auto retain = settings.value(kRetainKey);
    const auto text = retain.toString();
    if (retain.typeId() == QMetaType::Bool || text == QLatin1String("true") ||
        text == QLatin1String("false"))
      options.retainRawModelOutput = retain.toBool();
  }
  if (const auto evidence = storedBytes(settings, kEvidenceKey, kMiB,
                                       diskLimit, kMiB))
    options.evidenceBudgetBytes = *evidence;
  if (const auto working = storedBytes(settings, kWorkingSetKey,
                                      kDetectionWorkingSetMinBytes,
                                      memoryLimit, kMiB))
    options.workingSetBudgetBytes = *working;
  if (const auto scratch = storedBytes(settings, kScratchKey, kGiB,
                                      diskLimit, kGiB))
    options.sampleScratchBudgetBytes = *scratch;
  if (const auto rawHead = storedBytes(settings, kRawHeadKey, kGiB, diskLimit, kGiB))
    options.rawHeadBudgetBytes = *rawHead;
  const auto directory = settings.value(kLogDirectoryKey).toString().trimmed();
  if (!directory.contains(QChar(0))) options.logDirectory = directory;
  return options;
}

void DetectionResourceOptions::save() const {
  QSettings settings;
  settings.setValue(kRetainKey, retainRawModelOutput);
  settings.setValue(kEvidenceKey, QString::number(evidenceBudgetBytes));
  settings.setValue(kWorkingSetKey, QString::number(workingSetBudgetBytes));
  settings.setValue(kScratchKey, QString::number(sampleScratchBudgetBytes));
  settings.setValue(kRawHeadKey, QString::number(rawHeadBudgetBytes));
  settings.setValue(kLogDirectoryKey, logDirectory);
  settings.sync();
}

void DetectionResourceOptions::applyTo(DetectionRequest& request) const {
  request.legacyCombinedEvidence = false;
  request.retainRawModelOutput = retainRawModelOutput;
  request.rawHeadBudgetBytes = retainRawModelOutput ? rawHeadBudgetBytes : 0;
  request.evidenceBudgetBytes = evidenceBudgetBytes;
  request.workingSetBudgetBytes = workingSetBudgetBytes;
  request.sampleScratchBudgetBytes = sampleScratchBudgetBytes;
}

QString DetectionResourceOptions::resolvedLogFile() const {
  const auto directory = logDirectory.trimmed();
  if (directory.isEmpty()) return defaultLogFile();
  return QDir(directory).filePath(QStringLiteral("signal-studio.log"));
}

DetectionResourceOptionsDialog::DetectionResourceOptionsDialog(
    QWidget* parent, const QString& storagePath)
    : QDialog(parent), storagePath_(storagePath) {
  setObjectName("detectionResourceOptionsDialog");
  setWindowTitle("选项");
  setModal(true);
  setMinimumWidth(480);
  auto* layout = new QVBoxLayout(this);
  auto* intro = new QLabel(
      "这些设置在按下确定后写入，只用于之后新建的检测请求。"
      "正在运行的检测仍使用它自己保存的请求。",
      this);
  intro->setObjectName("detectionResourceOptionsIntro");
  intro->setWordWrap(true);
  layout->addWidget(intro);

  retain_ = new NoWheelCheckBox("保留每窗模型原始输出", this);
  retain_->setObjectName("retainRawModelOutput");
  layout->addWidget(retain_);
  retentionPolicy_ = new QLabel(this);
  retentionPolicy_->setObjectName("rawHeadRetentionPolicy");
  retentionPolicy_->setWordWrap(true);
  layout->addWidget(retentionPolicy_);
  const auto path = volumePath(storagePath_);
  const auto diskLimit = detectionDiskBudgetLimitBytes(path);
  const auto diskDefault = detectionDiskBudgetDefaultBytes(path);
  const int diskLimitGiB = wholeUnits(diskLimit, kGiB);
  const int diskDefaultGiB = wholeUnits(diskDefault, kGiB);
  const int evidenceLimitMiB = wholeUnits(diskLimit, kMiB);
  const int evidenceDefaultMiB = wholeUnits(diskDefault, kMiB);
  const int memoryLimitMiB = wholeUnits(detectionWorkingSetLimitBytes(), kMiB);
  const int memoryDefaultMiB = wholeUnits(detectionWorkingSetDefaultBytes(), kMiB);

  auto* form = new QFormLayout;
  form->setContentsMargins(0, 8, 0, 0);
  const auto spin = [this](const char* name, const QString& suffix) {
    auto* box = new ParameterIntSpinBox(this);
    box->setObjectName(name);
    box->setSuffix(suffix);
    box->setMinimumWidth(140);
    return box;
  };
  evidenceMiB_ = spin("evidenceBudgetMiB", " MiB");
  workingSetMiB_ = spin("workingSetBudgetMiB", " MiB");
  scratchGiB_ = spin("sampleScratchBudgetGiB", " GiB");
  rawHeadGiB_ = spin("rawHeadBudgetGiB", " GiB");
  configureSpin(evidenceMiB_, kEvidenceMinMiB, evidenceLimitMiB, evidenceDefaultMiB);
  configureSpin(workingSetMiB_, kWorkingSetMinMiB, memoryLimitMiB, memoryDefaultMiB);
  configureSpin(scratchGiB_, kScratchMinGiB, diskLimitGiB, diskDefaultGiB);
  configureSpin(rawHeadGiB_, kRawHeadMinGiB, diskLimitGiB, diskDefaultGiB);
  evidenceMiB_->setToolTip(
      "观测映射 JSON 的总预算。默认 64 GiB；本卷剩余空间更小时等于剩余空间。"
      "上限是该预算所在卷的剩余磁盘。单片仍是 8 MiB。");
  workingSetMiB_->setToolTip(
      "检测工作集。默认 8 GiB；物理内存更小时等于物理内存。上限是本机物理内存。");
  scratchGiB_->setToolTip(
      "Sample 精修临时盘。默认 64 GiB；本卷剩余空间更小时等于剩余空间。"
      "上限是该预算所在卷的剩余磁盘。");
  rawHeadGiB_->setToolTip(
      "打开保留每窗模型原始输出后使用。默认 64 GiB；本卷剩余空间更小时等于剩余空间。"
      "上限是该卷剩余磁盘。关闭时不按这项拒绝检测。");
  form->addRow("观测 JSON 预算", evidenceMiB_);
  form->addRow("检测工作集预算", workingSetMiB_);
  const auto physicalRamBytes = detectionPhysicalMemoryBytes();
  const auto workingSetDefaultBytes = detectionWorkingSetDefaultBytes();
  auto* authorizationNote = new QLabel(this);
  authorizationNote->setObjectName("workingSetAuthorizationNote");
  authorizationNote->setWordWrap(true);
  authorizationNote->setFocusPolicy(Qt::NoFocus);
  authorizationNote->setTextInteractionFlags(Qt::TextSelectableByMouse);
  authorizationNote->setText(
      QString("本机物理内存 %1 字节（%2 MiB）。默认工作集 %3 字节（%4 MiB）。"
              "更大的工作集授权是使用上限，不是一次连续分配。")
          .arg(QString::number(physicalRamBytes))
          .arg(wholeUnits(physicalRamBytes, kMiB))
          .arg(QString::number(workingSetDefaultBytes))
          .arg(memoryDefaultMiB));
  form->addRow("工作集说明", authorizationNote);
  const auto snapshot = captureRuntimeResourceSnapshot(path, nullptr);
  auto* snapshotNote = new QLabel(this);
  snapshotNote->setObjectName("runtimeResourceSnapshotNote");
  snapshotNote->setWordWrap(true);
  snapshotNote->setFocusPolicy(Qt::NoFocus);
  snapshotNote->setTextInteractionFlags(Qt::TextSelectableByMouse);
  const auto bytesText = [](std::uint64_t value) {
    return QString::number(static_cast<qulonglong>(value));
  };
  const QString ramText =
      snapshot.ram.status == ResourceProbeStatus::Probed
          ? QString("物理内存可用 %1 字节，总计 %2 字节。")
                .arg(bytesText(snapshot.ram.availableBytes),
                     bytesText(snapshot.ram.totalBytes))
          : QString("物理内存探测失败（%1）。不把默认工作集当成实测可用内存。")
                .arg(QString::fromStdString(snapshot.ram.detail));
  const QString volumeText =
      snapshot.volume.status == ResourceProbeStatus::Probed
          ? QString("本卷 %1 剩余 %2 字节。")
                .arg(QString::fromStdString(snapshot.volume.volumeKey),
                     bytesText(snapshot.volume.availableBytes))
          : QString("本卷剩余空间探测失败（%1）。")
                .arg(QString::fromStdString(snapshot.volume.detail));
  snapshotNote->setText(
      ramText + volumeText +
      "显存未在此窗口探测，不记为可用。这次显示不写入工程，也不改变预算。");
  form->addRow("运行时快照", snapshotNote);
  form->addRow("Sample 临时盘", scratchGiB_);
  form->addRow("原始输出配额", rawHeadGiB_);
  layout->addLayout(form);

  const auto refreshPolicy = [this] {
    rawHeadGiB_->setEnabled(retain_->isChecked());
    retentionPolicy_->setText(
        retain_->isChecked()
            ? "打开后按本次窗口数 × 每窗原始输出字节核算。"
              "配额上限是本卷剩余空间。所需字节超过配额或剩余磁盘时，"
              "在推理前拒绝，并写明窗数、每窗字节、所需字节和剩余磁盘。"
            : "默认关闭。不写每窗 .f32 原始输出，预检不按原始输出大小拒绝。");
  };
  refreshPolicy();
  connect(retain_, &QCheckBox::toggled, this, [refreshPolicy](bool) { refreshPolicy(); });

  // Kept out of this dialog because they are not preferences:
  // 8 MiB shards, 100000 results, 64 KiB
  // performance JSON and 8 KiB worker stderr are file-format and crash bounds.
  // 262144 analysis windows, 262144 evidence rows, 4096 live tracks, 32768
  // tracks, per-frame pairing, 4 million comparisons, 32 MiB track residency
  // and 1024 preview events still fail with the actual count when exceeded.
  // Engine batches 1 and 2 are a device fact. Frequency IoU 0.3, the 100000
  // annotation-segment budget and undo budgets are algorithm or project contracts.

  auto* logTitle = new QLabel("运行日志目录", this);
  layout->addWidget(logTitle);
  logPath_ = new QLabel(this);
  logPath_->setObjectName("runtimeLogPath");
  logPath_->setWordWrap(true);
  logPath_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(logPath_);
  auto* logNote = new QLabel(
      QString("日志按 %1 MiB 轮转，保留 %2 份。这两项固定。")
          .arg(RuntimeLog::kDefaultRotateBytes / (1024 * 1024))
          .arg(RuntimeLog::kRetainedRotations),
      this);
  logNote->setObjectName("runtimeLogRotationNote");
  logNote->setWordWrap(true);
  layout->addWidget(logNote);
  logDirectory_ = new QLineEdit(this);
  logDirectory_->setObjectName("runtimeLogDirectory");
  logDirectory_->setPlaceholderText("空表示使用默认目录");
  layout->addWidget(logDirectory_);
  auto* logButtons = new QHBoxLayout;
  auto* browse = new QPushButton("浏览…", this);
  browse->setObjectName("browseRuntimeLogDirectory");
  auto* openFolder = new QPushButton("打开所在文件夹", this);
  openFolder->setObjectName("openRuntimeLogDirectory");
  logButtons->addWidget(browse);
  logButtons->addWidget(openFolder);
  logButtons->addStretch();
  layout->addLayout(logButtons);
  connect(logDirectory_, &QLineEdit::textChanged, this,
          [this] { refreshLogPath(); });
  connect(browse, &QPushButton::clicked, this, [this] {
    const auto current = logDirectory_->text().trimmed().isEmpty()
                             ? QFileInfo(previewLogFile()).absolutePath()
                             : logDirectory_->text();
    const auto chosen = QFileDialog::getExistingDirectory(
        this, "运行日志目录", current);
    if (!chosen.isEmpty()) logDirectory_->setText(QDir::toNativeSeparators(chosen));
  });
  connect(openFolder, &QPushButton::clicked, this, [this] {
    const auto folder = QFileInfo(previewLogFile()).absolutePath();
    if (!QDir().mkpath(folder) ||
        !QDesktopServices::openUrl(QUrl::fromLocalFile(folder))) {
      QMessageBox::warning(this, "选项", "无法打开日志所在文件夹。");
    }
  });

  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  buttons->button(QDialogButtonBox::Ok)->setObjectName("detectionResourceOptionsOk");
  buttons->button(QDialogButtonBox::Cancel)
      ->setObjectName("detectionResourceOptionsCancel");
  buttons->button(QDialogButtonBox::Ok)->setText("确定");
  buttons->button(QDialogButtonBox::Cancel)->setText("取消");
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this,
          &DetectionResourceOptionsDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  inputPolicy_ = new ParameterInputPolicy(this, true);

  const auto options = DetectionResourceOptions::load(storagePath_);
  retain_->setChecked(options.retainRawModelOutput);
  evidenceMiB_->setValue(wholeUnits(options.evidenceBudgetBytes, kMiB));
  workingSetMiB_->setValue(wholeUnits(options.workingSetBudgetBytes, kMiB));
  scratchGiB_->setValue(wholeUnits(options.sampleScratchBudgetBytes, kGiB));
  rawHeadGiB_->setValue(wholeUnits(options.rawHeadBudgetBytes, kGiB));
  logDirectory_->setText(options.logDirectory);
  refreshPolicy();
  refreshLogPath();
}

QString DetectionResourceOptionsDialog::previewLogFile() const {
  DetectionResourceOptions options;
  options.logDirectory = logDirectory_->text();
  return QFileInfo(options.resolvedLogFile()).absoluteFilePath();
}

void DetectionResourceOptionsDialog::refreshLogPath() {
  logPath_->setText(previewLogFile());
  logPath_->setToolTip(logPath_->text());
}

bool DetectionResourceOptionsDialog::commitDrafts() {
  for (auto* widget : findChildren<QWidget*>()) {
    if (auto* draft = dynamic_cast<ParameterDraft*>(widget)) draft->commit();
  }
  for (auto* widget : findChildren<QWidget*>()) {
    if (widget->property("parameterDraft").toBool() ||
        widget->property("invalid").toBool())
      return false;
  }
  return true;
}

void DetectionResourceOptionsDialog::accept() {
  if (!commitDrafts()) return;
  const auto directory = logDirectory_->text().trimmed();
  if (directory.contains(QChar(0))) return;
  if (!directory.isEmpty()) {
    const QFileInfo info(directory);
    if (info.exists() && !info.isDir()) {
      setParameterError(logDirectory_, "请选择文件夹。");
      return;
    }
  }
  const auto path = volumePath(storagePath_);
  const auto diskLimit = detectionVolumeFreeBytes(path);
  const auto memoryLimit = detectionWorkingSetLimitBytes();
  const auto evidence =
      static_cast<std::uint64_t>(evidenceMiB_->value()) * kMiB;
  const auto working =
      static_cast<std::uint64_t>(workingSetMiB_->value()) * kMiB;
  const auto scratch =
      static_cast<std::uint64_t>(scratchGiB_->value()) * kGiB;
  const auto rawHead =
      static_cast<std::uint64_t>(rawHeadGiB_->value()) * kGiB;
  if (diskLimit && (evidence > *diskLimit || scratch > *diskLimit ||
                    (retain_->isChecked() && rawHead > *diskLimit))) {
    setParameterError(evidenceMiB_, "磁盘预算超过本卷剩余空间。");
    return;
  }
  if (working < kDetectionWorkingSetMinBytes || working > memoryLimit) {
    setParameterError(workingSetMiB_, "工作集预算超过本机物理内存。");
    return;
  }
  DetectionResourceOptions options;
  options.retainRawModelOutput = retain_->isChecked();
  options.evidenceBudgetBytes = evidence;
  options.workingSetBudgetBytes = working;
  options.sampleScratchBudgetBytes = scratch;
  options.rawHeadBudgetBytes = rawHead;
  options.logDirectory = directory;
  options.save();
  RuntimeLog::install(options.resolvedLogFile());
  QDialog::accept();
}

}  // namespace signalstudio
