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
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStringList>
#include <QStyle>
#include <QThread>
#include <QToolButton>
#include <QSettings>
#include <QUuid>
#include <QWheelEvent>
#include <QVBoxLayout>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "infrastructure/detection_model_snapshot.h"
#include "infrastructure/detection_source.h"
#include "infrastructure/detection_task.h"
#include "ui/detection/detection_resource_options.h"
#include "ui/controls/adaptive_value_edit.h"
#include "ui/controls/unsigned_value_edit.h"
#include "ui/detection/detection_phase.h"
#include "ui/parameter_input.h"

namespace signalstudio {
namespace {
QString text(const std::string& value) { return QString::fromUtf8(value); }

std::uint64_t steadyClockNanoseconds() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

QString number(double value) { return QString::number(value, 'g', 9); }

bool testFixturesEnabled() {
  return qApp->property("enableDetectionTestFixture").toBool() ||
         qEnvironmentVariableIntValue("SS_ENABLE_DETECTION_TEST_FIXTURES") == 1;
}

QString preflightAdvice(const QString& issue) {
  if (issue.contains("Loading")) return "请等待数据加载完成";
  if (issue.contains("无工程")) return "请创建工程并打开真实 IQ 文件";
  if (issue.contains("演示")) return "请打开真实 IQ 文件";
  if (issue.contains("缺失") || issue.contains("加载失败"))
    return "请恢复源文件并重新加载";
  if (issue.contains("预算") || issue.contains("批量") ||
      issue.contains("分片") || issue.contains("报文"))
    return "请减小批量、FFT 或窗口帧数";
  if (issue.contains("切换")) return "请刷新来源后重新配置";
  if (issue.contains("只接受复数 IQ")) return "请选择受支持的复数 IQ 来源";
  if (issue.contains("模型包") || issue.contains("实验配置") ||
      issue.contains("方案身份") || issue.contains("有效能力方案"))
    return "请检查模型包或选择已验证模型";
  if (issue.contains("ROI") || issue.contains("请求") || issue.contains("观测"))
    return "请在可用前缀内重新选择有效范围";
  if (issue.contains("前缀") || issue.contains("样本数"))
    return "请重新加载并确认可用数据";
  if (issue.contains("IQ")) return "请选择受支持的复数 IQ 来源";
  return "请修正对应参数后提交";
}

std::string configurationKey(const DetectionRequest& value) {
  return value.profile.id +
         (value.spectrumMode == DetectionSpectrumMode::ModelDefault
              ? ""
              : ":" + std::to_string(static_cast<int>(value.spectrumMode)));
}

void refreshStyle(QWidget* widget) {
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}

DetectionSourceConfig sourceConfig(const DetectionRequest& request) {
  DetectionSourceConfig config;
  config.inputAdaptationVersion = request.inputAdaptationVersion;
  config.profileId = request.profile.id;
  config.profileVersion = request.profile.version;
  config.fftSize = request.profile.fftSize;
  config.modelFrames = request.profile.modelFrames;
  config.modelHopFrames = request.profile.modelHopFrames;
  config.hopSeconds = request.profile.hopSeconds;
  config.workingSetBudgetBytes = request.workingSetBudgetBytes;
  config.evidenceBudgetBytes = request.evidenceBudgetBytes;
  config.rawHeadBudgetBytes = request.rawHeadBudgetBytes;
  config.legacyCombinedRawHeads = request.legacyCombinedEvidence;
  config.maximumCandidates = request.parameters.topK;
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

class NoWheelCheckBox : public QCheckBox {
 public:
  using QCheckBox::QCheckBox;

 protected:
  void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

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
  // Expanding a section must not apply a pending parameter draft.
  toggle->setProperty("preserveParameterDrafts", true);
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

QString quickModelIssue(const DetectionRequest& value, bool fixturesEnabled) {
  if (value.profile.backend == DetectionBackendKind::Experimental17)
    return QStringLiteral(
        "实验配置的 131072 输入尚未通过原张量对照，不能作为正式检测运行");
  if (value.profile.backend == DetectionBackendKind::Fixture && !fixturesEnabled)
    return QStringLiteral(
        "系统链路夹具仅在显式测试模式开放，不能代替真实模型");
  return {};
}

struct TimingCopy {
  QString physical;
  QString details;
};

TimingCopy composeTimingCopy(double sampleRateHz, std::uint64_t availableSamples,
                             const DetectionProfile& profile,
                             const DetectionRequest* request,
                             const DetectionSourcePlan* plan,
                             const QString* planFailure) {
  TimingCopy copy;
  if (sampleRateHz <= 0 || !std::isfinite(sampleRateHz)) {
    copy.physical = QStringLiteral("观测时长：待载入真实 IQ");
    copy.details = QStringLiteral("FFT 窗 / 谱步长 / 模型上下文 / 窗口滑动：—");
    return copy;
  }
  const double fftMs =
      std::bit_ceil(profile.fftSize) / sampleRateHz * 1000;
  const double hopMs = std::floor(sampleRateHz * profile.hopSeconds) /
                       sampleRateHz * 1000;
  const double contextMs = fftMs + (profile.modelFrames - 1) * hopMs;
  copy.physical = QString("请求观测 %1 ms · 滑动 %2 ms")
                      .arg(number(contextMs))
                      .arg(number(profile.modelHopFrames * hopMs));
  copy.details =
      QString(
          "FFT 窗 %1 ms · 谱步长 %2 ms\n模型上下文（请求）%3 帧 / %4 ms\n"
          "窗口滑动 %5 帧 / %6 ms\n上下文标称 %7 ms，观测支持含 FFT 窗。"
          "hop 是相邻谱帧间隔，不是独立时间分辨率。")
          .arg(number(fftMs))
          .arg(number(hopMs))
          .arg(profile.modelFrames)
          .arg(number(contextMs))
          .arg(profile.modelHopFrames)
          .arg(number(profile.modelHopFrames * hopMs))
          .arg(number(profile.modelFrames * hopMs));
  if (plan) {
    const auto realSupport =
        std::min(plan->contextRange.end, availableSamples) -
        std::min(plan->contextRange.begin, availableSamples);
    copy.physical = QString("实际窗口 %1 帧 · 上下文支持 %2 ms\n滑动 %3 ms")
                        .arg(plan->actualWindowFrames)
                        .arg(number(realSupport / sampleRateHz * 1000))
                        .arg(number(profile.modelHopFrames * hopMs));
    copy.details +=
        QString(
            "\n实际窗口 %1 帧 · 计算FFT %2 点 · 模型输入 %3 点。"
            "\n实际上下文样本 [%4, %5)，谱帧步长 %6 样本。"
            "\n源边缘补零 %7 / %8 样本；谱槽补零 %9 / %10 样本。"
            "边缘补零按实际支持记录；插值不增加真实分辨率。")
            .arg(plan->actualWindowFrames)
            .arg(plan->transformBins)
            .arg(request ? detectionModelInputBins(*request) : profile.fftSize)
            .arg(plan->contextRange.begin)
            .arg(plan->contextRange.end)
            .arg(plan->hopSamples)
            .arg(plan->sourceLeadingPadding)
            .arg(plan->sourceTrailingPadding)
            .arg(plan->slotLeadingPadding)
            .arg(plan->slotTrailingPadding);
  } else if (planFailure)
    copy.details += QStringLiteral("\n来源计划：") + *planFailure;
  return copy;
}
}  // namespace

bool detectionSourcePlanRunsOffCallerThread(const QThread* schedulingThread,
                                            const QThread* workerThread) {
  return schedulingThread != nullptr && workerThread != nullptr &&
         schedulingThread != workerThread;
}

DetectionDiagnosticReport buildDetectionDiagnosticReport(
    const DetectionDiagnosticInput& input, const QThread* schedulingThread) {
  DetectionDiagnosticReport report;
  const auto* workerThread = QThread::currentThread();
  report.ranOffCallerThread =
      detectionSourcePlanRunsOffCallerThread(schedulingThread, workerThread);
  // ASSERT: makeDetectionSourcePlan for diagnostics runs only when
  // detectionSourcePlanRunsOffCallerThread(schedulingThread, current) is true.
  // A null scheduling thread is a direct test call. DetectionCard::refresh,
  // refreshStatus, and refreshTiming do not enter this function.
  if (schedulingThread != nullptr && !report.ranOffCallerThread) {
    Q_ASSERT_X(false, "buildDetectionDiagnosticReport",
               "source plan ran on the caller thread");
    return report;
  }

  const auto firstIssue = [&](const QString& blocking) {
    if (!input.draftIssue.isEmpty()) return input.draftIssue;
    if (!input.sourceIssue.isEmpty()) return input.sourceIssue;
    if (!input.configurationIssue.isEmpty()) return input.configurationIssue;
    if (!input.rangeIssue.isEmpty()) return input.rangeIssue;
    if (!input.profileIssue.isEmpty()) return input.profileIssue;
    return blocking;
  };
  const bool earlier = !input.draftIssue.isEmpty() ||
                       !input.sourceIssue.isEmpty() ||
                       !input.configurationIssue.isEmpty() ||
                       !input.rangeIssue.isEmpty() ||
                       !input.profileIssue.isEmpty();

  DetectionSourcePlan plan;
  QString planError;
  bool planOk = false;
  if (input.hasMetadata) {
    report.planInvoked = true;
    planOk = makeDetectionSourcePlan(input.metadata, input.request.requestedRange,
                                     sourceConfig(input.request), plan, planError,
                                     input.projectGeneration);
  }

  QString modelIssue;
  QStringList details;
  const auto addIssue = [&details](const QString& category, const QString& error) {
    details.append(category + QStringLiteral("：") +
                   (error.isEmpty() ? QStringLiteral("通过") : error));
  };
  if (input.request.profile.backend != DetectionBackendKind::Fixture) {
    auto snapshot = input.request;
    freezeDetectionModelSnapshot(snapshot, modelIssue);
    addIssue(QStringLiteral("模型包"), modelIssue);
  } else {
    addIssue(QStringLiteral("测试夹具"),
             input.fixturesEnabled
                 ? QString()
                 : QStringLiteral("未显式启用测试夹具"));
  }
  const auto quick = quickModelIssue(input.request, input.fixturesEnabled);
  if (modelIssue.isEmpty()) modelIssue = quick;

  QString resourceIssue;
  if (planOk) {
    auto budgeted = input.request;
    resourceIssue =
        applyDetectionEvidencePreflight(input.projectDirectory, plan, budgeted);
  }
  QString blocking;
  if (!earlier) {
    if (!modelIssue.isEmpty())
      blocking = modelIssue;
    else if (input.hasMetadata && !planOk)
      blocking = QStringLiteral("来源预检失败：") + planError;
    else if (planOk && !plan.eligibleWindowCount)
      blocking = QStringLiteral("请求范围没有可用的真实观测");
    else if (planOk && !resourceIssue.isEmpty())
      blocking = resourceIssue;
    else if (planOk) {
      std::uint64_t reserve = 0;
      if (!detectionExecutionBatchSize(plan, input.request.parameters.batchSize,
                                       reserve))
        blocking = QStringLiteral(
            "减少批量后，单窗口仍超出DSP、输入报文或观测证据分片预算");
    }
  }
  report.blockingIssue = blocking;

  auto context = input.request.contextRange;
  if (planOk) {
    context.time = {
        std::min(input.request.requestedRange.time.begin, plan.contextRange.begin),
        std::max(input.request.requestedRange.time.end, plan.contextRange.end)};
  }
  const auto requested =
      rangeText(input.request.requestedRange, input.sampleRateHz);
  const auto issue = firstIssue(blocking);
  report.preflightDetail =
      issue.isEmpty()
          ? QString(
                "配置预检可运行。只读必要上下文：%1\n输出限定：%2。频率 ROI "
                "仅筛选输出，模型输入保持全 Fs 频带。来源指纹由任务预检核验。")
                .arg(rangeText(context, input.sampleRateHz), requested)
          : issue;

  QStringList body{QStringLiteral("预检：") + report.preflightDetail};
  const auto addBody = [&body](const QString& category, const QString& error) {
    body.append(category + QStringLiteral("：") +
                (error.isEmpty() ? QStringLiteral("通过") : error));
  };
  addBody(QStringLiteral("来源"), input.sourceIssue);
  addBody(QStringLiteral("配置"), input.configurationIssue);
  addBody(QStringLiteral("范围"), input.rangeIssue);
  addBody(QStringLiteral("模型身份"), input.profileIssue);
  for (const auto& line : details) body.append(line);
  if (input.hasMetadata) {
    if (!planOk)
      addBody(QStringLiteral("来源计划"), planError);
    else {
      std::uint64_t reserve = 0;
      addBody(QStringLiteral("真实观测"),
              plan.eligibleWindowCount
                  ? QString()
                  : QStringLiteral("请求范围没有可用的真实观测"));
      addBody(QStringLiteral("观测与原始输出"), resourceIssue);
      addBody(QStringLiteral("资源预算"),
              detectionExecutionBatchSize(
                  plan, input.request.parameters.batchSize, reserve)
                  ? QString()
                  : QStringLiteral("单窗口超出 DSP、报文或观测证据预算"));
      if (plan.eligibleWindowCount && plan.modelHeadBytesPerWindow &&
          plan.eligibleWindowCount <=
              std::numeric_limits<std::uint64_t>::max() /
                  plan.modelHeadBytesPerWindow) {
        body.append(
            QString("原始输出%1：%2 窗 × %3 字节，合计 %4 字节；观测 JSON 预算 %5 字节")
                .arg(input.request.retainRawModelOutput ? QStringLiteral("保留")
                                                       : QStringLiteral("不保留"))
                .arg(plan.eligibleWindowCount)
                .arg(plan.modelHeadBytesPerWindow)
                .arg(plan.eligibleWindowCount * plan.modelHeadBytesPerWindow)
                .arg(input.request.evidenceBudgetBytes));
      }
      body.append(QString("可处理窗口 %1 · 读取上下文样本 [%2, %3)")
                      .arg(plan.eligibleWindowCount)
                      .arg(plan.contextRange.begin)
                      .arg(plan.contextRange.end));
      for (const auto& exclusion : plan.exclusions)
        body.append(QString("排除样本 [%1, %2)：%3")
                        .arg(exclusion.samples.begin)
                        .arg(exclusion.samples.end)
                        .arg(text(exclusion.reason)));
    }
  }
  if (input.hasRun) {
    body.append(
        QString("运行 %1 · 来源状态 %2\n覆盖：%3")
            .arg(text(input.runId),
                 QString::fromUtf8(detectionSourceValidityName(input.sourceValidity)),
                 input.hasCoverage
                     ? QStringLiteral(
                           "实际覆盖见底部任务详情；未读与排除区域没有检测结论。")
                     : QStringLiteral("尚未发布覆盖；请求范围尚无完整结论。")));
    if (input.includeCoverage)
      for (const auto& coverage : input.coverage)
        body.append(
            QString("%1：%2 · %3")
                .arg(QString::fromUtf8(detectionCoverageKindName(coverage.kind)),
                     rangeText(coverage.range, input.runSampleRateHz),
                     text(coverage.reason)));
    if (!input.runError.empty())
      body.append(QStringLiteral("运行错误：") + text(input.runError));
    body.append(
        QString("实际谱帧 %1 · 精修候选 %2/%3；不可变快照、设备、分片及技术 "
                "JSON 见底部任务详情。")
            .arg(input.readFrames)
            .arg(input.refinedCandidates)
            .arg(input.totalRefinementCandidates));
  }
  report.diagnostics = body.join('\n');

  const QString* failure = nullptr;
  if (input.hasMetadata && !planOk) failure = &planError;
  const auto timing =
      composeTimingCopy(input.sampleRateHz, input.availableSamples,
                        input.request.profile, &input.request,
                        planOk ? &plan : nullptr, failure);
  report.physicalTimes = timing.physical;
  report.calculationDetails = timing.details;
  return report;
}

struct DetectionDiagnosticWorker {
  using Delivery =
      std::function<void(std::uint64_t, DetectionDiagnosticReport)>;
  struct Job {
    std::uint64_t generation = 0;
    QThread* caller = nullptr;
    DetectionDiagnosticInput input;
  };
  struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Job> pending;
    bool stopped = false;
    bool accepting = false;
    QObject* target = nullptr;
    Delivery delivery;
  };

  DetectionDiagnosticWorker(QObject* target, Delivery delivery)
      : state_(std::make_shared<State>()) {
    state_->target = target;
    state_->delivery = std::move(delivery);
    state_->accepting = true;
  }
  DetectionDiagnosticWorker(const DetectionDiagnosticWorker&) = delete;
  DetectionDiagnosticWorker& operator=(const DetectionDiagnosticWorker&) = delete;
  ~DetectionDiagnosticWorker() {
    {
      std::lock_guard lock(state_->mutex);
      state_->accepting = false;
      state_->stopped = true;
      state_->pending.reset();
    }
    state_->wake.notify_all();
    // Do not join. A long source plan must not freeze window teardown. The
    // thread keeps state_ alive and will not post after accepting is false.
    if (thread_.joinable()) thread_.detach();
  }

  void submit(std::uint64_t generation, DetectionDiagnosticInput input,
              QThread* caller) {
    bool start = false;
    {
      std::lock_guard lock(state_->mutex);
      state_->pending = Job{generation, caller, std::move(input)};
      start = !thread_.joinable();
    }
    if (start) {
      auto state = state_;
      thread_ = std::thread([state] { loop(std::move(state)); });
    }
    state_->wake.notify_one();
  }

 private:
  static void loop(std::shared_ptr<State> state) {
    for (;;) {
      std::optional<Job> job;
      {
        std::unique_lock lock(state->mutex);
        state->wake.wait(lock, [&] {
          return state->stopped || state->pending.has_value();
        });
        if (state->stopped) return;
        job = std::move(state->pending);
        state->pending.reset();
      }
      if (!job || !job->caller) continue;
      DetectionDiagnosticReport report;
      try {
        report = buildDetectionDiagnosticReport(job->input, job->caller);
      } catch (...) {
        report.ranOffCallerThread = detectionSourcePlanRunsOffCallerThread(
            job->caller, QThread::currentThread());
        report.diagnostics = QStringLiteral("诊断详情生成失败");
      }
      if (!report.ranOffCallerThread) continue;
      std::lock_guard lock(state->mutex);
      if (!state->accepting || state->stopped || !state->target ||
          !state->delivery)
        return;
      const auto generation = job->generation;
      const auto delivery = state->delivery;
      QMetaObject::invokeMethod(
          state->target,
          [delivery, generation, report]() mutable {
            if (delivery) delivery(generation, std::move(report));
          },
          Qt::QueuedConnection);
    }
  }

  std::shared_ptr<State> state_;
  std::thread thread_;
};

DetectionCard::~DetectionCard() = default;

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
      "#detectionAnalysisParameters, #detectionPostprocessParameters, "
      "#detectionResourceParameters { border: 1px solid #2b4961; }"
      "#detectionPhysicalTimes, "
      "#detectionParameterDraftNote { color: #86a4ba; font-size: 10px; }"
      "#detectionPreflight { border-left: 2px solid #e6ba65; "
      "background: #262922; color: #dec99f; padding: 4px; }"
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
    if (profile.backend == DetectionBackendKind::TensorRt ||
        profile.backend == DetectionBackendKind::Scn) {
      for (int mode = 1; mode <= 4; ++mode) {
        auto selected = profileConfigurations_.at(profile.id);
        selected.spectrumMode = static_cast<DetectionSpectrumMode>(mode);
        profileConfigurations_.emplace(configurationKey(selected),
                                       std::move(selected));
      }
    }
  }
  configuration_ = profileConfigurations_.at(detectionProfiles().front().id);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(13, 6, 13, 8);
  layout->setSpacing(9);
  setupProfile(layout);
  setupRange(layout);
  setupThresholds(layout);
  setupStrategy(layout);
  physicalTimes_ = wrappingLabel("detectionPhysicalTimes", this);
  layout->addWidget(physicalTimes_);
  setupStatus(layout);
  setupTiming(layout);
  setupBranches(layout);
  setupAdvanced(layout);
  calculationDetails_ = wrappingLabel("detectionCalculationDetailsText", this);
  calculationDetails_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(foldableFields("计算详情", calculationDetails_, false,
                                   "detectionCalculationDetails", this));
  diagnostics_ = wrappingLabel("detectionDiagnostics", this);
  diagnostics_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  // Keep the public diagnostics toggle name independent of the text label.
  layout->addWidget(foldableFields("检测诊断详情", diagnostics_, false,
                                   "detectionDiagnosticsSection", this));
  diagnosticsToggle_ =
      findChild<QToolButton*>("detectionDiagnosticsSectionToggle");
  diagnosticsToggle_->setObjectName("detectionDiagnosticsToggle");
  connect(diagnosticsToggle_, &QToolButton::toggled, this, [this](bool open) {
    if (open) publishDiagnosticInputs(true);
  });
  calculationToggle_ =
      findChild<QToolButton*>("detectionCalculationDetailsToggle");
  connect(calculationToggle_, &QToolButton::toggled, this, [this](bool open) {
    if (open) publishDiagnosticInputs(true);
  });
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
  model_ = new QComboBox(this);
  model_->setObjectName("detectionModel");
  model_->setAccessibleName("检测模型");
  model_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  model_->addItem("FFSCN GUI IQ", "ffscn-gui-iq-v1");
  model_->addItem("SCN", "scn-max-psd-v1");
  model_->addItem("FFSCN-17 · 实验", "ffscn-17-experimental-v1");
  if (testFixturesEnabled())
    model_->addItem("系统链路夹具 · 测试 / 非模型", "pipeline-fixture-v1");
  spectrumMode_ = new QComboBox(this);
  spectrumMode_->setObjectName("detectionSpectrumMode");
  spectrumMode_->setAccessibleName("检测输入谱");
  spectrumMode_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  layout->addWidget(pairedFields("模型", model_, "输入谱", spectrumMode_));
  connect(model_, &QComboBox::currentIndexChanged, this, [this] {
    if (!updating_) switchProfile();
  });
  connect(spectrumMode_, &QComboBox::currentIndexChanged, this, [this] {
    if (!updating_) switchProfile();
  });
}

void DetectionCard::setupRange(QVBoxLayout* layout) {
  layout->addWidget(new QLabel("检测范围", this));
  rangeKind_ = new QComboBox(this);
  rangeKind_->setObjectName("detectionRangeOrigin");
  rangeKind_->setAccessibleName("检测范围");
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
    profileConfigurations_[configurationKey(configuration_)] = configuration_;
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
  edit->setProperty("parameterLabel", label);
  edit->setAccessibleName(label);
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
      setParameterError(edit, {});
      saveConfiguration(std::move(candidate));
    }
  };
  connect(edit, &QLineEdit::textEdited, this,
          [this, edit](const QString& draft) {
            edit->setProperty("detectionDraftText", draft);
            refreshStatus();
          });
  connect(edit, &QLineEdit::textChanged, this, [this, edit] {
    if (!edit->property("parameterDraft").toBool()) {
      edit->setProperty("detectionCommitError", QString{});
      setParameterError(edit, {});
    }
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
  edit->setProperty("parameterLabel", label);
  edit->setAccessibleName(label);
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
  connect(edit, &QLineEdit::textEdited, this,
          [this, edit](const QString& draft) {
            edit->setProperty("detectionDraftText", draft);
            refreshStatus();
          });
  connect(edit, &QLineEdit::textChanged, this, [this] {
    if (!updating_) refreshStatus();
  });
  return edit;
}

void DetectionCard::setupTiming(QVBoxLayout* layout) {
  auto* body = new QWidget(this);
  auto* rows = new QVBoxLayout(body);
  rows->setContentsMargins(0, 0, 0, 0);
  rows->setSpacing(8);
  auto* fft = addUnsigned(
      "detectionFftSize", "FFT N", 262144, 32,
      [](const auto& value) { return value.profile.fftSize; },
      [](auto& value, auto number) {
        value.profile.fftSize = static_cast<std::uint32_t>(number);
      });
  hopMilliseconds_ = new AdaptiveValueEdit(UnitKind::Scalar, 1, this);
  hopMilliseconds_->setObjectName("detectionHopMilliseconds");
  hopMilliseconds_->setProperty("parameterLabel", "谱步长");
  hopMilliseconds_->setAccessibleName("检测谱步长");
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
          [this](const QString& draft) {
            hopMilliseconds_->setProperty("detectionDraftText", draft);
            refreshStatus();
          });
  connect(hopMilliseconds_, &QLineEdit::textChanged, this, [this] {
    if (!updating_) refreshStatus();
  });
  rows->addWidget(pairedFields("FFT N", fft, "谱步长 / ms", hopMilliseconds_));
  auto* frames = addUnsigned(
      "detectionModelFrames", "模型帧数",
      std::numeric_limits<std::uint32_t>::max(), 1,
      [](const auto& value) { return value.profile.modelFrames; },
      [](auto& value, auto number) {
        value.profile.modelFrames = static_cast<std::uint32_t>(number);
      });
  auto* slide = addUnsigned(
      "detectionModelHopFrames", "滑动帧数",
      std::numeric_limits<std::uint32_t>::max(), 1,
      [](const auto& value) { return value.profile.modelHopFrames; },
      [](auto& value, auto number) {
        value.profile.modelHopFrames = static_cast<std::uint32_t>(number);
      });
  rows->addWidget(pairedFields("模型帧数 T", frames, "滑动 / 帧", slide));
  layout->addWidget(foldableFields("专业分析参数", body, false,
                                   "detectionAnalysisParameters", this));
}

void DetectionCard::setupThresholds(QVBoxLayout* layout) {
  auto* score =
      addParameter("detectionScoreThreshold", "置信阈值（score threshold）",
                   &DetectionParameters::scoreThreshold, 1);
  layout->addWidget(new QLabel("置信阈值", this));
  layout->addWidget(score);
}

void DetectionCard::setupStrategy(QVBoxLayout* layout) {
  layout->addWidget(new QLabel("时间定位", this));
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
    const char* help;
  };
  const StrategyChoice choices[]{
      {DetectionTimeStrategy::Sample, "样本能量", "detectionStrategySample",
       "Sample：带限样本功率门控，按真实样本定位时间。"},
      {DetectionTimeStrategy::Roi, "时频精修", "detectionStrategyRoi",
       "ROI：候选区域 STFT 精修；与检测范围的自定义 ROI 独立。"},
      {DetectionTimeStrategy::Coarse, "粗窗口", "detectionStrategyCoarse",
       "Coarse：按窗口实际观测支持定位粗事件。"}};
  for (const auto& [strategy, label, name, help] : choices) {
    auto* button = new QPushButton(QString::fromUtf8(label), row);
    button->setObjectName(name);
    button->setCheckable(true);
    button->setToolTip(QString::fromUtf8(help));
    button->setProperty("detectionStrategyHelp", button->toolTip());
    strategies_->addButton(button, static_cast<int>(strategy));
    buttons->addWidget(button, 1);
  }
  layout->addWidget(row);
  splitBursts_ = new QCheckBox("允许同对象多段拆分", this);
  splitBursts_->setObjectName("detectionSplitBursts");
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
  auto* nms = addParameter("detectionNmsIou", "重叠抑制（NMS IoU）",
                           &DetectionParameters::nmsIou, 1);
  auto* topK = addUnsigned(
      "detectionTopK", "候选上限（topK）", 512, 1,
      [](const auto& value) { return value.parameters.topK; },
      [](auto& value, auto number) {
        value.parameters.topK = static_cast<std::uint32_t>(number);
      });
  rows->addWidget(pairedFields("重叠抑制", nms, "候选上限", topK));
  noiseMargin_ = addParameter("detectionBandwidthMarginDb", "噪底裕量 / dB",
                              &DetectionParameters::bandwidthMarginDb, 100);
  marginFields_ = new QWidget(body);
  auto* marginLayout = new QVBoxLayout(marginFields_);
  marginLayout->setContentsMargins(0, 0, 0, 0);
  marginLayout->addWidget(new QLabel("噪底裕量 / dB", marginFields_));
  marginLayout->addWidget(noiseMargin_);
  rows->addWidget(marginFields_);
  rows->addWidget(splitBursts_);
  edgeFraction_ = addParameter("detectionSampleEdgeRatio", "边缘比例",
                               &DetectionParameters::sampleEdgeRatio, 1);
  smoothMilliseconds_ =
      addParameter("detectionSmoothMilliseconds", "平滑 / ms",
                   &DetectionParameters::smoothSeconds, 60, 1000);
  fineFields_ =
      pairedFields("边缘比例", edgeFraction_, "平滑 / ms", smoothMilliseconds_);
  rows->addWidget(fineFields_);
  minOnMilliseconds_ =
      addParameter("detectionMinOnMilliseconds", "min on / ms",
                   &DetectionParameters::minOnSeconds, 60, 1000);
  minOffMilliseconds_ =
      addParameter("detectionMinOffMilliseconds", "min off / ms",
                   &DetectionParameters::minOffSeconds, 60, 1000);
  fineSplitFields_ = pairedFields("最短持续 / ms", minOnMilliseconds_,
                                  "最短间断 / ms", minOffMilliseconds_);
  rows->addWidget(fineSplitFields_);
  splitGapMilliseconds_ =
      addParameter("detectionCoarseSplitGapMilliseconds", "粗分段间隔 / ms",
                   &DetectionParameters::coarseSplitGapSeconds, 60, 1000);
  minBurstMilliseconds_ =
      addParameter("detectionCoarseMinBurstMilliseconds", "最短粗事件 / ms",
                   &DetectionParameters::coarseMinBurstSeconds, 60, 1000);
  coarseSplitFields_ = pairedFields("粗分段间隔 / ms", splitGapMilliseconds_,
                                    "最短粗事件 / ms", minBurstMilliseconds_);
  rows->addWidget(coarseSplitFields_);
  branchHelp_ = wrappingLabel("detectionBranchHelp", body);
  branchHelp_->hide();
  layout->addWidget(foldableFields("时间精修与后处理", body, false,
                                   "detectionPostprocessParameters", this));
}

void DetectionCard::setupAdvanced(QVBoxLayout* layout) {
  auto* body = new QWidget(this);
  auto* rows = new QVBoxLayout(body);
  rows->setContentsMargins(0, 0, 0, 0);
  rows->setSpacing(8);
  auto* batch = addUnsigned(
      "detectionBatchSize", "batch", 16, 1,
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
  rows->addWidget(pairedFields("推理批量", batch, "最少命中", hits));
  auto* miss = addUnsigned(
      "detectionMaxMiss", "maxMiss", 10000, 0,
      [](const auto& value) { return value.parameters.maxMiss; },
      [](auto& value, auto number) {
        value.parameters.maxMiss = static_cast<std::uint32_t>(number);
      });
  auto* merge = addParameter("detectionMergeFrequencyHz", "merge / Hz",
                             &DetectionParameters::mergeFrequencyHz, 1e12);
  rows->addWidget(pairedFields("最多漏检", miss, "合并频差 / Hz", merge));
  const bool restoreIntermediate =
      qApp->property("uiStatePersistenceEnabled").toBool() &&
      QSettings().contains(QStringLiteral("detection/showIntermediateResults"));
  const bool showIntermediate =
      !restoreIntermediate ||
      QSettings().value(QStringLiteral("detection/showIntermediateResults"), true)
          .toBool();
  showIntermediate_ = new NoWheelCheckBox("显示中间结果", body);
  showIntermediate_->setObjectName("showIntermediateDetectionResults");
  showIntermediate_->setChecked(showIntermediate);
  showIntermediate_->setToolTip(
      "勾选后，检测进行中在时频图和检测结果中显示虚线中间框。"
      "取消勾选则进行中不显示。检测完成、失败、取消或中断后都会自动清除这些中间框。");
  rows->addWidget(showIntermediate_);
  connect(showIntermediate_, &QCheckBox::toggled, this, [this](bool shown) {
    if (qApp->property("uiStatePersistenceEnabled").toBool())
      QSettings().setValue(QStringLiteral("detection/showIntermediateResults"),
                           shown);
    emit intermediateResultsToggled(shown);
  });
  layout->addWidget(foldableFields("高级资源参数", body, false,
                                   "detectionResourceParameters", this));
}

void DetectionCard::setupStatus(QVBoxLayout* layout) {
  draftNote_ = wrappingLabel("detectionParameterDraftNote", this);
  setToolTip(
      "Enter / Tab / 应用内点击提交；窗口失焦不提交。刷新保留草稿，"
      "切换工程、文件或通道撤销草稿；参数仅用于下一次任务。");
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
  viewRunning_ = new QPushButton("查看正在运行任务", actions_);
  viewRunning_->setObjectName("viewRunningDetection");
  viewRunning_->setToolTip("定位到其他文件上的未完成任务，不会取消");
  viewRunning_->setAccessibleName("查看正在运行任务");
  viewRunning_->hide();
  otherRun_ = new QLabel(this);
  otherRun_->setObjectName("detectionOtherRun");
  otherRun_->setWordWrap(true);
  otherRun_->hide();
  layout->addWidget(otherRun_);
  actions->addWidget(run_, 1);
  actions->addWidget(cancel_);
  actions->addWidget(viewRunning_);
  // MainWindow reparents this widget into the fixed area outside propScroll.
  // Never insert it into the card's scrolling layout.
  actions_->hide();
  connect(run_, &QPushButton::clicked, this, &DetectionCard::submit);
  connect(cancel_, &QPushButton::clicked, this, [this] {
    if (const auto* run = focusedUnfinished())
      emit cancelRunRequested(text(run->request.runId));
  });
  connect(viewRunning_, &QPushButton::clicked, this, [this] {
    const auto* run = unfinishedRun();
    const auto* file = session_.activeFile();
    if (run &&
        (!file || run->request.sourceIdentity.fileId != file->metadata.id))
      emit focusRunningRunRequested(text(run->request.runId));
  });
}

void DetectionCard::rejectControl(QLineEdit* edit, const QString& error,
                                  const std::function<void()>& restore) {
  const auto draft = edit->property("detectionDraftText").isValid()
                         ? edit->property("detectionDraftText").toString()
                         : edit->text();
  const QSignalBlocker block(edit);
  restore();
  edit->setText(draft);
  edit->setProperty("parameterDraft", true);
  edit->setProperty("detectionCommitError", error);
  setParameterError(edit, error);
  emit message(error);
  refreshStatus();
}

void DetectionCard::saveConfiguration(DetectionRequest value) {
  configuration_ = std::move(value);
  emit configurationCommitted();
  profileConfigurations_[configurationKey(configuration_)] = configuration_;
  refreshTiming();
  refreshBranchState();
  refreshStatus();
}

void DetectionCard::switchProfile() {
  const auto id = model_->currentData().toString().toStdString();
  const auto* profile = findDetectionProfile(id);
  if (!profile) return;
  const bool selectable = profile->backend == DetectionBackendKind::TensorRt ||
                          profile->backend == DetectionBackendKind::Scn;
  const int mode = selectable ? spectrumMode_->currentData().toInt() : 0;
  const auto key = id + (mode ? ":" + std::to_string(mode) : "");
  const auto found = profileConfigurations_.find(key);
  if (found == profileConfigurations_.end()) return;
  profileConfigurations_[configurationKey(configuration_)] = configuration_;
  for (auto* widget : findChildren<QWidget*>())
    if (auto* draft = dynamic_cast<ParameterDraft*>(widget)) draft->discard();
  configuration_ = found->second;
  emit configurationCommitted();
  loadControls();
  refresh();
}

void DetectionCard::loadControls() {
  updating_ = true;
  const QSignalBlocker modelBlock(model_), spectrumBlock(spectrumMode_),
      rangeBlock(rangeKind_), splitBlock(splitBursts_);
  model_->setCurrentIndex(model_->findData(text(configuration_.profile.id)));
  spectrumMode_->clear();
  const bool scn = configuration_.profile.backend == DetectionBackendKind::Scn;
  const bool selectable =
      scn || configuration_.profile.backend == DetectionBackendKind::TensorRt;
  spectrumMode_->addItem(scn ? "模型默认（最大谱）" : "模型默认（逐帧）", 0);
  if (selectable) {
    const QStringList modes{"瞬时谱（窗末帧）", "平均谱", "最大谱", "最小谱"};
    for (int mode = 1; mode <= 4; ++mode)
      spectrumMode_->addItem(modes[mode - 1], mode);
  } else
    spectrumMode_->setItemText(0, "模型默认");
  spectrumMode_->setEnabled(selectable);
  spectrumMode_->setCurrentIndex(
      spectrumMode_->findData(static_cast<int>(configuration_.spectrumMode)));
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
    setParameterError(edit, {});
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

bool DetectionCard::showIntermediateResults() const {
  return showIntermediate_ && showIntermediate_->isChecked();
}

DetectionRequest DetectionCard::baseRequest() const {
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
  if (qApp->property("uiStatePersistenceEnabled").toBool())
    DetectionResourceOptions::load(projectDirectory_()).applyTo(value);
  return value;
}

DetectionRequest DetectionCard::request() const {
  // Explicit assembly for submit and callers of request(). Refresh uses
  // baseRequest() so a progress poll does not run the source plan here.
  auto value = baseRequest();
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
  QString help;
  if (profile.backend == DetectionBackendKind::Scn)
    help =
        "SCN：默认最大谱；积累使用实际帧，网络单行32768点，内部频率适配。"
        "CNR / 频率收边固定 3 dB；Coarse / 粗回退固定 6 dB。"
        "噪底裕量不可调，已保存的配置值保留。";
  else if (profile.backend == DetectionBackendKind::Experimental17)
    help = "17 是频率阶数，固定 10 真实帧 / N 8192–131072 · 实验";
  else if (profile.backend == DetectionBackendKind::Fixture)
    help =
        "系统链路夹具：真实 IQ / DSP / 文件系统任务；"
        "不执行真实模型，不证明检测效果";
  else
    help =
        "默认逐帧Z-score；时间帧可变，模型输入为至少2048点的2次幂，内部适配。";
  if (configuration_.spectrumMode != DetectionSpectrumMode::ModelDefault)
    help +=
        " 当前输入：" +
        QStringList{"", "瞬时谱（窗末帧）", "平均谱（线性功率）", "最大谱",
                    "最小谱"}[static_cast<int>(configuration_.spectrumMode)] +
        "。";
  model_->setToolTip(help);
  spectrumMode_->setToolTip(help);
  if (source_.sampleRateHz <= 0 || !std::isfinite(source_.sampleRateHz)) {
    appliedTimingKey_.clear();
    const auto timing =
        composeTimingCopy(source_.sampleRateHz, source_.availableSamples,
                          profile, nullptr, nullptr, nullptr);
    physicalTimes_->setText(timing.physical);
    calculationDetails_->setText(timing.details);
    physicalTimes_->setToolTip(timing.details);
    return;
  }
  const auto key = currentTimingKey();
  // Progress ticks share this key. Keep the last plan-backed copy until a
  // profile or rate change, or until a diagnostic generation replaces it.
  if (key == appliedTimingKey_) return;
  appliedTimingKey_ = key;
  const auto timing =
      composeTimingCopy(source_.sampleRateHz, source_.availableSamples, profile,
                        nullptr, nullptr, nullptr);
  physicalTimes_->setText(timing.physical);
  calculationDetails_->setText(timing.details);
  physicalTimes_->setToolTip(timing.details);
}

void DetectionCard::refreshBranchState() {
  const auto strategy = configuration_.parameters.timeStrategy;
  const bool fine = strategy == DetectionTimeStrategy::Sample ||
                    strategy == DetectionTimeStrategy::Roi;
  const bool split = configuration_.parameters.splitBursts;
  const auto discardInactive = [](QWidget* control, bool active) {
    if (!active && control->property("parameterDraft").toBool())
      if (auto* draft = dynamic_cast<ParameterDraft*>(control)) draft->discard();
  };
  // Changing the strategy is an explicit context change for these fields.
  // Irrelevant hidden drafts must not block the new strategy's run action.
  for (auto* edit : {edgeFraction_, smoothMilliseconds_}) discardInactive(edit, fine);
  for (auto* edit : {minOnMilliseconds_, minOffMilliseconds_}) discardInactive(edit, fine && split);
  for (auto* edit : {splitGapMilliseconds_, minBurstMilliseconds_})
    discardInactive(edit, strategy == DetectionTimeStrategy::Coarse && split);
  discardInactive(noiseMargin_, fine && configuration_.profile.backend != DetectionBackendKind::Scn);
  fineFields_->setVisible(fine);
  fineSplitFields_->setVisible(fine && split);
  coarseSplitFields_->setVisible(strategy == DetectionTimeStrategy::Coarse &&
                                 split);
  splitBursts_->setVisible(fine || strategy == DetectionTimeStrategy::Coarse);
  edgeFraction_->setEnabled(fine);
  smoothMilliseconds_->setEnabled(fine);
  minOnMilliseconds_->setEnabled(fine && split);
  minOffMilliseconds_->setEnabled(fine && split);
  splitGapMilliseconds_->setEnabled(strategy == DetectionTimeStrategy::Coarse &&
                                    split);
  minBurstMilliseconds_->setEnabled(strategy == DetectionTimeStrategy::Coarse &&
                                    split);
  const bool scn = configuration_.profile.backend == DetectionBackendKind::Scn;
  // SCN keeps its explicitly approved disabled-margin explanation reachable.
  marginFields_->setVisible(fine || scn);
  noiseMargin_->setEnabled(!scn && strategy != DetectionTimeStrategy::Coarse);
  const QString marginHelp =
      scn ? "SCN 裕量固定：CNR / 频率收边 3 dB，Coarse / 粗回退 6 "
            "dB；保存值保留。"
          : "噪底裕量 / dB；精修策略生效，0–100；Enter / Tab / 应用内点击提交";
  if (noiseMargin_->property("invalid").toBool())
    noiseMargin_->setProperty("parameterHelp", marginHelp);
  else
    noiseMargin_->setToolTip(marginHelp);
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
  for (auto* button : strategies_->buttons())
    if (button->isChecked())
      button->setToolTip(button->property("detectionStrategyHelp").toString() +
                         "\n" + branchHelp_->text());
  splitBursts_->setToolTip(branchHelp_->text());
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

const DetectionRun* DetectionCard::focusedUnfinished() const {
  if (displayRunId_.empty()) return nullptr;
  const auto* file = session_.activeFile();
  if (!file) return nullptr;
  for (const auto& run : file->detectionRuns)
    if (run.request.runId == displayRunId_ &&
        !detectionIsTerminal(run.lifecycle))
      return &run;
  return nullptr;
}

QString DetectionCard::draftIssue() const {
  for (auto* edit : findChildren<QLineEdit*>())
    if (edit->property("parameterDraft").toBool() &&
        edit->property("invalid").toBool()) {
      const auto label = edit->property("parameterLabel").toString();
      return (label.isEmpty() ? QString{} : label + QStringLiteral("：")) +
             edit->property("parameterError").toString();
    }
  return {};
}

QString DetectionCard::statusIssue() const {
  if (const auto draft = draftIssue(); !draft.isEmpty()) return draft;
  if (const auto issue = sourceValidationError(); !issue.isEmpty()) return issue;
  const auto value = baseRequest();
  if (const auto issue = configurationError(value); !issue.isEmpty()) return issue;
  if (const auto issue = rangeError(value); !issue.isEmpty()) return issue;
  if (const auto issue = profileValidationError(value); !issue.isEmpty())
    return issue;
  if (const auto issue = quickModelIssue(value, testFixturesEnabled());
      !issue.isEmpty())
    return issue;
  return planIssue_;
}

QString DetectionCard::currentTimingKey() const {
  const auto& profile = configuration_.profile;
  return QString::number(source_.sampleRateHz, 'g', 17) + '|' + text(profile.id) +
         '|' + QString::number(profile.version) + '|' +
         QString::number(static_cast<int>(profile.backend)) + '|' +
         QString::number(profile.fftSize) + '|' +
         QString::number(profile.modelFrames) + '|' +
         QString::number(profile.modelHopFrames) + '|' +
         QString::number(profile.hopSeconds, 'g', 17);
}

QString DetectionCard::diagnosticInputKey() const {
  const auto value = baseRequest();
  const auto* run = latestRun();
  const auto& profile = value.profile;
  const auto& parameters = value.parameters;
  QStringList parts;
  parts << QString::number(source_.projectGeneration) << text(source_.fileId)
        << text(source_.path)
        << text(sampleFormatKey(source_.sampleFormat))
        << QString::number(source_.sampleRateHz, 'g', 17)
        << QString::number(source_.centerFrequencyHz, 'g', 17)
        << text(source_.quickFingerprint)
        << QString::number(source_.physicalSamples)
        << QString::number(source_.availableSamples)
        << QString::number(source_.selectedSampleChannel) << projectDirectory_()
        << QString::number(metadata_ && metadata_->demo) << text(profile.id)
        << QString::number(profile.version)
        << QString::number(static_cast<int>(profile.backend))
        << QString::number(profile.fftSize) << QString::number(profile.modelFrames)
        << QString::number(profile.modelHopFrames)
        << QString::number(profile.hopSeconds, 'g', 17)
        << QString::number(value.inputAdaptationVersion)
        << QString::number(static_cast<int>(value.spectrumMode))
        << text(value.rangeOrigin)
        << QString::number(value.requestedRange.time.begin)
        << QString::number(value.requestedRange.time.end)
        << QString::number(value.requestedRange.frequency.lowerHz, 'g', 17)
        << QString::number(value.requestedRange.frequency.upperHz, 'g', 17)
        << text(value.sourceAnnotationId) << text(value.sourceRevisionId)
        << QString::number(value.workingSetBudgetBytes)
        << QString::number(value.evidenceBudgetBytes)
        << QString::number(value.rawHeadBudgetBytes)
        << QString::number(value.retainRawModelOutput)
        << QString::number(value.legacyCombinedEvidence)
        << QString::number(value.sampleScratchBudgetBytes)
        << QString::number(parameters.scoreThreshold, 'g', 17)
        << QString::number(parameters.nmsIou, 'g', 17)
        << QString::number(parameters.topK) << QString::number(parameters.batchSize)
        << QString::number(parameters.minHits) << QString::number(parameters.maxMiss)
        << QString::number(parameters.mergeFrequencyHz, 'g', 17)
        << QString::number(parameters.bandwidthMarginDb, 'g', 17)
        << QString::number(static_cast<int>(parameters.timeStrategy))
        << QString::number(parameters.splitBursts)
        << QString::number(parameters.sampleEdgeRatio, 'g', 17)
        << QString::number(parameters.minOnSeconds, 'g', 17)
        << QString::number(parameters.minOffSeconds, 'g', 17)
        << QString::number(parameters.smoothSeconds, 'g', 17)
        << QString::number(parameters.coarseSplitGapSeconds, 'g', 17)
        << QString::number(parameters.coarseMinBurstSeconds, 'g', 17)
        << (value.parentRunId ? text(*value.parentRunId) : QString())
        << QString::number(testFixturesEnabled()) << text(displayRunId_)
        << (run ? text(run->request.runId) : QString())
        << (run ? QString::number(static_cast<int>(run->lifecycle))
                : QStringLiteral("-1"))
        << (run ? text(run->error) : QString());
  return parts.join('\n');
}

bool DetectionCard::detailSectionExpanded() const {
  return (diagnosticsToggle_ && diagnosticsToggle_->isChecked()) ||
         (calculationToggle_ && calculationToggle_->isChecked());
}

DetectionDiagnosticInput DetectionCard::captureDiagnosticInput() const {
  DetectionDiagnosticInput input;
  input.hasMetadata = metadata_.has_value();
  if (metadata_) input.metadata = *metadata_;
  input.projectGeneration = source_.projectGeneration;
  input.sampleRateHz = source_.sampleRateHz;
  input.availableSamples = source_.availableSamples;
  input.request = baseRequest();
  input.projectDirectory = projectDirectory_ ? projectDirectory_() : QString{};
  input.fixturesEnabled = testFixturesEnabled();
  input.draftIssue = draftIssue();
  input.sourceIssue = sourceValidationError();
  input.configurationIssue = configurationError(input.request);
  input.rangeIssue = rangeError(input.request);
  input.profileIssue = profileValidationError(input.request);
  input.includeCoverage =
      diagnosticsToggle_ && diagnosticsToggle_->isChecked();
  if (const auto* run = latestRun()) {
    input.hasRun = true;
    input.hasCoverage = !run->coverage.empty();
    input.runId = run->request.runId;
    input.sourceValidity = run->sourceValidity;
    input.runError = run->error;
    input.readFrames = run->readFrames;
    input.refinedCandidates = run->refinedCandidates;
    input.totalRefinementCandidates = run->totalRefinementCandidates;
    input.runSampleRateHz = run->request.sourceIdentity.sampleRateHz;
    if (input.includeCoverage) input.coverage = run->coverage;
  }
  return input;
}

void DetectionCard::ensureDiagnosticWorker() {
  if (diagnosticWorker_) return;
  diagnosticWorker_ = std::make_unique<DetectionDiagnosticWorker>(
      this, [this](std::uint64_t generation, DetectionDiagnosticReport report) {
        applyDiagnosticReport(generation, std::move(report));
      });
}

void DetectionCard::publishDiagnosticInputs(bool force) {
  if (!preflight_) return;
  const auto key = diagnosticInputKey();
  const bool changed = !diagnosticKeyReady_ || key != diagnosticKey_;
  // Progress polling repeats refresh with the same source and parameters.
  // That must not schedule another source plan.
  if (!changed && !force) return;
  if (changed) {
    diagnosticKey_ = key;
    diagnosticKeyReady_ = true;
    planIssue_.clear();
    diagnosticDetailReady_ = false;
  }
  ++diagnosticGeneration_;
  if (!detailSectionExpanded()) return;
  ensureDiagnosticWorker();
  diagnosticWorker_->submit(diagnosticGeneration_, captureDiagnosticInput(),
                            QThread::currentThread());
}

void DetectionCard::applyDiagnosticReport(std::uint64_t generation,
                                          DetectionDiagnosticReport report) {
  if (generation != diagnosticGeneration_ || !report.ranOffCallerThread) return;
  if (diagnostics_) diagnostics_->setText(report.diagnostics);
  if (!report.calculationDetails.isEmpty() && calculationDetails_) {
    calculationDetails_->setText(report.calculationDetails);
    if (physicalTimes_) physicalTimes_->setToolTip(report.calculationDetails);
  }
  if (!report.physicalTimes.isEmpty() && physicalTimes_)
    physicalTimes_->setText(report.physicalTimes);
  if (!report.preflightDetail.isEmpty() && preflight_) {
    preflight_->setToolTip(report.preflightDetail);
    diagnosticDetailReady_ = true;
  }
  planIssue_ = report.blockingIssue;
  updateShortStatus();
}

void DetectionCard::releaseStartLatchIfRunVisible() {
  if (!startLatchActive_ || startLatchRunId_.empty()) return;
  for (const auto& file : session_.project().files)
    for (const auto& run : file.detectionRuns)
      if (run.request.runId == startLatchRunId_) {
        // The persisted run now owns the button. A later terminal state can
        // enable it again. Rejection before save never inserts this id.
        startLatchActive_ = false;
        return;
      }
}

void DetectionCard::updateShortStatus() {
  if (!preflight_) return;
  releaseStartLatchIfRunVisible();
  const auto* activeRun = unfinishedRun();
  const bool preparing = startLatchActive_ && activeRun == nullptr;
  const bool draft = hasDraft();
  draftNote_->setVisible(draft);
  draftNote_->setText(draft ? "草稿待提交 · Enter / Tab / 应用内点击" : "");
  const auto value = baseRequest();
  const auto requested = rangeText(value.requestedRange, source_.sampleRateHz);
  rangeSummary_->setText(value.requestedRange.time.begin <
                                 value.requestedRange.time.end
                             ? requested.section('\n', 0, 0)
                             : "尚未提交范围");
  rangeSummary_->setToolTip(requested + QString("\n可用前缀 %1 / %2 样本")
                                            .arg(source_.availableSamples)
                                            .arg(source_.physicalSamples));
  const auto issue = statusIssue();
  const auto firstReason =
      issue.section('\n', 0, 0).section(QStringLiteral("；"), 0, 0);
  preflight_->setText(issue.isEmpty()
                          ? "配置就绪 · 可运行"
                          : firstReason + "；" + preflightAdvice(issue));
  if (!issue.isEmpty())
    preflight_->setToolTip(issue);
  else if (!diagnosticDetailReady_)
    preflight_->setToolTip(QStringLiteral("配置就绪 · 可运行"));
  if (preparing && issue.isEmpty())
    preflight_->setText(detectionPhaseText("preparing"));
  if (preflight_->property("preflightOk").toBool() != issue.isEmpty()) {
    preflight_->setProperty("preflightOk", issue.isEmpty());
    refreshStyle(preflight_);
  }
  const auto* focusedRun = focusedUnfinished();
  const bool otherFile =
      activeRun && activeRun->request.sourceIdentity.fileId != source_.fileId;
  // A provisional latch with no unfinished run is the gap before the task
  // exists. Do not turn the button back on in that gap.
  run_->setEnabled(issue.isEmpty() && !activeRun && !preparing);
  bool hasCommittedResult = false;
  if (const auto* file = session_.activeFile(); file && file->currentDetectionSlots)
    for (const auto& slot : *file->currentDetectionSlots)
      hasCommittedResult |= slot.sampleChannel == file->metadata.sampleFormat.selectedChannel && !slot.committedRunId.empty();
  run_->setText(preparing
                    ? detectionPhaseText("preparing")
                    : (hasCommittedResult ? QStringLiteral("应用并重新检测")
                                          : QStringLiteral("开始检测")));
  run_->setToolTip(
      "显式开始检测或重新检测。接受新检测会丢弃本文件、本 IQ 通道的上次正式结果，"
      "失败或取消也不会恢复；其它文件和通道不受影响。"
      "修改参数、模型或引擎不会清除已提交结果。"
      "保存前被拒绝的启动会恢复被临时隐藏的正式结果。");
  cancel_->setEnabled(focusedRun &&
                      focusedRun->lifecycle != DetectionLifecycle::Cancelling);
  cancel_->setToolTip(focusedRun ? "取消运行 " + text(focusedRun->request.runId)
                      : otherFile ? "其他文件的任务需先定位后再取消"
                                  : "没有未完成任务");
  if (otherRun_ && viewRunning_) {
    if (otherFile) {
      QString name = text(activeRun->request.sourceIdentity.fileId);
      for (const auto& file : session_.project().files)
        if (file.metadata.id == activeRun->request.sourceIdentity.fileId) {
          name = text(file.metadata.name);
          break;
        }
      otherRun_->setText(QString("其他文件有运行任务：%1 / %2")
                             .arg(name, lifecycleText(activeRun->lifecycle)));
      otherRun_->show();
      viewRunning_->show();
    } else {
      otherRun_->hide();
      viewRunning_->hide();
    }
  }
  selectRoi_->setEnabled(metadata_ && availableSamples(*metadata_) > 0);
  if (preparing) {
    phaseBar_->hide();
    runSummary_->setText(detectionPhaseText("preparing"));
    runSummary_->setToolTip(QStringLiteral("启动已受理，正在准备检测"));
    runSummary_->show();
  } else if (const auto* run = latestRun()) {
    const auto phase = text(run->computationPhase);
    const int step = phase == "completed"                           ? 4
                     : phase == "refine" || phase == "publishing"   ? 3
                     : phase == "inference" || phase == "frequency" ||
                           phase == "decoding" || phase == "storing-heads" ||
                           phase == "tracking" || phase == "checkpoint" ? 2
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
    runSummary_->setText(QString("%1 · %2 · 窗口 %3/%4 · 结果 %5")
                             .arg(lifecycleText(run->lifecycle),
                                  detectionPhaseText(run->computationPhase))
                             .arg(run->completedWindows)
                             .arg(run->eligibleWindows)
                             .arg(run->resultCount));
    runSummary_->setToolTip(
        QString("运行 %1；完整来源、覆盖及技术 JSON 请查看底部任务详情。")
            .arg(text(run->request.runId)));
    runSummary_->show();
  } else {
    phaseBar_->hide();
    runSummary_->setText(activeRun ? "已有未完成任务 · 当前配置用于下一次"
                                   : "");
    runSummary_->setVisible(activeRun != nullptr);
  }
}

void DetectionCard::refreshStatus() {
  if (!preflight_) return;
  publishDiagnosticInputs(false);
  updateShortStatus();
}

void DetectionCard::refresh() {
  refreshSource();
  refreshSegments();
  refreshTiming();
  refreshBranchState();
  // ASSERT: this path does not call makeDetectionSourcePlan or
  // buildDetectionDiagnosticReport. Short status uses the run snapshot;
  // expanded detail sections schedule one background generation.
  refreshStatus();
}

QString DetectionCard::configurationError(const DetectionRequest& value) const {
  const auto& profile = value.profile;
  if (!findDetectionProfile(profile.id)) return "请选择有效能力方案";
  if (profile.fftSize < 32 || profile.fftSize > 262144 ||
      !profile.modelFrames || !profile.modelHopFrames)
    return "分析N须为32–262144，窗口帧数和滑动帧数须为正整数";
  if (!std::isfinite(profile.hopSeconds) || profile.hopSeconds <= 0 ||
      profile.hopSeconds > 100)
    return "谱步长须为大于 0 且不超过 100000 ms 的有限数";
  if (source_.sampleRateHz > 0 &&
      std::floor(source_.sampleRateHz * profile.hopSeconds) < 1)
    return "谱步长必须至少包含 1 个源样本";
  for (const auto& parameter : parameters_) {
    const double number = value.parameters.*parameter.member;
    if (!std::isfinite(number) || number < 0 || number > parameter.maximum)
      return parameter.label + QString("须为 0–%1 的有限数")
                                   .arg(parameter.maximum * parameter.scale);
  }
  const auto& p = value.parameters;
  if (!p.topK || p.topK > 512) return "topK 须为 1–512";
  if (!isDetectionExecutionBatchTier(p.batchSize))
    return "batch 须为 1、2、4、8 或 16";
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
  if (!registered || value.profile.version != registered->version ||
      value.profile.backend != registered->backend)
    return "能力方案身份无效";
  return {};
}

QString DetectionCard::contextValidationError(
    const DetectionRequest& value) const {
  if (const auto quick = quickModelIssue(value, testFixturesEnabled());
      !quick.isEmpty())
    return quick;
  if (value.profile.backend != DetectionBackendKind::Fixture) {
    auto snapshot = value;
    QString issue;
    if (!freezeDetectionModelSnapshot(snapshot, issue)) return issue;
  }
  DetectionSourcePlan plan;
  QString planError;
  if (!makeDetectionSourcePlan(*metadata_, value.requestedRange,
                               sourceConfig(value), plan, planError,
                               source_.projectGeneration))
    return "来源预检失败：" + planError;
  if (!plan.eligibleWindowCount) return "请求范围没有可用的真实观测";
  std::uint64_t reserve = 0;
  if (!detectionExecutionBatchSize(plan, value.parameters.batchSize, reserve))
    return "减少批量后，单窗口仍超出DSP、输入报文或观测证据分片预算";
  return {};
}

QString DetectionCard::validationError() const {
  if (const auto issue = draftIssue(); !issue.isEmpty()) return issue;
  if (const auto issue = sourceValidationError(); !issue.isEmpty())
    return issue;
  const auto value = baseRequest();
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
  const auto found =
      profileConfigurations_.find(configurationKey(parentRun->request));
  if (found == profileConfigurations_.end() ||
      model_->findData(text(parentRun->request.profile.id)) < 0) {
    emit message("派生快照的方案当前不可用，已保留配置草稿");
    return;
  }
  configuration_ = parentRun->request;
  configuration_.inputAdaptationVersion = 2;
  configuration_.parentRunId = parentRun->request.runId;
  configuration_.runId.clear();
  // Explicit derivation copies the original range, independently of a later
  // view or annotation revision. Merely opening the card never does this.
  requestedRoi_ = parentRun->request.requestedRange;
  configuration_.rangeOrigin = "custom";
  profileConfigurations_[configurationKey(configuration_)] = configuration_;
  loadControls();
  refresh();
}

void DetectionCard::setRequestedRoi(ViewRange range) {
  refreshSource();
  requestedRoi_ = range;
  configuration_.rangeOrigin = "custom";
  profileConfigurations_[configurationKey(configuration_)] = configuration_;
  const QSignalBlocker block(rangeKind_);
  rangeKind_->setCurrentIndex(rangeKind_->findData("custom"));
  refresh();
}

void DetectionCard::useVisibleRange() {
  refreshSource();
  configuration_.rangeOrigin = "visible";
  profileConfigurations_[configurationKey(configuration_)] = configuration_;
  const QSignalBlocker block(rangeKind_);
  rangeKind_->setCurrentIndex(rangeKind_->findData("visible"));
  refresh();
}

void DetectionCard::focusRun(const QString& runId) {
  displayRunId_ = runId.toStdString();
  refreshStatus();
}

void DetectionCard::noteStartRejected(const std::string& runId,
                                      const QString& notice) {
  if (QThread::currentThread() != thread()) {
    QPointer<DetectionCard> self(this);
    const auto id = runId;
    const auto text = notice;
    QMetaObject::invokeMethod(
        this,
        [self, id, text] {
          if (self) self->noteStartRejected(id, text);
        },
        Qt::QueuedConnection);
    return;
  }
  if (!startLatchActive_ || runId != startLatchRunId_) return;
  startLatchActive_ = false;
  refreshStatus();
  if (preflight_ && !notice.isEmpty()) {
    preflight_->setText(notice);
    preflight_->setToolTip(notice);
  }
}

void DetectionCard::submit() {
  if (QThread::currentThread() != thread()) {
    QPointer<DetectionCard> self(this);
    QMetaObject::invokeMethod(
        this,
        [self] {
          if (self) self->submit();
        },
        Qt::QueuedConnection);
    return;
  }
  // A click that is still inside this function, and any later click while
  // the provisional run has not been rejected or persisted, must not emit
  // again. The message is the prepare wait, not a publication wait.
  if (startLatchActive_) {
    emit message(QStringLiteral("检测正在准备，请稍候"));
    return;
  }
  // Enter/Tab/application clicks are the only commit paths. Running also
  // commits any remaining drafts. The source plan, disk query, and evidence
  // budget stay on the detection commit worker; this click only rejects
  // drafts and local configuration before handing off the request.
  for (auto* widget : findChildren<QWidget*>())
    if (auto* draft = dynamic_cast<ParameterDraft*>(widget)) draft->commit();
  refresh();
  auto value = baseRequest();
  const auto issue = [&] {
    if (const auto draft = draftIssue(); !draft.isEmpty()) return draft;
    if (const auto source = sourceValidationError(); !source.isEmpty())
      return source;
    if (const auto configuration = configurationError(value);
        !configuration.isEmpty())
      return configuration;
    if (const auto range = rangeError(value); !range.isEmpty()) return range;
    if (const auto profile = profileValidationError(value); !profile.isEmpty())
      return profile;
    return quickModelIssue(value, testFixturesEnabled());
  }();
  if (!issue.isEmpty()) {
    emit message(issue);
    return;
  }
  if (unfinishedRun()) {
    emit message("本工程已有未完成任务；当前参数用于下一次运行");
    return;
  }
  if (startLatchActive_) {
    emit message(QStringLiteral("检测正在准备，请稍候"));
    return;
  }
  value.runId =
      "run-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
  displayRunId_ = value.runId;
  startLatchRunId_ = value.runId;
  startLatchActive_ = true;
  startLatchSteadyNs_ = steadyClockNanoseconds();
  const auto preparing = detectionPhaseText("preparing");
  run_->setEnabled(false);
  run_->setText(preparing);
  if (preflight_) preflight_->setText(preparing);
  emit requestSubmitted(value);
  // A synchronous rejection clears the latch and writes the real error.
  // Refreshing again here would replace that text with "配置就绪 · 可运行".
  if (startLatchActive_) refreshStatus();
}
}  // namespace signalstudio
