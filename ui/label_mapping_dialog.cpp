#include "ui/label_mapping_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QVBoxLayout>
#include <cmath>

#include "ui/parameter_input.h"

namespace signalstudio {
LabelMappingDialog::LabelMappingDialog(const QString& path, const QString& preview,
                                       QWidget* parent) : QDialog(parent) {
  setObjectName("labelMappingDialog");
  setWindowTitle("TXT 标签字段映射");
  resize(780, 850);
  new ParameterInputPolicy(this);
  auto* layout = new QVBoxLayout(this);
  auto* description = new QLabel("已找到同名 TXT，字段含义尚未确认。请选择实际列、单位和区间规则。\n"
                                 "列号从 1 起，0 表示未知。时间未知仅保存频率观察；频率未知保留原文，不生成标记。\n" + path, this);
  description->setWordWrap(true);
  description->setTextFormat(Qt::PlainText);
  layout->addWidget(description);
  auto* raw = new QPlainTextEdit(this);
  raw->setObjectName("labelMappingPreview");
  raw->setReadOnly(true);
  raw->setPlainText(preview.left(65536));
  raw->setMaximumHeight(180);
  layout->addWidget(raw);
  auto* area = new QScrollArea(this);
  area->setWidgetResizable(true);
  auto* panel = new QWidget(area);
  auto* form = new QFormLayout(panel);
  area->setWidget(panel);
  layout->addWidget(area);
  const auto combo = [&](const char* name, const QString& title, const QStringList& choices) {
    auto* value = new QComboBox(panel);
    value->setObjectName(name);
    value->addItems(choices);
    form->addRow(title, value);
    return value;
  };
  delimiter_ = combo("labelDelimiter", "分隔符", {"请选择", "空白", "Tab", "逗号", "分号"});
  const auto column = [&](const char* name, const QString& title, int* output) {
    auto* value = new ParameterIntSpinBox(panel);
    value->setObjectName(name);
    value->setRange(0, 256);
    value->setSpecialValueText("未知");
    connect(value, &QSpinBox::valueChanged, this, [output](int v) { *output = v - 1; });
    form->addRow(title, value);
  };
  auto* skip = new ParameterIntSpinBox(panel);
  skip->setObjectName("labelSkipLines");
  skip->setRange(0, 1000000);
  connect(skip, &QSpinBox::valueChanged, this, [this](int n) { draft_.skipLines = n; });
  form->addRow("跳过开头物理行数", skip);
  comment_ = new ParameterTextEdit(panel);
  comment_->setObjectName("labelCommentPrefix");
  comment_->setMaxLength(32);
  comment_->apply = [this](const QString& text) { draft_.commentPrefix = text; return true; };
  form->addRow("注释前缀（空为无）", comment_);
  column("labelClassColumn", "类别列", &draft_.classColumn);
  timeUnit_ = combo("labelTimeUnit", "时间坐标", {"请选择", "未知", "样本", "秒", "帧"});
  column("labelStartColumn", "时间起点列", &draft_.startColumn);
  column("labelEndColumn", "时间终点 / 数量列", &draft_.endColumn);
  interval_ = combo("labelInterval", "时间区间含义", {"请选择", "[起点, 终点)", "[起点, 终点]（样本/帧）", "起点 + 数量/时长"});
  origin_ = combo("labelIndexOrigin", "索引起点", {"请选择", "从 0 起（秒也选此项）", "从 1 起"});
  const auto uintEditor = [&](const char* name, const QString& title, std::uint64_t* output) {
    auto* value = new ParameterTextEdit(panel);
    value->setObjectName(name);
    value->setMaxLength(20);
    value->displayText("0");
    value->apply = [value, output](const QString& text) {
      bool ok = false;
      const auto v = text.toULongLong(&ok);
      ok = ok && QRegularExpression("^(0|[1-9][0-9]{0,19})$").match(text).hasMatch();
      if (!ok) { setParameterError(value, "请输入 uint64 整数字面量"); return false; }
      *output = v;
      return true;
    };
    form->addRow(title, value);
    return value;
  };
  hop_ = uintEditor("labelFrameHop", "帧步长（样本，帧坐标必填）", &draft_.frameHopSamples);
  length_ = uintEditor("labelFrameLength", "帧长（样本，帧坐标必填）", &draft_.frameLengthSamples);
  auto* rounding = new QCheckBox("允许秒坐标按最近样本舍入（证据记录误差）", panel);
  rounding->setObjectName("labelAllowRounding");
  connect(rounding, &QCheckBox::toggled, this, [this](bool v) { draft_.allowTimeRounding = v; });
  form->addRow(rounding);
  quantization_ = new ParameterTextEdit(panel);
  quantization_->setObjectName("labelTimeQuantization");
  quantization_->setMaxLength(32);
  quantization_->displayText("0");
  quantization_->apply = [this](const QString& text) {
    bool ok = false;
    const double v = text.toDouble(&ok);
    if (!ok || !std::isfinite(v) || v < 0 || v > 1e6) {
      setParameterError(quantization_, "请输入 0 到 1e6 的有限秒数"); return false;
    }
    draft_.timeQuantizationSeconds = v;
    return true;
  };
  form->addRow("原文时间量化步长（秒，0 为未声明）", quantization_);
  frequencyLayout_ = combo("labelFrequencyLayout", "频率字段", {"请选择", "未知", "频率下界 / 上界", "中心频率 / 带宽"});
  column("labelLowerColumn", "频率下界 / 中心列", &draft_.lowerColumn);
  column("labelUpperColumn", "频率上界 / 带宽列", &draft_.upperColumn);
  frequencyUnit_ = combo("labelFrequencyUnit", "频率单位", {"请选择", "Hz", "kHz", "MHz", "GHz"});
  frequencyReference_ = combo("labelFrequencyReference", "频率基准", {"请选择", "绝对 RF（不加 Fc）", "基带偏移（加一次 Fc）"});
  error_ = new QLabel(this);
  error_->setObjectName("labelMappingError");
  error_->setWordWrap(true);
  error_->setTextFormat(Qt::PlainText);
  layout->addWidget(error_);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  buttons->button(QDialogButtonBox::Ok)->setText("确认映射并预检");
  buttons->button(QDialogButtonBox::Ok)->setDefault(false);
  buttons->button(QDialogButtonBox::Ok)->setAutoDefault(false);
  connect(buttons, &QDialogButtonBox::accepted, this, &LabelMappingDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

void LabelMappingDialog::accept() {
  // Applying is explicit. Invalid drafts remain visible and block acceptance.
  for (auto* widget : findChildren<QWidget*>()) {
    if (auto* parameter = dynamic_cast<ParameterDraft*>(widget)) parameter->commit();
    if (widget->property("parameterDraft").toBool()) {
      error_->setText("请修正无效字段。"); return;
    }
  }
  draft_.delimiter = static_cast<LabelDelimiter>(delimiter_->currentIndex());
  draft_.timeUnit = static_cast<LabelTimeUnit>(timeUnit_->currentIndex());
  draft_.interval = static_cast<LabelInterval>(interval_->currentIndex());
  draft_.indexOrigin = origin_->currentIndex() - 1;
  draft_.frequencyLayout = static_cast<LabelFrequencyLayout>(frequencyLayout_->currentIndex());
  draft_.frequencyUnit = static_cast<LabelFrequencyUnit>(frequencyUnit_->currentIndex());
  draft_.frequencyReference = static_cast<LabelFrequencyReference>(frequencyReference_->currentIndex());
  if (draft_.delimiter == LabelDelimiter::Unspecified || draft_.classColumn < 0 ||
      draft_.timeUnit == LabelTimeUnit::Unspecified ||
      draft_.frequencyLayout == LabelFrequencyLayout::Unspecified) {
    error_->setText("请明确分隔符、类别列、时间和频率字段含义。"); return;
  }
  if (draft_.timeUnit != LabelTimeUnit::Unknown &&
      (draft_.startColumn < 0 || draft_.endColumn < 0 || draft_.indexOrigin < 0 ||
       draft_.interval == LabelInterval::Unspecified ||
       (draft_.timeUnit == LabelTimeUnit::Frames &&
        (!draft_.frameHopSamples || !draft_.frameLengthSamples)))) {
    error_->setText("请补全时间列、索引起点、区间规则和帧参数。"); return;
  }
  if (draft_.frequencyLayout != LabelFrequencyLayout::Unknown &&
      (draft_.lowerColumn < 0 || draft_.upperColumn < 0 ||
       draft_.frequencyUnit == LabelFrequencyUnit::Unspecified ||
       draft_.frequencyReference == LabelFrequencyReference::Unspecified)) {
    error_->setText("请补全频率列、单位及频率基准。"); return;
  }
  accepted_ = draft_;
  QDialog::accept();
}
}  // namespace signalstudio
