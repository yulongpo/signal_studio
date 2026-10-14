#include "ui/annotations/annotation_evidence_widget.h"

#include <QDataStream>
#include <QFont>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QSizePolicy>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_set>

#include "domain/project.h"

namespace signalstudio {
namespace {

constexpr std::size_t kRangePreviewCount = 4;
constexpr std::size_t kWindowPreviewCount = 12;
constexpr qsizetype kRawPreviewCharacters = 12000;

QString text(const std::string& value) { return QString::fromUtf8(value); }

QString count(std::uint64_t value) {
  return QString::number(static_cast<qulonglong>(value));
}

QString number(double value) {
  return std::isfinite(value) ? QString::number(value, 'g', 12) : "暂无";
}

QString present(const std::string& value, const QString& absent = "—") {
  return value.empty() ? absent : text(value);
}

// Break long identifiers visually; the tooltip preserves the original text.
QString wrapLongTokens(const QString& value) {
  QString result;
  int tokenLength = 0;
  for (const auto character : value) {
    if (character.isSpace()) {
      tokenLength = 0;
    } else if (++tokenLength > 24) {
      result += QChar(0x200b);
      tokenLength = 1;
    }
    result += character;
  }
  return result;
}

void setLabelText(QLabel* label, const QString& value) {
  label->setText(wrapLongTokens(value));
  label->setToolTip("<qt>" + value.toHtmlEscaped().replace('\n', "<br>") +
                    "</qt>");
}

QLabel* wrappingLabel(const QString& objectName, QWidget* parent) {
  auto* label = new QLabel(parent);
  label->setObjectName(objectName);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QString sourceName(AnnotationSource source) {
  switch (source) {
    case AnnotationSource::ImportedLabel:
      return "原始标签";
    case AnnotationSource::Manual:
      return "手动标记";
    case AnnotationSource::Algorithm:
      return "算法结果";
  }
  return "未知来源";
}

QString refineMethodName(RefineMethod method) {
  switch (method) {
    case RefineMethod::Coarse:
      return "coarse";
    case RefineMethod::Sample:
      return "sample";
    case RefineMethod::Roi:
      return "ROI";
    case RefineMethod::None:
      return "none";
    case RefineMethod::Manual:
      return "manual";
  }
  return "unknown";
}

QString refineStatusName(RefineStatus status) {
  switch (status) {
    case RefineStatus::Applied:
      return "applied · 已应用";
    case RefineStatus::Fallback:
      return "fallback · 回退";
    case RefineStatus::Skipped:
      return "skipped · 跳过";
    case RefineStatus::Unknown:
      return "unknown · 未提供";
  }
  return "unknown · 未提供";
}

QString timeQualityName(TimeQuality quality) {
  switch (quality) {
    case TimeQuality::SampleLabel:
      return "SampleLabel · 标签样本索引";
    case TimeQuality::RoundedLabel:
      return "RoundedLabel · 文本量化标签";
    case TimeQuality::AlgorithmEstimated:
      return "AlgorithmEstimated · 算法时间估计";
    case TimeQuality::Manual:
      return "Manual · 人工指定";
    case TimeQuality::WindowObservation:
      return "WindowObservation · 窗口观察，时间未定位";
    case TimeQuality::Unknown:
      return "Unknown · 时间未定位";
  }
  return "Unknown · 时间未定位";
}

QString frequencyQualityName(FrequencyQuality quality) {
  switch (quality) {
    case FrequencyQuality::NominalLabel:
      return "NominalLabel · 标称标签频带，非实测占用带宽";
    case FrequencyQuality::ModelEstimated:
      return "ModelEstimated · 模型频带估计";
    case FrequencyQuality::Refined:
      return "Refined · 已保存的精修频带";
    case FrequencyQuality::Manual:
      return "Manual · 人工指定频带";
    case FrequencyQuality::Unknown:
      return "Unknown · 未提供频率质量";
  }
  return "Unknown · 未提供频率质量";
}

QString frequencyFrameName(FrequencyFrame frame) {
  switch (frame) {
    case FrequencyFrame::RF:
      return "RF";
    case FrequencyFrame::Baseband:
      return "基带";
    case FrequencyFrame::RealOneSided:
      return "实数单边";
  }
  return "未知频率坐标";
}

QString rangeText(const ViewRange& range, FrequencyFrame frame,
                  double sampleRate) {
  QString value = QString("样本 [%1, %2)")
                      .arg(count(range.time.begin), count(range.time.end));
  if (std::isfinite(sampleRate) && sampleRate > 0) {
    value += QString(" · %1–%2 ms")
                 .arg(number(static_cast<double>(range.time.begin) /
                             sampleRate * 1000),
                      number(static_cast<double>(range.time.end) / sampleRate *
                             1000));
  } else {
    value += " · 采样率缺失，不能换算时间";
  }
  return value + QString("\n%1–%2 MHz（%3）")
                     .arg(number(range.frequency.lowerHz / 1e6),
                          number(range.frequency.upperHz / 1e6),
                          frequencyFrameName(frame));
}

QString segmentRanges(const std::vector<Segment>& segments,
                      const std::string& activeId, double sampleRate) {
  if (segments.empty()) return "t0 / t1 = —；无活动段，不推算发射起止";
  QStringList ranges;
  const auto selected = std::find_if(
      segments.begin(), segments.end(),
      [&activeId](const auto& segment) { return segment.id == activeId; });
  if (selected != segments.end()) {
    ranges << "选中段：" + present(selected->id) + "\n" +
                  rangeText(selected->range, selected->frequencyFrame,
                            sampleRate);
  }
  for (auto it = segments.begin(); it != segments.end(); ++it) {
    if (it == selected) continue;
    if (ranges.size() >= static_cast<qsizetype>(kRangePreviewCount)) break;
    ranges << present(it->id) + "\n" +
                  rangeText(it->range, it->frequencyFrame, sampleRate);
  }
  if (segments.size() > kRangePreviewCount) {
    ranges << QString("共 %1 段；此处预览 %2 段，选择活动段查看其边界")
                  .arg(count(segments.size()), count(kRangePreviewCount));
  }
  return ranges.join("\n\n");
}

const AnnotationSourceSet* sourceSet(const Annotation& annotation,
                                     const FileState* file) {
  if (!file) return nullptr;
  const auto found = std::find_if(
      file->sourceSets.begin(), file->sourceSets.end(),
      [&annotation](const auto& s) { return s.id == annotation.sourceSetId; });
  return found == file->sourceSets.end() ? nullptr : &*found;
}

bool isFixture(const Annotation& annotation,
               const AnnotationSourceSet* source) {
  return (source && source->profileId == "pipeline-fixture-v1") ||
         (annotation.originEvidence &&
          annotation.originEvidence->fallbackReason ==
              "Pipeline fixture has no validated time localization");
}

bool isManualRevision(const Annotation& annotation) {
  return std::any_of(annotation.revisions.begin(), annotation.revisions.end(),
                     [&annotation](const auto& revision) {
                       return revision.id == annotation.revisionId &&
                              !revision.parentRevisionId.empty();
                     }) ||
         std::any_of(annotation.segments.begin(), annotation.segments.end(),
                     [](const auto& segment) {
                       return segment.timeQuality == TimeQuality::Manual;
                     });
}

QString evidenceTitle(const Annotation& annotation) {
  if (annotation.sourceKind == AnnotationSource::Manual)
    return "人工标注 / 修订";
  if (isManualRevision(annotation)) return "人工修订 · 保留原始来源";
  const auto* evidence = annotation.originEvidence.get();
  if (annotation.sourceKind == AnnotationSource::ImportedLabel) {
    if (evidence && evidence->formatId == "generator-txt-v1")
      return "生成器 TXT 标签 · 10 µs 量化";
    if (evidence && evidence->formatId == "torchsig-rfbench-sigmf-v1")
      return "TorchSig 样本标签 · 原始元数据";
    return "导入标签 · 原始元数据";
  }
  if (evidence && evidence->refineStatus == RefineStatus::Fallback)
    return "精修回退";
  if (annotation.segments.empty()) return "窗口观察频带 · 时间未知";
  if (!evidence || evidence->refineStatus != RefineStatus::Applied)
    return "时间事件 · 暂无已应用的精修证据";
  switch (evidence->refineMethod) {
    case RefineMethod::Sample:
      return "sample 精修成功";
    case RefineMethod::Roi:
      return "ROI 精修成功";
    case RefineMethod::Coarse:
      return "粗时间事件 · coarse";
    case RefineMethod::Manual:
      return "人工修订 · 保留原始来源";
    case RefineMethod::None:
      return "时间事件 · 未执行精修";
  }
  return "时间事件 · 精修方法未知";
}

QString originNotice(const Annotation& annotation,
                     const AnnotationSourceSet* source, bool fixture) {
  QString value;
  if (fixture) {
    value =
        "M2 系统链路夹具：分数和候选仅用于工程测试；暂无真实模型前向、"
        "精修或效果证据。";
  } else if (annotation.sourceKind == AnnotationSource::Algorithm) {
    value =
        "此卡展示来源保存的算法记录；选中标记不触发模型或重算。"
        "未保存的精修数据、噪底或轨迹显示暂无。";
  } else if (annotation.sourceKind == AnnotationSource::Manual) {
    value = "边界由人工指定；不是算法时间估计或检测效果证据。";
  } else {
    const auto format = annotation.originEvidence
                            ? annotation.originEvidence->formatId
                        : source ? source->formatId
                                 : std::string{};
    value = format == "generator-txt-v1"
                ? "生成器 TIME 文本以 10 µs 量化，导入按最近样本取整；"
                  "不是样本级真值。标称频带不是实测占用带宽。"
            : format == "torchsig-rfbench-sigmf-v1"
                ? "TorchSig SigMF 使用原始样本索引和 RF 频率；"
                  "标签类别来自元数据，未作模型识别。"
                : "仅展示已保存的标签字段；坐标解释见来源记录。";
  }
  if (annotation.sourceKind != AnnotationSource::Manual &&
      isManualRevision(annotation))
    value += " 当前人工修订不会覆盖原始精修方法、状态或分数。";
  return value;
}

QString optionalScore(const std::optional<double>& score) {
  return score ? number(*score) : "暂无保存值";
}

class MeasuredEvidenceTrace final : public QWidget {
 public:
  explicit MeasuredEvidenceTrace(QWidget* parent) : QWidget(parent) {
    setObjectName("annotationMeasuredEvidenceTrace");
    setFixedHeight(70);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    hide();
  }
  void setEvidence(const QJsonObject& evidence) {
    points_.clear();
    maximum_ = 0;
    threshold_ = evidence["comparisonThreshold"].toDouble(-1);
    const auto trace = evidence["trace"].toObject();
    const auto savedPoints = trace["points"].toArray();
    bool valid = savedPoints.size() >= 2 && savedPoints.size() <= 512 &&
                 trace["kind"] == "measured-causal-mean-power-v1";
    bool beginOk = false, endOk = false;
    const auto begin =
        trace["sourceOriginSample"].toString().toULongLong(&beginOk);
    const auto end =
        trace["sourceSupportEndSample"].toString().toULongLong(&endOk);
    valid &= beginOk && endOk && begin < end;
    quint64 previous = begin;
    for (const auto item : savedPoints) {
      const auto pair = item.toArray();
      if (pair.size() != 2) {
        valid = false;
        break;
      }
      bool ok = false;
      const auto sample = pair.at(0).toString().toULongLong(&ok);
      const double power = pair.at(1).toDouble(-1);
      if (!valid || !ok || sample < previous || sample >= end ||
          !std::isfinite(power) || power < 0) {
        valid = false;
        break;
      }
      points_.emplace_back(static_cast<double>(sample - begin) / (end - begin),
                           power);
      maximum_ = std::max(maximum_, power);
      previous = sample;
    }
    setProperty("measuredPointCount",
                static_cast<qulonglong>(valid ? points_.size() : 0));
    setVisible(valid && maximum_ > 0);
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF area(2, 3, width() - 4, height() - 21);
    painter.fillRect(area, QColor("#0d2233"));
    painter.setPen(QColor("#26465d"));
    painter.drawLine(area.topLeft(), area.topRight());
    painter.drawLine(area.bottomLeft(), area.bottomRight());
    if (points_.empty() || maximum_ <= 0) return;
    QPainterPath path;
    for (std::size_t i = 0; i < points_.size(); ++i) {
      const QPointF point(
          area.left() + points_[i].x() * area.width(),
          area.bottom() - points_[i].y() / maximum_ * area.height());
      if (!i)
        path.moveTo(point);
      else
        path.lineTo(point);
    }
    painter.setPen(QPen(QColor("#61c3ca"), 1.5));
    painter.drawPath(path);
    if (threshold_ >= 0 && threshold_ <= maximum_) {
      painter.setPen(QPen(QColor("#e4b26d"), 1, Qt::DashLine));
      const auto y = area.bottom() - threshold_ / maximum_ * area.height();
      painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
    painter.setPen(QColor("#94b3c6"));
    painter.drawText(QRectF(2, height() - 17, width() - 4, 17),
                     "已保存的实测包络 / 线性功率");
  }

 private:
  std::vector<QPointF> points_;
  double maximum_ = 0, threshold_ = -1;
};

}  // namespace

AnnotationEvidenceWidget::AnnotationEvidenceWidget(QWidget* parent)
    : AnnotationEvidenceWidget(Mode::Inspector, parent) {}

AnnotationEvidenceWidget::AnnotationEvidenceWidget(Mode mode, QWidget* parent)
    : QWidget(parent), mode_(mode) {
  setObjectName("annotationEvidenceWidget");
  setProperty("evidenceMode",
              mode_ == Mode::Auxiliary ? "auxiliary" : "inspector");
  setMinimumWidth(0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto* layout = new QVBoxLayout(this);
  if (mode_ == Mode::Auxiliary) {
    layout->setContentsMargins(18, 11, 18, 11);
    layout->setAlignment(Qt::AlignTop);
  } else {
    layout->setContentsMargins(0, 0, 0, 0);
  }
  layout->setSpacing(7);
  title_ = wrappingLabel("annotationEvidenceTitle", this);
  auto font = title_->font();
  font.setBold(true);
  title_->setFont(font);
  context_ = wrappingLabel("annotationEvidenceContext", this);
  notice_ = wrappingLabel("annotationEvidenceNotice", this);
  if (mode_ == Mode::Auxiliary) {
    buildAuxiliary(layout);
  } else {
    layout->addWidget(title_);
    layout->addWidget(context_);
    layout->addWidget(notice_);
    buildFacts(layout);
  }
  if (mode_ == Mode::Inspector) {
    measuredTrace_ = new MeasuredEvidenceTrace(this);
    layout->addWidget(measuredTrace_);
  }
  rawToggle_ = new QToolButton(this);
  rawToggle_->setObjectName("annotationOriginalFieldsToggle");
  rawToggle_->setText("不可变原始字段");
  rawToggle_->setCheckable(true);
  rawToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  rawToggle_->setArrowType(Qt::RightArrow);
  rawFields_ = new QPlainTextEdit(this);
  rawFields_->setObjectName("annotationEvidence");
  rawFields_->setReadOnly(true);
  rawFields_->setMaximumHeight(170);
  rawFields_->setMinimumWidth(0);
  rawFields_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  rawFields_->hide();
  layout->addWidget(rawToggle_);
  layout->addWidget(rawFields_);
  connect(rawToggle_, &QToolButton::toggled, this, [this](bool expanded) {
    rawFields_->setVisible(expanded);
    rawToggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
  });
  clearEvidence();
}

void AnnotationEvidenceWidget::buildFacts(QVBoxLayout* layout) {
  auto* facts = new QGridLayout;
  facts->setColumnStretch(1, 1);
  facts->setHorizontalSpacing(8);
  facts->setVerticalSpacing(5);
  layout->addLayout(facts);
  addFact(facts, Fact::Method, "原始精修", "annotationEvidenceMethod");
  addFact(facts, Fact::TimeBasis, "时间依据", "annotationEvidenceTimeBasis");
  addFact(facts, Fact::Uncertainty, "不确定度",
          "annotationEvidenceUncertainty");
  addFact(facts, Fact::FrequencyBasis, "频率依据",
          "annotationEvidenceFrequency");
  addFact(facts, Fact::CurrentRange, "当前范围",
          "annotationEvidenceCurrentRange");
  addFact(facts, Fact::OriginalRange, "原始范围",
          "annotationEvidenceOriginalRange");
  addFact(facts, Fact::Observation, "观察支持",
          "annotationEvidenceObservation");
  addFact(facts, Fact::Mean, "mean", "annotationEvidenceMean");
  addFact(facts, Fact::Maximum, "max", "annotationEvidenceMaximum");
  addFact(facts, Fact::Windows, "唯一窗口", "annotationEvidenceWindows");
  addFact(facts, Fact::WindowIds, "窗口 IDs", "annotationEvidenceWindowIds");
  addFact(facts, Fact::Noise, "噪底 / 轨迹", "annotationEvidenceNoise");
  addFact(facts, Fact::Reason, "回退 / 原因", "annotationEvidenceReason");
  addFact(facts, Fact::Class, "原始类别", "annotationEvidenceClass");
  addFact(facts, Fact::Format, "formatId", "annotationEvidenceFormat");
  addFact(facts, Fact::Origin, "originRef", "annotationEvidenceOrigin");
  addFact(facts, Fact::Coordinates, "坐标解释",
          "annotationEvidenceCoordinates");
  scoreDefinition_ = wrappingLabel("annotationEvidenceScoreDefinition", this);
  layout->addWidget(scoreDefinition_);
}

void AnnotationEvidenceWidget::buildAuxiliary(QVBoxLayout* layout) {
  auto* summary = new QWidget(this);
  summary->setObjectName("annotationAuxiliaryEvidenceSummary");
  summary->setMinimumWidth(0);
  auto* columns = new QGridLayout(summary);
  columns->setContentsMargins(0, 0, 0, 0);
  columns->setHorizontalSpacing(18);
  const auto column = [&](int index, const QString& name) {
    auto* body = new QWidget(summary);
    body->setObjectName(name);
    body->setMinimumWidth(0);
    body->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* contents = new QVBoxLayout(body);
    contents->setContentsMargins(0, 0, 0, 0);
    contents->setSpacing(5);
    columns->addWidget(body, 0, index, Qt::AlignTop);
    columns->setColumnStretch(index, 1);
    return contents;
  };
  const auto heading = [&](QVBoxLayout* contents, const QString& caption,
                           const QString& name) {
    auto* label = wrappingLabel(name, this);
    auto font = label->font();
    font.setBold(true);
    label->setFont(font);
    label->setStyleSheet("color: #94d9df;");
    setLabelText(label, caption);
    contents->addWidget(label);
  };
  const auto facts = [](QVBoxLayout* contents) {
    auto* rows = new QGridLayout;
    rows->setColumnStretch(1, 1);
    rows->setHorizontalSpacing(8);
    rows->setVerticalSpacing(5);
    contents->addLayout(rows);
    return rows;
  };

  auto* time = column(0, "annotationAuxiliaryTimeSummary");
  title_->setStyleSheet("color: #94d9df;");
  time->addWidget(title_);
  auto* timeFacts = facts(time);
  addFact(timeFacts, Fact::Method, "原始精修", "annotationEvidenceMethod");
  addFact(timeFacts, Fact::TimeBasis, "时间依据",
          "annotationEvidenceTimeBasis");
  time->addWidget(notice_);

  auto* scores = column(1, "annotationAuxiliaryScoreSummary");
  heading(scores, "保存分数 / 唯一窗口", "annotationAuxiliaryScoreHeading");
  auto* scoreFacts = facts(scores);
  addFact(scoreFacts, Fact::Mean, "mean", "annotationEvidenceMean");
  auto* mean = values_[static_cast<std::size_t>(Fact::Mean)];
  auto metricFont = mean->font();
  metricFont.setPointSize(18);
  mean->setFont(metricFont);
  mean->setStyleSheet("color: #bfe9f5;");
  addFact(scoreFacts, Fact::Maximum, "max", "annotationEvidenceMaximum");
  addFact(scoreFacts, Fact::Windows, "unique windows",
          "annotationEvidenceWindows");

  auto* diagnostics = column(2, "annotationAuxiliaryDiagnosticSummary");
  heading(diagnostics, "噪底 / 轨迹 / 回退", "annotationAuxiliaryNoiseHeading");
  auto* diagnosticFacts = facts(diagnostics);
  addFact(diagnosticFacts, Fact::Noise, "保存证据", "annotationEvidenceNoise");
  addFact(diagnosticFacts, Fact::Reason, "回退 / 原因",
          "annotationEvidenceReason");
  measuredTrace_ = new MeasuredEvidenceTrace(this);
  diagnostics->addWidget(measuredTrace_);
  layout->addWidget(summary);

  auto* detailsToggle = new QToolButton(this);
  detailsToggle->setObjectName("annotationEvidenceDetailsToggle");
  detailsToggle->setText("当前 / 原始 / 观察与来源详情");
  detailsToggle->setCheckable(true);
  detailsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  detailsToggle->setArrowType(Qt::RightArrow);
  layout->addWidget(detailsToggle);
  auto* details = new QWidget(this);
  details->setObjectName("annotationAuxiliaryEvidenceDetails");
  details->setMinimumWidth(0);
  auto* detailsLayout = new QVBoxLayout(details);
  detailsLayout->setContentsMargins(0, 0, 0, 0);
  detailsLayout->setSpacing(7);
  detailsLayout->addWidget(context_);
  auto* detailFacts = facts(detailsLayout);
  addFact(detailFacts, Fact::Uncertainty, "不确定度",
          "annotationEvidenceUncertainty");
  addFact(detailFacts, Fact::FrequencyBasis, "频率依据",
          "annotationEvidenceFrequency");
  addFact(detailFacts, Fact::CurrentRange, "当前范围",
          "annotationEvidenceCurrentRange");
  addFact(detailFacts, Fact::OriginalRange, "原始范围",
          "annotationEvidenceOriginalRange");
  addFact(detailFacts, Fact::Observation, "观察支持",
          "annotationEvidenceObservation");
  addFact(detailFacts, Fact::WindowIds, "窗口 IDs",
          "annotationEvidenceWindowIds");
  addFact(detailFacts, Fact::Class, "原始类别", "annotationEvidenceClass");
  addFact(detailFacts, Fact::Format, "formatId", "annotationEvidenceFormat");
  addFact(detailFacts, Fact::Origin, "originRef", "annotationEvidenceOrigin");
  addFact(detailFacts, Fact::Coordinates, "坐标解释",
          "annotationEvidenceCoordinates");
  scoreDefinition_ = wrappingLabel("annotationEvidenceScoreDefinition", this);
  detailsLayout->addWidget(scoreDefinition_);
  details->hide();
  layout->addWidget(details);
  connect(
      detailsToggle, &QToolButton::toggled, this,
      [details, detailsToggle](bool expanded) {
        details->setVisible(expanded);
        detailsToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
      });
}

void AnnotationEvidenceWidget::addFact(QGridLayout* layout, Fact fact,
                                       const QString& caption,
                                       const QString& objectName) {
  const auto index = static_cast<std::size_t>(fact);
  auto* name = wrappingLabel(objectName + "Caption", this);
  name->setMaximumWidth(90);
  name->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  setLabelText(name, caption);
  auto* value = wrappingLabel(objectName, this);
  values_[index] = value;
  const auto row = layout->count() == 0 ? 0 : layout->rowCount();
  layout->addWidget(name, row, 0, Qt::AlignTop);
  layout->addWidget(value, row, 1, Qt::AlignTop);
}

void AnnotationEvidenceWidget::setFact(Fact fact, const QString& value) {
  setLabelText(values_[static_cast<std::size_t>(fact)], value);
}

void AnnotationEvidenceWidget::clearEvidence() {
  if (empty_) return;
  empty_ = true;
  displayKey_.clear();
  displayedOrigin_.reset();
  static_cast<MeasuredEvidenceTrace*>(measuredTrace_)->setEvidence({});
  setLabelText(title_, "检测证据");
  setLabelText(context_, "选择一个标记查看原始来源与当前修订。");
  setLabelText(notice_, mode_ == Mode::Auxiliary
                            ? "选择一个标记查看时间、精修与已保存证据。"
                            : QString{});
  setLabelText(scoreDefinition_, {});
  for (auto* value : values_) setLabelText(value, "—");
  rawFields_->clear();
  rawToggle_->setEnabled(false);
}

void AnnotationEvidenceWidget::setAnnotation(const Annotation* annotation,
                                             const FileState* file) {
  if (!annotation) {
    clearEvidence();
    return;
  }
  const auto* source = sourceSet(*annotation, file);
  const bool fixture = isFixture(*annotation, source);
  const double currentSampleRate = file ? file->metadata.sampleRateHz : 0;
  const double originalSampleRate =
      source ? source->sourceIdentity.sampleRateHz : currentSampleRate;
  // Revisions own geometry; evidence and source sets are immutable. Retain only
  // the shared const origin and a value key, never Annotation/FileState
  // pointers. This also preserves raw-field selection/scroll on unrelated view
  // updates.
  QByteArray key;
  QDataStream values(&key, QIODevice::WriteOnly);
  values << text(annotation->id) << text(annotation->revisionId)
         << text(annotation->activeSegmentId)
         << text(annotation->reviewRevisionId)
         << static_cast<int>(annotation->sourceKind)
         << static_cast<int>(annotation->reviewStatus) << text(annotation->name)
         << text(annotation->note) << text(annotation->classLabel.value_or(""))
         << quint64(annotation->revisions.size())
         << quint64(annotation->reviewHistory.size()) << currentSampleRate
         << originalSampleRate << (file ? text(file->metadata.id) : QString{})
         << (source ? text(source->id) : QString{})
         << (source ? text(source->sourceHash) : QString{})
         << (source ? text(source->profileId) : QString{})
         << (source ? text(source->formatId) : QString{})
         << (source ? text(source->originalPath) : QString{});
  if (!empty_ && key == displayKey_ &&
      displayedOrigin_ == annotation->originEvidence)
    return;
  empty_ = false;
  displayKey_ = std::move(key);
  displayedOrigin_ = annotation->originEvidence;
  setLabelText(title_,
               (fixture ? "链路夹具 · " : "") + evidenceTitle(*annotation));
  setLabelText(context_, sourceName(annotation->sourceKind) + " · " +
                             present(annotation->id) + "\nrevision " +
                             present(annotation->revisionId));
  setLabelText(notice_, originNotice(*annotation, source, fixture));
  showTimeEvidence(*annotation, currentSampleRate, originalSampleRate);
  showOriginEvidence(*annotation, source);
  showScores(*annotation, fixture);
}

void AnnotationEvidenceWidget::showTimeEvidence(const Annotation& annotation,
                                                double currentSampleRate,
                                                double originalSampleRate) {
  const auto* segment = activeSegment(annotation);
  setFact(Fact::TimeBasis,
          timeQualityName(segment ? segment->timeQuality
                                  : annotation.observationTimeQuality) +
              (segment && segment->clippedAtRoi ? "；ROI 边缘裁切" : ""));
  QString uncertainty = "—；无活动段，不能从观察支持推算定位误差";
  if (segment) {
    uncertainty = count(segment->uncertaintySamples) + " samples";
    if (std::isfinite(currentSampleRate) && currentSampleRate > 0)
      uncertainty += " / " +
                     number(static_cast<double>(segment->uncertaintySamples) /
                            currentSampleRate * 1e6) +
                     " µs";
    uncertainty += "（保存值；不代表零误差或统计置信区间）";
  }
  setFact(Fact::Uncertainty, uncertainty);
  setFact(Fact::FrequencyBasis,
          segment ? frequencyQualityName(segment->frequencyQuality)
                  : "仅观察频带；暂无活动段频率质量记录");
  setFact(Fact::CurrentRange,
          segmentRanges(annotation.segments, annotation.activeSegmentId,
                        currentSampleRate));
  const auto* evidence = annotation.originEvidence.get();
  setFact(Fact::OriginalRange,
          evidence
              ? segmentRanges(evidence->originalSegments,
                              annotation.activeSegmentId, originalSampleRate)
              : "暂无不可变原始几何");
  QString observation =
      annotation.observation
          ? "当前观察：" + rangeText(*annotation.observation,
                                     annotation.observationFrequencyFrame,
                                     currentSampleRate)
          : "当前无观察窗口";
  if (evidence && evidence->originalObservation)
    observation +=
        "\n原始观察：" + rangeText(*evidence->originalObservation,
                                   evidence->observationFrequencyFrame,
                                   originalSampleRate);
  observation += "\n观察窗口是检测支持范围，不是发射时段或发射时长。";
  setFact(Fact::Observation, observation);
}

void AnnotationEvidenceWidget::showOriginEvidence(
    const Annotation& annotation, const AnnotationSourceSet* source) {
  const auto* evidence = annotation.originEvidence.get();
  setFact(Fact::Method, evidence
                            ? refineMethodName(evidence->refineMethod) + " / " +
                                  refineStatusName(evidence->refineStatus)
                            : "暂无原始精修记录");
  setFact(Fact::Reason,
          evidence ? present(evidence->fallbackReason,
                             evidence->refineStatus == RefineStatus::Fallback
                                 ? "回退状态未保存原因"
                                 : "—；未保存原因")
                   : "暂无原始记录");
  setFact(Fact::Class,
          annotation.classLabel && !annotation.classLabel->empty()
              ? text(*annotation.classLabel) + "（来源记录，非模型识别）"
              : "类别未知");
  setFact(Fact::Format, evidence ? present(evidence->formatId)
                        : source ? present(source->formatId)
                                 : "—");
  setFact(Fact::Origin, evidence ? present(evidence->originRef) : "—");
  setFact(Fact::Coordinates,
          evidence ? present(evidence->coordinateInterpretation)
          : source ? present(source->coordinateInterpretation)
                   : "—");
  QString raw = "annotationId: " + present(annotation.id) +
                "\nsourceKind: " + sourceName(annotation.sourceKind) +
                "\nsourceSetId: " + present(annotation.sourceSetId) +
                "\noriginRecordId: " + present(annotation.originRecordId) +
                "\nrevisionId: " + present(annotation.revisionId);
  if (source)
    raw += "\nprofileId: " + present(source->profileId) +
           "\nsourceHash: " + present(source->sourceHash) +
           "\nsourcePath: " + present(source->originalPath);
  if (evidence) {
    const auto fields = text(evidence->originalFieldsJson);
    raw += "\nformatId: " + present(evidence->formatId) +
           "\noriginRef: " + present(evidence->originRef) +
           "\nrunId: " + present(evidence->runId) +
           "\nparentCandidateId: " + present(evidence->parentCandidateId) +
           "\nrefineMethod: " + refineMethodName(evidence->refineMethod) +
           "\nrefineStatus: " + refineStatusName(evidence->refineStatus) +
           "\nfallbackReason: " + present(evidence->fallbackReason) +
           "\ncoordinateInterpretation: " +
           present(evidence->coordinateInterpretation) +
           "\noriginalFieldsJson:\n" + fields.left(kRawPreviewCharacters);
    if (!evidence->evidenceRefs.empty()) {
      raw += "\nevidenceRefs（仅保存引用，不触发重算或读取）:";
      for (std::size_t index = 0;
           index < std::min(kWindowPreviewCount, evidence->evidenceRefs.size());
           ++index)
        raw += "\n" + text(evidence->evidenceRefs[index]);
      if (evidence->evidenceRefs.size() > kWindowPreviewCount)
        raw += "\n…共 " + count(evidence->evidenceRefs.size()) + " 项引用";
    }
    if (fields.size() > kRawPreviewCharacters)
      raw += "\n…原始字段仅预览前 12000 字符；完整不可变快照保留在工程。";
  } else {
    raw += "\n暂无不可变原始字段；不反推或伪造来源。";
  }
  raw += QString("\nrevision 记录: %1\nreview 记录: %2")
             .arg(count(annotation.revisions.size()),
                  count(annotation.reviewHistory.size()));
  rawFields_->setPlainText(raw);
  rawToggle_->setEnabled(true);
}

void AnnotationEvidenceWidget::showScores(const Annotation& annotation,
                                          bool fixture) {
  const auto* evidence = annotation.originEvidence.get();
  setFact(Fact::Mean,
          evidence ? optionalScore(evidence->scoreMean) : "暂无保存值");
  setFact(Fact::Maximum,
          evidence ? optionalScore(evidence->scoreMax) : "暂无保存值");
  std::unordered_set<std::uint64_t> unique;
  QStringList ids;
  if (evidence) {
    for (const auto id : evidence->windowIds) {
      if (unique.insert(id).second &&
          ids.size() < static_cast<qsizetype>(kWindowPreviewCount))
        ids << count(id);
    }
  }
  QString windows =
      evidence ? count(unique.size()) + " / " + count(evidence->windowCount)
               : "暂无窗口记录";
  if (evidence && !evidence->windowCount) windows += "；分母未提供，比例未知";
  if (evidence && evidence->windowCount &&
      unique.size() > evidence->windowCount)
    windows += "；记录矛盾：唯一命中数大于保存分母";
  if (evidence && unique.size() != evidence->windowIds.size())
    windows += "；原始 IDs 含重复项，命中数按唯一 ID 计数";
  setFact(Fact::Windows, windows);
  if (unique.size() > kWindowPreviewCount)
    ids << QString("…共 %1 个唯一窗口").arg(count(unique.size()));
  setFact(Fact::WindowIds, ids.empty() ? "—" : ids.join(", "));
  if (evidence)
    rawFields_->appendPlainText(
        "scoreMean: " + optionalScore(evidence->scoreMean) +
        "\nscoreMax: " + optionalScore(evidence->scoreMax) +
        "\nuniqueWindowCount: " + count(unique.size()) +
        "\nwindowCount (denominator): " + count(evidence->windowCount));
  QString noise = "暂无噪底、门限、时间能量或轨迹预览数据";
  const auto timeEvidence =
      evidence ? QJsonDocument::fromJson(
                     QByteArray::fromStdString(evidence->originalFieldsJson))
                     .object()["timeAnalysis"]
                     .toObject()
               : QJsonObject{};
  static_cast<MeasuredEvidenceTrace*>(measuredTrace_)
      ->setEvidence(timeEvidence);
  if (timeEvidence["noise"].isDouble())
    noise = (timeEvidence["coarseParametersApplied"].toBool()
                 ? "Coarse 谱密度噪底（dB 域） "
                 : "带限线性功率噪底 ") +
            number(timeEvidence["noise"].toDouble());
  if (timeEvidence["coarseParametersApplied"].toBool() &&
      timeEvidence["threshold"].isDouble())
    noise += "\n实际比较门限（dB 域） " +
             number(timeEvidence["threshold"].toDouble());
  if (timeEvidence["comparisonThreshold"].isDouble())
    noise += "\n实际比较门限 " +
             number(timeEvidence["comparisonThreshold"].toDouble());
  if (timeEvidence["roiSmoothingCells"].toInt() > 0)
    noise += QString("\nROI 自适应门限 · %1 单元平滑")
                 .arg(timeEvidence["roiSmoothingCells"].toInt());
  if (timeEvidence["roiTimeThreshold"].isDouble() &&
      timeEvidence["roiFrequencyThreshold"].isDouble())
    noise = "ROI 正像素轨迹（去均值 dB 域）\n时间比较门限 " +
            number(timeEvidence["roiTimeThreshold"].toDouble()) +
            " · 频率比较门限 " +
            number(timeEvidence["roiFrequencyThreshold"].toDouble()) +
            QString("\n%1 单元平滑；不是线性功率噪底")
                .arg(timeEvidence["roiSmoothingCells"].toInt());
  if (evidence && !evidence->evidenceRefs.empty())
    noise += "；保留 " + count(evidence->evidenceRefs.size()) +
             " 项证据引用，当前卡未加载其内容";
  if (evidence && evidence->estimatedCnrDb)
    noise += "\n保存的 CNR 估计：" + number(*evidence->estimatedCnrDb) +
             " dB（不是噪底，也不是模型分数）";
  setFact(Fact::Noise, noise);
  QString definition =
      "mean / max：对象关联唯一窗口的分数平均 / 最大值；显示保存值，"
      "不补齐或重算缺失明细。分数未校准，不能解释为识别置信度或 CNR。\n";
  definition +=
      fixture ? "分母 windowCount：本次冻结请求范围的全部可分析模型窗口数，"
                "含未命中窗口。"
              : "分母 windowCount：来源保存的窗口统计分母；"
                "当前记录未另存分母范围定义，不能据此推断完整任务覆盖。";
  definition += "唯一窗口命中比例不是信号占空比或任务完成率。";
  if (fixture) definition += "本卡 mean / max 为链路夹具值，不是模型推理。";
  setLabelText(scoreDefinition_, definition);
}

}  // namespace signalstudio
