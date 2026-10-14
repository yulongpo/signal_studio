#include "ui/detection/detection_card.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <utility>

#include "infrastructure/detection_model_snapshot.h"
#include "infrastructure/detection_source.h"
#include "ui/controls/adaptive_value_edit.h"
#include "ui/controls/unsigned_value_edit.h"
#include "ui/detection/detection_phase.h"
#include "ui/parameter_input.h"

namespace signalstudio {
namespace {
QString text(const std::string& value) { return QString::fromUtf8(value); }

QString number(double value) { return QString::number(value, 'g', 9); }

void refreshStyle(QWidget* widget) {
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}

DetectionSourceConfig sourceConfig(const DetectionRequest& request) {
  DetectionSourceConfig config;
  config.profileId = request.profile.id;
  config.profileVersion = request.profile.version;
  config.fftSize = request.profile.fftSize;
  config.modelFrames = request.profile.modelFrames;
  config.modelHopFrames = request.profile.modelHopFrames;
  config.hopSeconds = request.profile.hopSeconds;
  config.workingSetBudgetBytes = request.workingSetBudgetBytes;
  return config;
}

QLabel* wrappingLabel(const char* name, QWidget* parent) {
  auto* label = new QLabel(parent);
  label->setObjectName(name);
  label->setWordWrap(true);
  label->setTextFormat(Qt::PlainText);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  return label;
}

QString rangeText(ViewRange range, double sampleRate) {
  if (!std::isfinite(sampleRate) || sampleRate <= 0) return "尚未提交范围";
  return QString("%1–%2 ms · RF %3–%4 MHz\n样本 [%5, %6)")
      .arg(number(static_cast<double>(range.time.begin) / sampleRate * 1000))
      .arg(number(static_cast<double>(range.time.end) / sampleRate * 1000))
      .arg(number(range.frequency.lowerHz / 1e6))
      .arg(number(range.frequency.upperHz / 1e6))
      .arg(range.time.begin)
      .arg(range.time.end);
}

ViewRange rfSegmentRange(const Segment& segment, const SourceIdentity& source) {
  auto range = segment.range;
  if (segment.frequencyFrame == FrequencyFrame::Baseband) {
    range.frequency.lowerHz += source.centerFrequencyHz;
    range.frequency.upperHz += source.centerFrequencyHz;
  }
  return range;
}

QString lifecycleText(DetectionLifecycle state) {
  switch (state) {
    case DetectionLifecycle::Queued:
      return "排队";
    case DetectionLifecycle::Validating:
      return "预检";
    case DetectionLifecycle::Running:
      return "运行";
    case DetectionLifecycle::Cancelling:
      return "取消中";
    case DetectionLifecycle::Completed:
      return "完成";
    case DetectionLifecycle::Cancelled:
      return "已取消";
    case DetectionLifecycle::Failed:
      return "失败";
    case DetectionLifecycle::Interrupted:
      return "中断 / 可恢复";
  }
  return "未知";
}

QWidget* foldableFields(const QString& title, QWidget* body, bool expanded,
                        const char* name, QWidget* parent) {
  auto* group = new QFrame(parent);
  group->setObjectName(name);
  auto* layout = new QVBoxLayout(group);
  layout->setContentsMargins(8, 0, 8, 4);
  layout->setSpacing(5);
  auto* toggle = new QToolButton(group);
  toggle->setObjectName(QString::fromLatin1(name) + "Toggle");
  toggle->setText(title);
  toggle->setCheckable(true);
  toggle->setChecked(expanded);
  toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
  body->setVisible(expanded);
  layout->addWidget(toggle);
  layout->addWidget(body);
  QObject::connect(
      toggle, &QToolButton::toggled, body, [body, toggle](bool open) {
        body->setVisible(open);
        toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
      });
  return group;
}
}  // namespace

DetectionCard::DetectionCard(Session& session,
                             std::function<QString()> projectDirectory,
                             QWidget* parent)
    : QWidget(parent),
      session_(session),
      projectDirectory_(std::move(projectDirectory)) {
  setObjectName("detectionCard");
  setMinimumWidth(0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  setStyleSheet(
      "#detectionPhysicalTimesBox, #detectionBranchParameters, "
      "#detectionAdvancedParameters { border: 1px solid #2b4961; }"
      "#detectionProfileDescription, #detectionBranchHelp, "
      "#detectionParameterDraftNote { color: #86a4ba; font-size: 10px; }"
      "#detectionPreflight { border-left: 2px solid #e6ba65; "
      "background: #262922; color: #dec99f; padding: 8px; }"
      "#detectionPreflight[preflightOk=\"true\"] { border-color: #63d2be; "
      "background: #132f32; color: #94c6bd; }"
      "#detectionTimeStrategy QPushButton:checked { background: #245878; "
      "color: #dbf7ff; }"
      "QLineEdit[invalid=\"true\"] { border: 1px solid #e98b95; }");
  for (const auto& profile : detectionProfiles()) {
    DetectionRequest initial;
    initial.profile = profile;
    initial.rangeOrigin = "visible";
    if (profile.backend == DetectionBackendKind::Scn)
      initial.parameters.timeStrategy = DetectionTimeStrategy::Coarse;
    profileConfigurations_.emplace(profile.id, std::move(initial));
  }
  configuration_ = profileConfigurations_.at(detectionProfiles().front().id);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(13, 6, 13, 8);
  layout->setSpacing(9);
  setupProfile(layout);
  setupRange(layout);
  setupTiming(layout);
  setupThresholds(layout);
  setupStrategy(layout);
  setupBranches(layout);
  setupAdvanced(layout);
  setupStatus(layout);
  // MainWindow owns the application input filter. This local policy only
  // discards card drafts when its physical source context changes.
  inputPolicy_ = new ParameterInputPolicy(this, false);
  loadControls();
  refresh();
}

QWidget* DetectionCard::pairedFields(const QString& firstLabel,
                                     QWidget* firstControl,
                                     const QString& secondLabel,
                                     QWidget* secondControl) {
  auto* pair = new QWidget(this);
  auto* layout = new QGridLayout(pair);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setHorizontalSpacing(11);
  layout->setVerticalSpacing(4);
  layout->setColumnStretch(0, 1);
  layout->setColumnStretch(1, 1);
  for (auto* control : {firstControl, secondControl}) {
    control->setMinimumWidth(0);
    control->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  }
  auto* first = new QLabel(firstLabel, pair);
  auto* second = new QLabel(secondLabel, pair);
  first->setBuddy(firstControl);
  second->setBuddy(secondControl);
  layout->addWidget(first, 0, 0);
  layout->addWidget(second, 0, 1);
  layout->addWidget(firstControl, 1, 0);
  layout->addWidget(secondControl, 1, 1);
  return pair;
}

void DetectionCard::setupProfile(QVBoxLayout* layout) {
  layout->addWidget(new QLabel("能力方案", this));
  profile_ = new QComboBox(this);
  profile_->setObjectName("detectionProfile");
  profile_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  profile_->addItem("FFSCN GUI IQ · 主方案", "ffscn-gui-iq-v1");
  profile_->addItem("SCN · 频率对照", "scn-max-psd-v1");
  profile_->addItem("FFSCN-17 · 实验", "ffscn-17-experimental-v1");
  if (qApp->property("enableDetectionTestFixture").toBool())
    profile_->addItem("系统链路夹具 · 非模型", "pipeline-fixture-v1");
  layout->addWidget(profile_);
  profileHelp_ = wrappingLabel("detectionProfileDescription", this);
  layout->addWidget(profileHelp_);
  connect(profile_, &QComboBox::currentIndexChanged, this, [this] {
    if (!updating_) switchProfile();
  });
}

void DetectionCard::setupRange(QVBoxLayout* layout) {
  layout->addWidget(new QLabel("检测范围", this));
  rangeKind_ = new QComboBox(this);
  rangeKind_->setObjectName("detectionRangeOrigin");
  rangeKind_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  rangeKind_->addItem("当前可见区域", "visible");
  rangeKind_->addItem("全可用文件", "available-file");
  rangeKind_->addItem("活动标记的单段", "active-segment");
  rangeKind_->addItem("自定义 ROI", "custom");
  layout->addWidget(rangeKind_);
  activeSegment_ = new QComboBox(this);
  activeSegment_->setObjectName("detectionActiveSegment");
  activeSegment_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  layout->addWidget(activeSegment_);
  rangeSummary_ = wrappingLabel("detectionRequestedRange", this);
  layout->addWidget(rangeSummary_);
  selectRoi_ = new QPushButton("▨ 选择单个检测 ROI…", this);
  selectRoi_->setObjectName("selectDetectionRoi");
  layout->addWidget(selectRoi_);
  connect(selectRoi_, &QPushButton::clicked, this,
          &DetectionCard::roiSelectionRequested);
  connect(rangeKind_, &QComboBox::currentIndexChanged, this, [this] {
    if (updating_) return;
    configuration_.rangeOrigin =
        rangeKind_->currentData().toString().toStdString();
    profileConfigurations_[configuration_.profile.id] = configuration_;
    refresh();
  });
  connect(activeSegment_, &QComboBox::currentIndexChanged, this, [this] {
    if (updating_) return;
    auto* file = session_.activeFile();
    if (!file) return;
    const auto id = activeSegment_->currentData().toString().toStdString();
    if (id.empty()) return;
    const auto result = session_.annotations().select(
        *file, file->selectedMarkIds, file->activeMarkId, id);
    if (!result.ok) emit message(text(result.error));
    refresh();
    if (result.ok) emit selectionChanged();
  });
}

UnsignedValueEdit* DetectionCard::addUnsigned(
    const char* name, const QString& label, std::uint64_t maximum,
    std::uint64_t minimum,
    std::function<std::uint64_t(const DetectionRequest&)> getter,
    std::function<void(DetectionRequest&, std::uint64_t)> setter) {
  auto* edit = new UnsignedValueEdit(getter(configuration_), this);
  edit->setObjectName(name);
  edit->setToolTip(QString("%1：%2–%3；Enter / Tab / 应用内点击提交")
                       .arg(label)
                       .arg(minimum)
                       .arg(maximum));
  unsignedControls_.push_back({edit, getter});
  edit->applied = [this, edit, getter, setter, label, minimum, maximum] {
    auto candidate = configuration_;
    QString issue;
    if (edit->value() < minimum || edit->value() > maximum)
      issue =
          QString("%1须为 %2–%3 的整数").arg(label).arg(minimum).arg(maximum);
    else {
      setter(candidate, edit->value());
      issue = configurationError(candidate);
    }
    if (!issue.isEmpty())
      rejectControl(edit, issue, [this, edit, getter] {
        edit->setValue(getter(configuration_));
      });
    else {
      edit->setProperty("detectionCommitError", QString{});
      edit->setProperty("invalid", false);
      refreshStyle(edit);
      saveConfiguration(std::move(candidate));
    }
  };
  connect(edit, &QLineEdit::textEdited, this, [this] { refreshStatus(); });
  connect(edit, &QLineEdit::textChanged, this, [this] {
    if (!updating_) refreshStatus();
  });
  return edit;
}

AdaptiveValueEdit* DetectionCard::addParameter(
    const char* name, const QString& label,
    double DetectionParameters::* member, double maximum, double scale) {
  const auto kind = member == &DetectionParameters::mergeFrequencyHz
                        ? UnitKind::Frequency
                        : UnitKind::Scalar;
  auto* edit = new AdaptiveValueEdit(
      kind, configuration_.parameters.*member * scale, this);
  edit->setObjectName(name);
  edit->setToolTip(QString("%1：0–%2；Enter / Tab / 应用内点击提交")
                       .arg(label)
                       .arg(maximum * scale));
  parameters_.push_back({edit, member, scale, maximum, label});
  connect(edit, &AdaptiveValueEdit::valueCommitted, this,
          [this, edit, member, scale](double value) {
            auto candidate = configuration_;
            candidate.parameters.*member = value / scale;
            const auto issue = configurationError(candidate);
            if (!issue.isEmpty())
              rejectControl(edit, issue, [this, edit, member, scale] {
                edit->setValue(configuration_.parameters.*member * scale);
              });
            else {
              edit->setProperty("detectionCommitError", QString{});
              refreshStyle(edit);
              saveConfiguration(std::move(candidate));
            }
          });
  connect(edit, &QLineEdit::textEdited, this, [this] { refreshStatus(); });
  connect(edit, &QLineEdit::textChanged, this, [this] {
    if (!updating_) refreshStatus();
  });
  return edit;
}

void DetectionCard::setupTiming(QVBoxLayout* layout) {
  auto* fft = addUnsigned(
      "detectionFftSize", "FFT N", 1048576, 1,
      [](const auto& value) { return value.profile.fftSize; },
      [](auto& value, auto number) {
        value.profile.fftSize = static_cast<std::uint32_t>(number);
      });
  hopMilliseconds_ = new AdaptiveValueEdit(UnitKind::Scalar, 1, this);
  hopMilliseconds_->setObjectName("detectionHopMilliseconds");
  hopMilliseconds_->setToolTip("谱步长 / ms；按 floor(Fs × hop) 对齐源样本");
  connect(hopMilliseconds_, &AdaptiveValueEdit::valueCommitted, this,
          [this](double milliseconds) {
            auto candidate = configuration_;
            candidate.profile.hopSeconds = milliseconds / 1000;
            const auto issue = configurationError(candidate);
            if (!issue.isEmpty())
              rejectControl(hopMilliseconds_, issue, [this] {
                hopMilliseconds_->setValue(configuration_.profile.hopSeconds *
                                           1000);
              });
            else {
              hopMilliseconds_->setProperty("detectionCommitError", QString{});
              refreshStyle(hopMilliseconds_);
              saveConfiguration(std::move(candidate));
            }
          });
  connect(hopMilliseconds_, &QLineEdit::textEdited, this,
          [this] { refreshStatus(); });
  connect(hopMilliseconds_, &QLineEdit::textChanged, this, [this] {
    if (!updating_) refreshStatus();
  });
  layout->addWidget(
      pairedFields("FFT N", fft, "谱步长 / ms", hopMilliseconds_));
  auto* frames = addUnsigned(
      "detectionModelFrames", "模型帧数", 1048576, 1,
      [](const auto& value) { return value.profile.modelFrames; },
      [](auto& value, auto number) {
        value.profile.modelFrames = static_cast<std::uint32_t>(number);
      });
  auto* slide = addUnsigned(
      "detectionModelHopFrames", "滑动帧数", 1048576, 1,
      [](const auto& value) { return value.profile.modelHopFrames; },
      [](auto& value, auto number) {
        value.profile.modelHopFrames = static_cast<std::uint32_t>(number);
      });
  layout->addWidget(pairedFields("模型帧数", frames, "滑动 / 帧", slide));
  auto* box = new QFrame(this);
  box->setObjectName("detectionPhysicalTimesBox");
  auto* boxLayout = new QVBoxLayout(box);
  boxLayout->setContentsMargins(9, 8, 9, 8);
  physicalTimes_ = wrappingLabel("detectionPhysicalTimes", box);
  physicalTimes_->setTextFormat(Qt::RichText);
  boxLayout->addWidget(physicalTimes_);
  layout->addWidget(box);
}

void DetectionCard::setupThresholds(QVBoxLayout* layout) {
  auto* score = addParameter("detectionScoreThreshold", "score threshold",
                             &DetectionParameters::scoreThreshold, 1);
  auto* nms = addParameter("detectionNmsIou", "NMS IoU",
                           &DetectionParameters::nmsIou, 1);
  layout->addWidget(pairedFields("score threshold", score, "NMS IoU", nms));
  auto* topK = addUnsigned(
      "detectionTopK", "topK", 512, 1,
      [](const auto& value) { return value.parameters.topK; },
      [](auto& value, auto number) {
        value.parameters.topK = static_cast<std::uint32_t>(number);
      });
  noiseMargin_ = addParameter("detectionBandwidthMarginDb", "噪底裕量 / dB",
                              &DetectionParameters::bandwidthMarginDb, 100);
  layout->addWidget(pairedFields("topK", topK, "噪底裕量 / dB", noiseMargin_));
}

void DetectionCard::setupStrategy(QVBoxLayout* layout) {
  layout->addWidget(new QLabel("互斥时间策略", this));
  auto* row = new QWidget(this);
  row->setObjectName("detectionTimeStrategy");
  auto* buttons = new QHBoxLayout(row);
  buttons->setContentsMargins(2, 2, 2, 2);
  buttons->setSpacing(2);
  strategies_ = new QButtonGroup(this);
  strategies_->setExclusive(true);
  struct StrategyChoice {
    DetectionTimeStrategy strategy;
    const char* label;
    const char* name;
  };
  const StrategyChoice choices[]{
      {DetectionTimeStrategy::Sample, "sample", "detectionStrategySample"},
      {DetectionTimeStrategy::Roi, "ROI", "detectionStrategyRoi"},
      {DetectionTimeStrategy::Coarse, "coarse", "detectionStrategyCoarse"}};
  for (const auto& [strategy, label, name] : choices) {
    auto* button = new QPushButton(QString::fromLatin1(label), row);
    button->setObjectName(name);
    button->setCheckable(true);
    strategies_->addButton(button, static_cast<int>(strategy));
    buttons->addWidget(button, 1);
  }
  layout->addWidget(row);
  splitBursts_ = new QCheckBox("允许同对象多段拆分", this);
  splitBursts_->setObjectName("detectionSplitBursts");
  layout->addWidget(splitBursts_);
  connect(strategies_, &QButtonGroup::idClicked, this, [this](int strategy) {
    configuration_.parameters.timeStrategy =
        static_cast<DetectionTimeStrategy>(strategy);
    saveConfiguration(configuration_);
  });
  connect(splitBursts_, &QCheckBox::toggled, this, [this](bool enabled) {
    if (updating_) return;
    configuration_.parameters.splitBursts = enabled;
    saveConfiguration(configuration_);
  });
}

void DetectionCard::setupBranches(QVBoxLayout* layout) {
  auto* body = new QWidget(this);
  auto* rows = new QVBoxLayout(body);
  rows->setContentsMargins(0, 0, 0, 0);
  rows->setSpacing(8);
  edgeFraction_ = addParameter("detectionSampleEdgeRatio", "边缘比例",
                               &DetectionParameters::sampleEdgeRatio, 1);
  smoothMilliseconds_ =
      addParameter("detectionSmoothMilliseconds", "平滑 / ms",
                   &DetectionParameters::smoothSeconds, 60, 1000);
  rows->addWidget(pairedFields("边缘比例", edgeFraction_, "平滑 / ms",
                               smoothMilliseconds_));
  minOnMilliseconds_ =
      addParameter("detectionMinOnMilliseconds", "min on / ms",
                   &DetectionParameters::minOnSeconds, 60, 1000);
  minOffMilliseconds_ =
      addParameter("detectionMinOffMilliseconds", "min off / ms",
                   &DetectionParameters::minOffSeconds, 60, 1000);
  rows->addWidget(pairedFields("min on / ms", minOnMilliseconds_,
                               "min off / ms", minOffMilliseconds_));
  splitGapMilliseconds_ =
      addParameter("detectionCoarseSplitGapMilliseconds", "粗分段间隔 / ms",
                   &DetectionParameters::coarseSplitGapSeconds, 60, 1000);
  minBurstMilliseconds_ =
      addParameter("detectionCoarseMinBurstMilliseconds", "最短粗事件 / ms",
                   &DetectionParameters::coarseMinBurstSeconds, 60, 1000);
  rows->addWidget(pairedFields("粗分段间隔 / ms", splitGapMilliseconds_,
                               "最短粗事件 / ms", minBurstMilliseconds_));
  branchHelp_ = wrappingLabel("detectionBranchHelp", body);
  rows->addWidget(branchHelp_);
  layout->addWidget(foldableFields("分支有效参数", body, true,
                                   "detectionBranchParameters", this));
}

void DetectionCard::setupAdvanced(QVBoxLayout* layout) {
  auto* body = new QWidget(this);
  auto* rows = new QVBoxLayout(body);
  rows->setContentsMargins(0, 0, 0, 0);
  rows->setSpacing(8);
  auto* batch = addUnsigned(
      "detectionBatchSize", "batch", 2, 1,
      [](const auto& value) { return value.parameters.batchSize; },
      [](auto& value, auto number) {
        value.parameters.batchSize = static_cast<std::uint32_t>(number);
      });
  auto* hits = addUnsigned(
      "detectionMinHits", "minHits", 10000, 1,
      [](const auto& value) { return value.parameters.minHits; },
      [](auto& value, auto number) {
        value.parameters.minHits = static_cast<std::uint32_t>(number);
      });
  rows->addWidget(pairedFields("batch", batch, "minHits", hits));
  auto* miss = addUnsigned(
      "detectionMaxMiss", "maxMiss", 10000, 0,
      [](const auto& value) { return value.parameters.maxMiss; },
      [](auto& value, auto number) {
        value.parameters.maxMiss = static_cast<std::uint32_t>(number);
      });
  auto* merge = addParameter("detectionMergeFrequencyHz", "merge / Hz",
                             &DetectionParameters::mergeFrequencyHz, 1e12);
  rows->addWidget(pairedFields("maxMiss", miss, "merge / Hz", merge));
  layout->addWidget(foldableFields("高级参数", body, false,
                                   "detectionAdvancedParameters", this));
}

void DetectionCard::setupStatus(QVBoxLayout* layout) {
  draftNote_ = wrappingLabel("detectionParameterDraftNote", this);
  layout->addWidget(draftNote_);
  preflight_ = wrappingLabel("detectionPreflight", this);
  layout->addWidget(preflight_);
  runSummary_ = wrappingLabel("detectionRunSummary", this);
  layout->addWidget(runSummary_);
  phaseBar_ = new QWidget(this);
  phaseBar_->setObjectName("detectionPhaseProgress");
  auto* phases = new QHBoxLayout(phaseBar_);
  phases->setContentsMargins(0, 0, 0, 0);
  phases->setSpacing(4);
  const std::array<QString, 5> phaseNames{"排队", "谱帧", "推理 / 关联",
                                          "精修 / 发布", "完成"};
  for (std::size_t index = 0; index < phaseSteps_.size(); ++index) {
    phaseSteps_[index] = new QLabel(phaseBar_);
    phaseSteps_[index]->setFixedHeight(3);
    phaseSteps_[index]->setToolTip(phaseNames[index]);
    phases->addWidget(phaseSteps_[index], 1);
  }
  phaseBar_->hide();
  layout->addWidget(phaseBar_);
  actions_ = new QWidget(this);
  actions_->setObjectName("detectionRunActions");
  auto* actions = new QHBoxLayout(actions_);
  actions->setContentsMargins(0, 8, 0, 0);
  actions->setSpacing(7);
  run_ = new QPushButton("▶ 运行检测", this);
  run_->setObjectName("runWidebandDetection");
  run_->setProperty("uiRole", "primary");
  cancel_ = new QPushButton("■ 取消任务", this);
  cancel_->setObjectName("cancelDetectionCardRun");
  actions->addWidget(run_, 1);
  actions->addWidget(cancel_);
  layout->addWidget(actions_);
  connect(run_, &QPushButton::clicked, this, &DetectionCard::submit);
  connect(cancel_, &QPushButton::clicked, this, [this] {
    if (const auto* run = unfinishedRun())
      emit cancelRunRequested(text(run->request.runId));
  });
}

void DetectionCard::rejectControl(QLineEdit* edit, const QString& error,
                                  const std::function<void()>& restore) {
  const auto draft = edit->text();
  const QSignalBlocker block(edit);
  restore();
  edit->setText(draft);
  edit->setProperty("parameterDraft", true);
  edit->setProperty("detectionCommitError", error);
  edit->setProperty("invalid", true);
  refreshStyle(edit);
  edit->setToolTip(error);
  emit message(error);
  refreshStatus();
}

void DetectionCard::saveConfiguration(DetectionRequest value) {
  configuration_ = std::move(value);
  profileConfigurations_[configuration_.profile.id] = configuration_;
  refreshTiming();
  refreshBranchState();
  refreshStatus();
}

void DetectionCard::switchProfile() {
  const auto id = profile_->currentData().toString().toStdString();
  const auto found = profileConfigurations_.find(id);
  if (found == profileConfigurations_.end()) return;
  profileConfigurations_[configuration_.profile.id] = configuration_;
  for (auto* widget : findChildren<QWidget*>())
    if (auto* draft = dynamic_cast<ParameterDraft*>(widget)) draft->discard();
  configuration_ = found->second;
  loadControls();
  refresh();
}

void DetectionCard::loadControls() {
  updating_ = true;
  const QSignalBlocker profileBlock(profile_), rangeBlock(rangeKind_),
      splitBlock(splitBursts_);
  profile_->setCurrentIndex(
      profile_->findData(text(configuration_.profile.id)));
  rangeKind_->setCurrentIndex(
      rangeKind_->findData(text(configuration_.rangeOrigin)));
  for (const auto& parameter : parameters_) {
    parameter.edit->discard();
    parameter.edit->setValue(configuration_.parameters.*parameter.member *
                             parameter.scale);
  }
  for (const auto& control : unsignedControls_)
    control.edit->setValue(control.getter(configuration_));
  hopMilliseconds_->discard();
  hopMilliseconds_->setValue(configuration_.profile.hopSeconds * 1000);
  if (auto* button = strategies_->button(
          static_cast<int>(configuration_.parameters.timeStrategy)))
    button->setChecked(true);
  splitBursts_->setChecked(configuration_.parameters.splitBursts);
  for (auto* edit : findChildren<QLineEdit*>()) {
    edit->setProperty("detectionCommitError", QString{});
    edit->setProperty("invalid", false);
    refreshStyle(edit);
  }
  updating_ = false;
}

void DetectionCard::refreshSource() {
  const auto* file = session_.activeFile();
  SourceIdentity source;
  if (file)
    source =
        annotationSourceIdentity(file->metadata, session_.projectGeneration());
  // Loading progress changes the available prefix, not the draft context.
  // Actual source/format, project, file and channel switches discard drafts.
  const QString key =
      QString("%1:%2:%3:%4:%5:%6:%7:%8:%9")
          .arg(session_.projectGeneration())
          .arg(text(source.fileId))
          .arg(text(session_.project().activeChannelId))
          .arg(text(source.path))
          .arg(text(sampleFormatKey(source.sampleFormat)))
          .arg(QString::number(source.sampleRateHz, 'g', 17))
          .arg(QString::number(source.centerFrequencyHz, 'g', 17))
          .arg(text(source.quickFingerprint))
          .arg(source.physicalSamples);
  if (key != contextKey_) {
    inputPolicy_->setContext(key);
    contextKey_ = key;
    displayRunId_.clear();
    requestedRoi_.reset();
    segmentAnnotationId_.clear();
    segmentRevisionId_.clear();
    for (auto& entry : profileConfigurations_) entry.second.parentRunId.reset();
    configuration_.parentRunId.reset();
    loadControls();
  }
  source_ = std::move(source);
  metadata_ = file ? std::optional<FileMetadata>(file->metadata) : std::nullopt;
}

const Annotation* DetectionCard::selectedAnnotation() const {
  const auto* file = session_.activeFile();
  if (!file || file->metadata.id != source_.fileId) return nullptr;
  const auto* annotation = findAnnotation(*file, file->activeMarkId);
  return annotation && !annotation->deleted ? annotation : nullptr;
}

const Segment* DetectionCard::selectedSegment() const {
  const auto* annotation = selectedAnnotation();
  if (!annotation) return nullptr;
  const auto id = activeSegment_->currentData().toString().toStdString();
  const auto found =
      std::find_if(annotation->segments.begin(), annotation->segments.end(),
                   [&](const auto& segment) { return segment.id == id; });
  return found == annotation->segments.end() ? nullptr : &*found;
}

void DetectionCard::refreshSegments() {
  const auto* annotation = selectedAnnotation();
  const auto* file = session_.activeFile();
  const std::string annotationId = annotation ? annotation->id : std::string{};
  const std::string revisionId =
      annotation ? annotation->revisionId : std::string{};
  const QSignalBlocker block(activeSegment_);
  if (annotationId != segmentAnnotationId_ ||
      revisionId != segmentRevisionId_) {
    activeSegment_->clear();
    if (annotation)
      for (std::size_t index = 0; index < annotation->segments.size();
           ++index) {
        const auto& segment = annotation->segments[index];
        activeSegment_->addItem(
            QString("活动段 %1 · %2")
                .arg(index + 1)
                .arg(rangeText(rfSegmentRange(segment, source_),
                               source_.sampleRateHz)
                         .replace('\n', " · ")),
            text(segment.id));
      }
    if (!activeSegment_->count()) activeSegment_->addItem("没有定位段", "");
    segmentAnnotationId_ = annotationId;
    segmentRevisionId_ = revisionId;
  }
  if (annotation && file) {
    auto id = file->activeSegmentId;
    if (id.empty()) id = annotation->activeSegmentId;
    const int index = activeSegment_->findData(text(id));
    if (index >= 0) activeSegment_->setCurrentIndex(index);
  }
  activeSegment_->setVisible(configuration_.rangeOrigin == "active-segment");
}

ViewRange DetectionCard::availableRange() const {
  return {{0, source_.availableSamples},
          {source_.centerFrequencyHz - source_.sampleRateHz / 2,
           source_.centerFrequencyHz + source_.sampleRateHz / 2}};
}

DetectionRequest DetectionCard::request() const {
  auto value = configuration_;
  value.runId.clear();
  value.sourceIdentity = source_;
  value.sourceAnnotationId.clear();
  value.sourceRevisionId.clear();
  value.requestedRange = {};
  if (value.rangeOrigin == "available-file")
    value.requestedRange = availableRange();
  if (value.rangeOrigin == "visible") {
    const auto* file = session_.activeFile();
    if (file && file->metadata.id == source_.fileId)
      value.requestedRange = file->view;
  }
  if (value.rangeOrigin == "custom" && requestedRoi_)
    value.requestedRange = *requestedRoi_;
  if (value.rangeOrigin == "active-segment") {
    if (const auto* segment = selectedSegment()) {
      value.requestedRange = rfSegmentRange(*segment, source_);
      if (const auto* annotation = selectedAnnotation()) {
        value.sourceAnnotationId = annotation->id;
        value.sourceRevisionId = annotation->revisionId;
      }
    }
  }
  value.contextRange = availableRange();
  if (metadata_) {
    DetectionSourcePlan plan;
    QString error;
    if (makeDetectionSourcePlan(*metadata_, value.requestedRange,
                                sourceConfig(value), plan, error,
                                source_.projectGeneration))
      value.contextRange.time = {
          std::min(value.requestedRange.time.begin, plan.contextRange.begin),
          std::max(value.requestedRange.time.end, plan.contextRange.end)};
  }
  return value;
}

void DetectionCard::refreshTiming() {
  const auto& profile = configuration_.profile;
  if (profile.backend == DetectionBackendKind::Scn)
    profileHelp_->setText(
        "SCN：16 帧最大积累，网络单行 32768 · 频率对照。"
        "CNR / 频率收边固定 3 dB；Coarse / 粗回退固定 6 dB。"
        "噪底裕量不可调，已保存的配置值保留。");
  else if (profile.backend == DetectionBackendKind::Experimental17)
    profileHelp_->setText(
        "17 是频率阶数，固定 10 真实帧 / N 8192–131072 · 实验");
  else if (profile.backend == DetectionBackendKind::Fixture)
    profileHelp_->setText(
        "系统链路夹具：真实 IQ / DSP / "
        "文件系统任务；不执行真实模型，不证明检测效果");
  else
    profileHelp_->setText(
        "10 帧全阵列 Z-score · epoch-1200 IQ 能力配置；模型与资产预检独立");
  if (source_.sampleRateHz <= 0 || !std::isfinite(source_.sampleRateHz)) {
    physicalTimes_->setText("FFT 窗 / 谱步长 / 模型上下文 / 窗口滑动：—");
    return;
  }
  const double fftMs = profile.fftSize / source_.sampleRateHz * 1000;
  const double hopMs = std::floor(source_.sampleRateHz * profile.hopSeconds) /
                       source_.sampleRateHz * 1000;
  const double contextMs = fftMs + (profile.modelFrames - 1) * hopMs;
  physicalTimes_->setText(
      QString("<table width='100%' cellspacing='5'><tr><td>FFT 窗<br><b>%1 "
              "ms</b></td>"
              "<td>谱步长<br><b>%2 ms</b></td></tr><tr><td>模型上下文<br>"
              "<b>%3 帧 / %4 ms</b></td><td>窗口滑动<br><b>%5 帧 / %6 "
              "ms</b></td></tr></table>"
              "上下文标称 %7 ms，观测支持含 FFT 窗。hop "
              "是相邻谱帧间隔，不是独立时间分辨率。")
          .arg(number(fftMs))
          .arg(number(hopMs))
          .arg(profile.modelFrames)
          .arg(number(contextMs))
          .arg(profile.modelHopFrames)
          .arg(number(profile.modelHopFrames * hopMs))
          .arg(number(profile.modelFrames * hopMs)));
}

void DetectionCard::refreshBranchState() {
  const auto strategy = configuration_.parameters.timeStrategy;
  const bool fine = strategy == DetectionTimeStrategy::Sample ||
                    strategy == DetectionTimeStrategy::Roi;
  const bool split = configuration_.parameters.splitBursts;
  edgeFraction_->setEnabled(fine);
  smoothMilliseconds_->setEnabled(fine);
  minOnMilliseconds_->setEnabled(fine && split);
  minOffMilliseconds_->setEnabled(fine && split);
  splitGapMilliseconds_->setEnabled(strategy == DetectionTimeStrategy::Coarse &&
                                    split);
  minBurstMilliseconds_->setEnabled(strategy == DetectionTimeStrategy::Coarse &&
                                    split);
  const bool scn = configuration_.profile.backend == DetectionBackendKind::Scn;
  noiseMargin_->setEnabled(!scn && strategy != DetectionTimeStrategy::Coarse);
  if (strategy == DetectionTimeStrategy::Sample)
    branchHelp_->setText(
        "sample：带限功率门控；边缘比例 / 平滑生效。允许拆分时 min on/off "
        "生效；粗分段参数不生效。");
  else if (strategy == DetectionTimeStrategy::Roi)
    branchHelp_->setText(
        scn ? "ROI：ROI STFT 自适应门限；边缘比例 / 平滑生效。允许拆分时 "
              "min on/off 生效；SCN 裕量固定。"
            : "ROI：ROI STFT / 噪底轨迹；边缘比例 / 平滑与噪底裕量生效。"
              "允许拆分时 min on/off 生效。");
  else
    branchHelp_->setText(
        "coarse：窗口支持 / 粗事件。精修参数不生效；允许拆分时粗分段间隔 / "
        "最短事件生效。");
}

bool DetectionCard::hasDraft() const {
  for (auto* widget : findChildren<QWidget*>())
    if (widget->property("parameterDraft").toBool()) return true;
  return false;
}

const DetectionRun* DetectionCard::latestRun() const {
  const auto* file = session_.activeFile();
  if (file && !displayRunId_.empty())
    for (const auto& run : file->detectionRuns)
      if (run.request.runId == displayRunId_) return &run;
  return file && !file->detectionRuns.empty() ? &file->detectionRuns.back()
                                              : nullptr;
}

const DetectionRun* DetectionCard::unfinishedRun() const {
  for (const auto& file : session_.project().files)
    for (const auto& run : file.detectionRuns)
      if (!detectionIsTerminal(run.lifecycle)) return &run;
  return nullptr;
}

void DetectionCard::refreshStatus() {
  if (!preflight_) return;
  const auto* activeRun = unfinishedRun();
  draftNote_->setText(
      hasDraft()
          ? QString("草稿待提交：Enter / Tab / 应用内点击提交。%1")
                .arg(activeRun ? "用于下一次，本次运行快照不变。"
                               : "输入中刷新不覆盖草稿。")
          : QString("Enter / Tab / "
                    "应用内点击提交。应用失焦不应用，刷新不覆盖草稿。%1")
                .arg(activeRun ? "参数用于下一次，本次运行快照不变。" : ""));
  const auto value = request();
  rangeSummary_->setText(
      value.requestedRange.time.begin < value.requestedRange.time.end
          ? rangeText(value.requestedRange, source_.sampleRateHz)
          : "尚未提交范围");
  const auto issue = validationError();
  preflight_->setText(
      issue.isEmpty()
          ? QString(
                "配置预检可运行。只读必要上下文：%1\n输出限定：%2。频率 ROI "
                "仅筛选输出，模型输入保持全 Fs 频带。来源指纹由任务预检核验。")
                .arg(rangeText(value.contextRange, source_.sampleRateHz),
                     rangeText(value.requestedRange, source_.sampleRateHz))
          : issue);
  if (preflight_->property("preflightOk").toBool() != issue.isEmpty()) {
    preflight_->setProperty("preflightOk", issue.isEmpty());
    refreshStyle(preflight_);
  }
  run_->setEnabled(issue.isEmpty() && !activeRun);
  cancel_->setEnabled(activeRun &&
                      activeRun->lifecycle != DetectionLifecycle::Cancelling);
  cancel_->setToolTip(activeRun ? "取消运行 " + text(activeRun->request.runId)
                                : "没有未完成任务");
  selectRoi_->setEnabled(metadata_ && availableSamples(*metadata_) > 0);
  if (const auto* run = latestRun()) {
    const auto phase = text(run->computationPhase);
    const int step = phase == "completed"                           ? 4
                     : phase == "refine" || phase == "publishing"   ? 3
                     : phase == "inference" || phase == "frequency" ? 2
                     : phase == "reading"                           ? 1
                                                                    : 0;
    phaseBar_->show();
    if (phaseBar_->property("completedStageIndex") != QVariant(step)) {
      phaseBar_->setProperty("completedStageIndex", step);
      for (std::size_t index = 0; index < phaseSteps_.size(); ++index)
        phaseSteps_[index]->setStyleSheet(static_cast<int>(index) <= step
                                              ? "background: #61c3ca;"
                                              : "background: #26465d;");
    }
    runSummary_->setText(
        QString("%1 · %2\n冻结范围：%3\n实际谱帧 %4 · unique windows %5/%6 · "
                "持久化记录 %7\n%8%9")
            .arg(lifecycleText(run->lifecycle), text(run->request.runId),
                 rangeText(run->request.requestedRange,
                           run->request.sourceIdentity.sampleRateHz))
            .arg(run->readFrames)
            .arg(run->completedWindows)
            .arg(run->eligibleWindows)
            .arg(run->resultCount)
            .arg(run->coverage.empty()
                     ? "尚未发布覆盖；请求范围尚无完整结论。"
                     : "实际覆盖见后台任务详情；未读与排除区域没有检测结论。")
            .arg(run->error.empty() ? "" : "\n" + text(run->error)) +
        QString("\n阶段 %1 · 精修候选处理 %2/%3")
            .arg(detectionPhaseText(run->computationPhase))
            .arg(run->refinedCandidates)
            .arg(run->totalRefinementCandidates));
  } else {
    phaseBar_->hide();
    runSummary_->setText(
        "尚无本次任务；三个检测入口只打开此配置卡。" +
        QString(activeRun ? "本工程已有未完成运行，新参数用于下一次。" : ""));
  }
}

void DetectionCard::refresh() {
  refreshSource();
  refreshSegments();
  refreshTiming();
  refreshBranchState();
  refreshStatus();
}

QString DetectionCard::configurationError(const DetectionRequest& value) const {
  const auto& profile = value.profile;
  if (!findDetectionProfile(profile.id)) return "请选择有效能力方案";
  if (!profile.fftSize || profile.fftSize > 1048576 || !profile.modelFrames ||
      profile.modelFrames > 1048576 || !profile.modelHopFrames ||
      profile.modelHopFrames > profile.modelFrames)
    return "FFT N / 模型帧须为有效整数，窗口滑动帧数不能超过模型帧数";
  if (!std::isfinite(profile.hopSeconds) || profile.hopSeconds <= 0 ||
      profile.hopSeconds > 100)
    return "谱步长须为大于 0 且不超过 100000 ms 的有限数";
  if (source_.sampleRateHz > 0 &&
      std::floor(source_.sampleRateHz * profile.hopSeconds) < 1)
    return "谱步长必须至少包含 1 个源样本";
  if ((profile.backend == DetectionBackendKind::TensorRt ||
       profile.backend == DetectionBackendKind::Fixture) &&
      (profile.fftSize != 65536 || profile.modelFrames != 10))
    return "FFSCN GUI IQ 固定 10 × 65536；非法形状未提交";
  if (profile.backend == DetectionBackendKind::Scn &&
      (profile.fftSize != 32768 || profile.modelFrames != 16))
    return "SCN 固定 16 帧最大积累 / 32768；非法形状未提交";
  if (profile.backend == DetectionBackendKind::Experimental17 &&
      (profile.modelFrames != 10 || profile.fftSize < 8192 ||
       profile.fftSize > 131072 ||
       (profile.fftSize & (profile.fftSize - 1)) != 0))
    return "FFSCN-17 固定 10 帧 / N 8192–131072 的 2 次幂；非法形状未提交";
  for (const auto& parameter : parameters_) {
    const double number = value.parameters.*parameter.member;
    if (!std::isfinite(number) || number < 0 || number > parameter.maximum)
      return parameter.label + QString("须为 0–%1 的有限数")
                                   .arg(parameter.maximum * parameter.scale);
  }
  const auto& p = value.parameters;
  if (!p.topK || p.topK > 512) return "topK 须为 1–512";
  if (!p.batchSize || p.batchSize > 2) return "batch 须为 1–2";
  if (!p.minHits || p.minHits > 10000 || p.maxMiss > 10000)
    return "minHits 须为 1–10000，maxMiss 须为 0–10000";
  if (source_.sampleRateHz > 0 && p.mergeFrequencyHz > source_.sampleRateHz)
    return "merge 频差不能超过源采样率";
  if (p.timeStrategy != DetectionTimeStrategy::Sample &&
      p.timeStrategy != DetectionTimeStrategy::Roi &&
      p.timeStrategy != DetectionTimeStrategy::Coarse)
    return "请选择 sample / ROI / coarse 之一";
  return {};
}

QString DetectionCard::rangeError(const DetectionRequest& value) const {
  if (value.rangeOrigin == "custom" && !requestedRoi_)
    return "请显式选择并确认一个 ROI";
  if (value.rangeOrigin == "active-segment" && !selectedSegment())
    return "活动对象无定位段；时间未知的观察频带不能作为标记 "
           "ROI，请选单个有效段";
  if (value.rangeOrigin == "active-segment" &&
      (value.sourceAnnotationId.empty() || value.sourceRevisionId.empty()))
    return "活动段缺少可冻结的 annotation / revision ID";
  const auto bounds = availableRange();
  const auto& range = value.requestedRange;
  if (range.time.begin >= range.time.end || range.time.end > bounds.time.end)
    return "请求越过可用前缀或时间范围为空；不会截断标签，请另选有效范围";
  if (!std::isfinite(range.frequency.lowerHz) ||
      !std::isfinite(range.frequency.upperHz) ||
      range.frequency.lowerHz >= range.frequency.upperHz ||
      range.frequency.lowerHz < bounds.frequency.lowerHz ||
      range.frequency.upperHz > bounds.frequency.upperHz)
    return "请求 RF 频带须为全 Fs 物理频带内有序、非空的有限范围";
  return {};
}

QString DetectionCard::sourceValidationError() const {
  const auto* file = session_.activeFile();
  if (!file || !metadata_ || projectDirectory_().isEmpty())
    return "无工程 / 信号，检测不可用；请先创建或保存工程并打开真实文件";
  if (source_.projectGeneration != session_.projectGeneration() ||
      file->metadata.id != source_.fileId ||
      file->metadata.path != source_.path ||
      file->metadata.sampleFormat != source_.sampleFormat ||
      file->metadata.sampleRateHz != source_.sampleRateHz ||
      file->metadata.centerFrequencyHz != source_.centerFrequencyHz)
    return "工程、文件或来源已切换，请刷新配置；旧草稿不能启动";
  if (metadata_->demo)
    return "检测需要真实 IQ 来源；演示图谱不能产生真实算法结果";
  if (metadata_->availability.status == LoadStatus::Loading)
    return "Loading：源仍在载入，请等待可用前缀就绪";
  if (metadata_->availability.status == LoadStatus::Failed)
    return "源加载失败：" + text(metadata_->availability.error);
  if (!QFileInfo(text(source_.path)).isFile())
    return "源文件缺失；已有标记可查看，任务不可启动";
  if (!source_.availableSamples ||
      source_.availableSamples > source_.physicalSamples ||
      !std::isfinite(source_.sampleRateHz) || source_.sampleRateHz <= 0 ||
      !std::isfinite(source_.centerFrequencyHz))
    return "来源样本数 / Fs / Fc 无效或没有可用前缀";
  if (source_.sampleFormat.structure != SampleStructure::ComplexIQ)
    return "此能力方案只接受复数 IQ，原始标签与手动标记仍可使用";
  return {};
}

QString DetectionCard::profileValidationError(
    const DetectionRequest& value) const {
  const auto* registered = findDetectionProfile(value.profile.id);
  if (!registered || value.profile.fftSize != registered->fftSize ||
      value.profile.modelFrames != registered->modelFrames ||
      value.profile.modelHopFrames != registered->modelHopFrames ||
      value.profile.hopSeconds != registered->hopSeconds)
    return "当前形状 / "
           "谱步长与本节点注册能力配置不符，不能启动；请恢复该方案已验证配置";
  return {};
}

QString DetectionCard::contextValidationError(
    const DetectionRequest& value) const {
  const auto hop = std::floor(source_.sampleRateHz * value.profile.hopSeconds);
  const long double support =
      value.profile.fftSize +
      static_cast<long double>(value.profile.modelFrames - 1) * hop;
  if (static_cast<long double>(source_.availableSamples) < support)
    return QString(
               "上下文不足：可用文件 %1 ms < %2 ms 观测支持；不重复谱帧补齐")
        .arg(number(static_cast<double>(source_.availableSamples) /
                    source_.sampleRateHz * 1000))
        .arg(
            number(static_cast<double>(support) / source_.sampleRateHz * 1000));
  if (value.profile.backend != DetectionBackendKind::Fixture) {
    auto snapshot = value;
    QString issue;
    if (!freezeDetectionModelSnapshot(snapshot, issue)) return issue;
  } else if (!qApp->property("enableDetectionTestFixture").toBool())
    return "系统链路夹具仅在显式测试模式开放，不能代替真实模型";
  DetectionSourcePlan plan;
  QString planError;
  if (!makeDetectionSourcePlan(*metadata_, value.requestedRange,
                               sourceConfig(value), plan, planError,
                               source_.projectGeneration))
    return "来源预检失败：" + planError;
  if (!plan.eligibleWindowCount)
    return "请求范围没有完整真实模型窗口，不能重复谱帧补齐";
  const auto payloadBytes =
      static_cast<std::uint64_t>(value.parameters.batchSize) *
      value.profile.modelFrames * value.profile.fftSize * sizeof(float);
  if (plan.estimatedPeakBytes + 6 * payloadBytes + 2 * 33554432ULL +
          2 * 65536ULL >
      value.workingSetBudgetBytes)
    return "工作集预算不足以容纳真实 DSP、批量输入和 worker 传输";
  return {};
}

QString DetectionCard::validationError() const {
  for (auto* edit : findChildren<QLineEdit*>())
    if (edit->property("parameterDraft").toBool() &&
        !edit->property("detectionCommitError").toString().isEmpty())
      return edit->property("detectionCommitError").toString();
  if (const auto issue = sourceValidationError(); !issue.isEmpty())
    return issue;
  const auto value = request();
  if (const auto issue = configurationError(value); !issue.isEmpty())
    return issue;
  if (const auto issue = rangeError(value); !issue.isEmpty()) return issue;
  if (const auto issue = profileValidationError(value); !issue.isEmpty())
    return issue;
  return contextValidationError(value);
}

void DetectionCard::configure(const DetectionRun* parentRun) {
  refresh();
  if (!parentRun) return;
  if (parentRun->request.sourceIdentity.fileId != source_.fileId) {
    emit message("派生运行需要先打开其源文件；当前配置草稿保持");
    return;
  }
  const auto found = profileConfigurations_.find(parentRun->request.profile.id);
  if (found == profileConfigurations_.end() ||
      profile_->findData(text(parentRun->request.profile.id)) < 0) {
    emit message("派生快照的方案当前不可用，已保留配置草稿");
    return;
  }
  configuration_ = parentRun->request;
  configuration_.parentRunId = parentRun->request.runId;
  configuration_.runId.clear();
  // Explicit derivation copies the original range, independently of a later
  // view or annotation revision. Merely opening the card never does this.
  requestedRoi_ = parentRun->request.requestedRange;
  configuration_.rangeOrigin = "custom";
  profileConfigurations_[configuration_.profile.id] = configuration_;
  loadControls();
  refresh();
}

void DetectionCard::setRequestedRoi(ViewRange range) {
  refreshSource();
  requestedRoi_ = range;
  configuration_.rangeOrigin = "custom";
  profileConfigurations_[configuration_.profile.id] = configuration_;
  const QSignalBlocker block(rangeKind_);
  rangeKind_->setCurrentIndex(rangeKind_->findData("custom"));
  refresh();
}

void DetectionCard::focusRun(const QString& runId) {
  displayRunId_ = runId.toStdString();
  refreshStatus();
}

void DetectionCard::submit() {
  // Enter/Tab/application clicks are the only commit paths. Running also
  // commits any remaining drafts, then validates the complete next request.
  for (auto* widget : findChildren<QWidget*>())
    if (auto* draft = dynamic_cast<ParameterDraft*>(widget)) draft->commit();
  refresh();
  const auto issue = validationError();
  if (!issue.isEmpty()) {
    emit message(issue);
    return;
  }
  if (unfinishedRun()) {
    emit message("本工程已有未完成任务；当前参数用于下一次运行");
    return;
  }
  auto value = request();
  value.runId =
      "run-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
  emit requestSubmitted(value);
  refreshStatus();
}
}  // namespace signalstudio
