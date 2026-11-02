#include "ui/detection/ffscn_engine_dialog.h"

#include "ui/parameter_input.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

namespace signalstudio {
namespace {
constexpr int kMinimumFrequencyBins = 2048;
constexpr int kMaximumFrequencyBins = 262144;

QString normalizedPath(const QString& path) {
  return QDir::fromNativeSeparators(QDir::cleanPath(path.trimmed())).toLower();
}

QString absolutePath(const QString& path) {
  const auto trimmed = path.trimmed();
  if (trimmed.isEmpty()) return {};
  return QDir::cleanPath(QFileInfo(trimmed).absoluteFilePath());
}

bool powerOfTwo(int value) {
  return value > 0 && (value & (value - 1)) == 0;
}

QString shapeText(const QJsonArray& shape) {
  QStringList parts;
  for (const auto& value : shape) parts << QString::number(value.toInt());
  return parts.join(", ");
}

class WheelBlocker final : public QObject {
 public:
  using QObject::QObject;

 protected:
  bool eventFilter(QObject*, QEvent* event) override {
    if (event->type() == QEvent::Wheel) {
      event->ignore();
      return true;
    }
    return false;
  }
};

QString defaultPython() {
  const QString preferred = QStringLiteral("D:/profiles/anaconda/python.exe");
  return QFileInfo::exists(preferred) ? preferred : QString{};
}

QString defaultCheckpoint() {
  const QStringList candidates = {
      QStringLiteral("D:/coding/FFSCN_train2/epoch_1200.pth"),
      QStringLiteral(
          "D:/coding/FFSCN_train2/FFSCN_TEST_GUI/config/epoch_1200.pth"),
      QStringLiteral(
          "D:/project/FFSCN_train2/FFSCN_TEST_GUI/config/epoch_1200.pth")};
  for (const auto& path : candidates)
    if (QFileInfo::exists(path)) return path;
  return {};
}
}  // namespace

QStringList ffscnEngineRequestErrors(const FfscnEngineRequest& request) {
  QStringList errors;
  const auto python = absolutePath(request.pythonPath);
  const auto checkpoint = absolutePath(request.checkpointPath);
  const auto trtRoot = absolutePath(request.trtRoot);
  const auto output = absolutePath(request.outputDirectory);
  const auto script = absolutePath(request.scriptPath);
  if (python.isEmpty() || !QFileInfo(python).isFile())
    errors << QString("未找到 Python 解释器：%1")
                  .arg(python.isEmpty() ? request.pythonPath : python);
  if (checkpoint.isEmpty() || !QFileInfo(checkpoint).isFile())
    errors << QString("未找到 checkpoint：%1")
                  .arg(checkpoint.isEmpty() ? request.checkpointPath
                                            : checkpoint);
  if (trtRoot.isEmpty() || !QFileInfo(trtRoot).isDir())
    errors << QString("未找到 TensorRT 目录：%1")
                  .arg(trtRoot.isEmpty() ? request.trtRoot : trtRoot);
  if (script.isEmpty() || !QFileInfo(script).isFile())
    errors << "未找到转换脚本 tools/detection/ffscn_trt_convert.py。";
  if (output.isEmpty()) {
    errors << "请填写输出目录。";
  } else {
    const auto outputNorm = normalizedPath(output);
    const auto parts = outputNorm.split('/');
    if (parts.contains(QStringLiteral("detection-models")) ||
        (!parts.isEmpty() &&
         parts.constLast() == QStringLiteral("ffscn-gui-iq-v1")))
      errors << "输出目录不能位于 detection-models 或已部署的 "
                "ffscn-gui-iq-v1 包内。";
    if (QFileInfo(output).isFile())
      errors << "输出目录不能是一个已有文件。";
    const auto application = normalizedPath(request.applicationDirectory);
    if (!application.isEmpty() && outputNorm == application)
      errors << "输出目录不能是程序所在目录。";
    const auto root = normalizedPath(request.repoRoot);
    if (!root.isEmpty()) {
      const QStringList blocked{"app", "ui", "domain", "infrastructure",
                                "tests", "tools", "docs", "reference",
                                "cmake", "resources"};
      if (outputNorm == root) {
        errors << "输出目录不能是仓库根目录。";
      } else if (outputNorm.startsWith(root + "/")) {
        const auto relative = outputNorm.mid(root.size() + 1);
        const auto head = relative.section('/', 0, 0);
        if (blocked.contains(head))
          errors << QString("输出目录不能位于源码目录 %1。").arg(head);
      }
    }
  }
  if (request.timeFrames < 1 || request.timeFrames > 8192)
    errors << "时间帧 T 必须在 1 到 8192 之间。";
  if (!powerOfTwo(request.frequencyBins) ||
      request.frequencyBins < kMinimumFrequencyBins ||
      request.frequencyBins > kMaximumFrequencyBins)
    errors << "频率点数 F 必须是 2048 到 262144 之间的 2 的幂。";
  if (request.precision != QLatin1String("FP32") &&
      request.precision != QLatin1String("FP16"))
    errors << "精度只能是 FP32 或显式 FP16，不支持 INT8。";
  if (request.workspaceMiB < 1)
    errors << "workspace 至少为 1 MiB。";
  return errors;
}

FfscnEngineDialog::FfscnEngineDialog(QWidget* parent, bool restoreSettings)
    : QDialog(parent), restoreSettings_(restoreSettings) {
  setObjectName("ffscnEngineDialog");
  setWindowTitle("FFSCN 引擎转换");
  setModal(false);
  setWindowModality(Qt::NonModal);
  setMinimumSize(760, 680);
  buildUi();
  inputPolicy_ = new ParameterInputPolicy(this);
  if (restoreSettings_) loadSettings();
  else applyDefaults();
}

QString FfscnEngineDialog::repoRoot() const {
  QDir dir(QCoreApplication::applicationDirPath());
  for (int hop = 0; hop < 8; ++hop) {
    if (QFileInfo::exists(dir.filePath("tools/detection/model_ffscn.py")) &&
        QFileInfo::exists(
            dir.filePath("tools/detection/model_dynamic_package.py")) &&
        QFileInfo::exists(dir.filePath("tools/detection/ffscn_trt_convert.py")))
      return QDir::cleanPath(dir.absolutePath());
    if (!dir.cdUp()) break;
  }
  return {};
}

QString FfscnEngineDialog::scriptPath() const {
  const auto root = repoRoot();
  if (root.isEmpty()) return {};
  return QDir(root).filePath("tools/detection/ffscn_trt_convert.py");
}

void FfscnEngineDialog::buildUi() {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(18, 16, 18, 14);
  outer->setSpacing(10);
  auto* help = new QLabel(
      "将冻结的 epoch-1200 FFSCN checkpoint（.pth）转换为 TensorRT "
      "引擎，并在固定的时间帧 T 和频率点数 F 上探测实际能完成 setInputShape / "
      "enqueue 的最大 batch。候选为 1、2、4、8、16。这是运行时能力探测，不是吞吐"
      "基准，也不表示端到端检测变快或检测更准。默认精度为 FP32；FP16 "
      "必须显式选择；不支持 INT8。频率点数保持 2 的幂且至少 2048，网络 "
      "down_nums=11。引擎只写入所选输出目录，不会替换已部署的 detection-models "
      "包。推理设备是 NVIDIA GeForce RTX 5060（SM 12.0），不使用 AMD "
      "显示 GPU。若 TensorRT 或 CUDA 无法构建，下面会显示具体错误。"
      "要让下次检测使用新引擎，请在工具菜单中选择「切换 FFSCN 引擎」；"
      "本窗口不会替换已部署的包。");
  help->setObjectName("ffscnEngineHelp");
  help->setWordWrap(true);
  help->setProperty("uiRole", "help");
  outer->addWidget(help);

  auto* form = new QFormLayout;
  form->setSpacing(8);
  form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  const auto pathRow = [this](ParameterTextEdit* edit, const QString& name,
                              const QString& title, bool directory,
                              const QString& filter) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    edit->setObjectName(name);
    layout->addWidget(edit, 1);
    auto* browse = new QPushButton("浏览");
    browse->setAutoDefault(false);
    browse->setDefault(false);
    layout->addWidget(browse);
    connect(browse, &QPushButton::clicked, this,
            [this, edit, directory, title, filter] {
      QString selected;
      if (directory) {
        selected = QFileDialog::getExistingDirectory(this, title, edit->text());
      } else {
        selected = QFileDialog::getOpenFileName(this, title, edit->text(),
                                                filter);
      }
      if (selected.isEmpty()) return;
      edit->setProperty("parameterDraft", false);
      edit->displayText(QDir::toNativeSeparators(selected));
    });
    return row;
  };
  python_ = new ParameterTextEdit;
  checkpoint_ = new ParameterTextEdit;
  trtRoot_ = new ParameterTextEdit;
  output_ = new ParameterTextEdit;
  form->addRow("Python 解释器",
               pathRow(python_, "ffscnPythonPath", "选择 Python 解释器", false,
                       "Python (python.exe);;All files (*.*)"));
  form->addRow("Checkpoint (.pth)",
               pathRow(checkpoint_, "ffscnCheckpointPath",
                       "选择 FFSCN checkpoint", false,
                       "PyTorch checkpoint (*.pth);;All files (*.*)"));
  form->addRow("TensorRT 目录",
               pathRow(trtRoot_, "ffscnTrtRoot", "选择 TensorRT 根目录", true,
                       {}));
  form->addRow("输出目录",
               pathRow(output_, "ffscnOutputDir", "选择引擎输出目录", true,
                       {}));

  timeFrames_ = new ParameterIntSpinBox;
  timeFrames_->setObjectName("ffscnTimeFrames");
  timeFrames_->setRange(1, 8192);
  timeFrames_->setValue(10);
  timeFrames_->setToolTip("固定时间帧 T。滚轮不会改值。");
  form->addRow("时间帧 T", timeFrames_);

  frequencyBins_ = new QComboBox;
  frequencyBins_->setObjectName("ffscnFrequencyBins");
  for (int bins = kMinimumFrequencyBins; bins <= kMaximumFrequencyBins;
       bins *= 2)
    frequencyBins_->addItem(QString::number(bins), bins);
  frequencyBins_->setCurrentIndex(frequencyBins_->findData(65536));
  frequencyBins_->setToolTip("固定频率点数 F，必须是不小于 2048 的 2 的幂。");
  frequencyBins_->installEventFilter(new WheelBlocker(frequencyBins_));
  form->addRow("频率点数 F", frequencyBins_);

  precision_ = new QComboBox;
  precision_->setObjectName("ffscnPrecision");
  precision_->addItem("FP32（默认，正式检测使用 FP32）", "FP32");
  precision_->addItem("FP16（显式选项，不写入正式检测包）", "FP16");
  precision_->installEventFilter(new WheelBlocker(precision_));
  form->addRow("精度", precision_);

  workspace_ = new ParameterIntSpinBox;
  workspace_->setObjectName("ffscnWorkspaceMiB");
  workspace_->setRange(256, 8192);
  workspace_->setSingleStep(256);
  workspace_->setValue(1024);
  workspace_->setSuffix(" MiB");
  form->addRow("构建 workspace", workspace_);
  outer->addLayout(form);

  auto* buttons = new QHBoxLayout;
  validateButton_ = new QPushButton("校验输入");
  validateButton_->setObjectName("ffscnValidateButton");
  convertButton_ = new QPushButton("转换并探测");
  convertButton_->setObjectName("ffscnConvertButton");
  convertButton_->setProperty("uiRole", "primary");
  cancelButton_ = new QPushButton("取消转换");
  cancelButton_->setObjectName("ffscnCancelButton");
  cancelButton_->setEnabled(false);
  auto* close = new QPushButton("关闭");
  close->setObjectName("ffscnCloseButton");
  for (auto* button : {validateButton_, convertButton_, cancelButton_, close}) {
    button->setAutoDefault(false);
    button->setDefault(false);
    buttons->addWidget(button);
  }
  buttons->addStretch();
  outer->addLayout(buttons);
  connect(validateButton_, &QPushButton::clicked, this,
          &FfscnEngineDialog::validateInputs);
  connect(convertButton_, &QPushButton::clicked, this,
          &FfscnEngineDialog::startConversion);
  connect(cancelButton_, &QPushButton::clicked, this,
          &FfscnEngineDialog::cancelConversion);
  connect(close, &QPushButton::clicked, this, &QDialog::reject);

  summary_ = new QLabel("尚未开始。正式检测包不会被替换。");
  summary_->setObjectName("ffscnSummary");
  summary_->setWordWrap(true);
  summary_->setProperty("uiRole", "summary");
  summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  outer->addWidget(summary_);
  log_ = new QPlainTextEdit;
  log_->setObjectName("ffscnLog");
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(4000);
  log_->setPlaceholderText("转换日志");
  outer->addWidget(log_, 1);
  setStyleSheet(
      "QDialog { background:#101d2f; color:#dbe8f8; }"
      "QLabel { color:#d5e6f5; background:transparent; }"
      "QLabel[uiRole=\"help\"] { color:#8eabc4; }"
      "QLabel[uiRole=\"summary\"] { color:#d7ecff; background:#0e1c2e; "
      "border:1px solid #2c4a66; border-radius:4px; padding:8px; }"
      "QLineEdit, QComboBox { background:#0e1a2a; color:#dbe8f8; "
      "border:1px solid #314963; border-radius:4px; padding:4px 6px; }"
      "QPlainTextEdit { background:#0c1726; color:#c5d7ea; "
      "border:1px solid #2a445c; font-family:Consolas,monospace; }"
      "QPushButton { background:#1b2d41; color:#d3e3f1; "
      "border:1px solid #456078; border-radius:4px; padding:6px 12px; }"
      "QPushButton:hover { background:#294963; }"
      "QPushButton:disabled { color:#8799aa; background:#182331; }"
      "QPushButton[uiRole=\"primary\"] { background:#1a4e76; color:#bdebff; }");
}

void FfscnEngineDialog::applyDefaults() {
  python_->displayText(defaultPython());
  checkpoint_->displayText(defaultCheckpoint());
  const auto root = repoRoot();
  const auto trt = root.isEmpty()
                       ? QString{}
                       : QDir(root).filePath(
                             "out/tasks/wideband-detection/M3/assets/runtime/"
                             "tensorrt-10.11");
  trtRoot_->displayText(QFileInfo::exists(trt) ? trt : QString{});
  output_->displayText(
      root.isEmpty()
          ? QString{}
          : QDir(root).filePath("out/tasks/ux-r6/ffscn-engine-tool"));
  timeFrames_->setValue(10);
  const int frequency = frequencyBins_->findData(65536);
  if (frequency >= 0) frequencyBins_->setCurrentIndex(frequency);
  precision_->setCurrentIndex(0);
  workspace_->setValue(1024);
}

void FfscnEngineDialog::loadSettings() {
  applyDefaults();
  QSettings settings;
  settings.beginGroup("ffscnEngineTool");
  const auto textOr = [&](ParameterTextEdit* edit, const QString& key) {
    const auto value = settings.value(key).toString();
    if (!value.isEmpty()) edit->displayText(value);
  };
  textOr(python_, "python");
  textOr(checkpoint_, "checkpoint");
  textOr(trtRoot_, "trtRoot");
  textOr(output_, "outputDir");
  if (settings.contains("timeFrames"))
    timeFrames_->setValue(settings.value("timeFrames").toInt());
  if (settings.contains("frequencyBins")) {
    const int index =
        frequencyBins_->findData(settings.value("frequencyBins").toInt());
    if (index >= 0) frequencyBins_->setCurrentIndex(index);
  }
  const auto precision = settings.value("precision").toString();
  const int precisionIndex = precision_->findData(precision);
  if (precisionIndex >= 0) precision_->setCurrentIndex(precisionIndex);
  if (settings.contains("workspaceMiB"))
    workspace_->setValue(settings.value("workspaceMiB").toInt());
  settings.endGroup();
}

void FfscnEngineDialog::saveSettings() const {
  if (!restoreSettings_) return;
  QSettings settings;
  settings.beginGroup("ffscnEngineTool");
  settings.setValue("python", python_->text());
  settings.setValue("checkpoint", checkpoint_->text());
  settings.setValue("trtRoot", trtRoot_->text());
  settings.setValue("outputDir", output_->text());
  settings.setValue("timeFrames", timeFrames_->value());
  settings.setValue("frequencyBins", frequencyBins_->currentData().toInt());
  settings.setValue("precision", precision_->currentData().toString());
  settings.setValue("workspaceMiB", workspace_->value());
  settings.endGroup();
}

void FfscnEngineDialog::commitVisibleDrafts() {
  for (auto* widget : findChildren<QWidget*>())
    if (auto* draft = dynamic_cast<ParameterDraft*>(widget)) draft->commit();
}

FfscnEngineRequest FfscnEngineDialog::currentRequest() const {
  FfscnEngineRequest request;
  request.pythonPath = python_->text();
  request.checkpointPath = checkpoint_->text();
  request.trtRoot = trtRoot_->text();
  request.outputDirectory = output_->text();
  request.repoRoot = repoRoot();
  request.scriptPath = scriptPath();
  request.applicationDirectory = QCoreApplication::applicationDirPath();
  request.timeFrames = timeFrames_->value();
  request.frequencyBins = frequencyBins_->currentData().toInt();
  request.precision = precision_->currentData().toString();
  request.workspaceMiB = workspace_->value();
  return request;
}

QStringList FfscnEngineDialog::requestErrors() const {
  auto errors = ffscnEngineRequestErrors(currentRequest());
  if (timeFrames_->property("parameterDraft").toBool() ||
      workspace_->property("parameterDraft").toBool())
    errors.prepend(
        "有数值尚未提交。请按 Enter、Tab，或点击其它位置后再继续。");
  return errors;
}

QString FfscnEngineDialog::summaryText() const { return summary_->text(); }

bool FfscnEngineDialog::conversionRunning() const { return busy_; }

int FfscnEngineDialog::startedProcesses() const { return startedProcesses_; }

void FfscnEngineDialog::validateInputs() {
  commitVisibleDrafts();
  startProcess(false);
}

void FfscnEngineDialog::startConversion() {
  commitVisibleDrafts();
  startProcess(true);
}

void FfscnEngineDialog::startProcess(bool convert) {
  if (busy_) return;
  const auto errors = requestErrors();
  if (!errors.isEmpty()) {
    const auto text = errors.join("\n");
    summary_->setText(text + "\n未开始转换。正式检测包未被替换。");
    appendLog(text);
    return;
  }
  const auto request = currentRequest();
  const auto output = absolutePath(request.outputDirectory);
  reportPath_ = QDir(output).filePath("conversion-report.json");
  const auto script = absolutePath(request.scriptPath);
  QStringList arguments{
      script,
      "--checkpoint",
      absolutePath(request.checkpointPath),
      "--trt-root",
      absolutePath(request.trtRoot),
      "--output",
      output,
      "--report",
      reportPath_,
      "--time-frames",
      QString::number(request.timeFrames),
      "--frequency-bins",
      QString::number(request.frequencyBins),
      "--precision",
      request.precision,
      "--workspace-mib",
      QString::number(request.workspaceMiB)};
  if (!convert) arguments << "--validate-only" << "--check-toolchain";
  if (!process_) {
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    process_->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) {
          args->flags |= 0x08000000;  // CREATE_NO_WINDOW
        });
#endif
    connect(process_, &QProcess::readyRead, this,
            &FfscnEngineDialog::consumeProcessOutput);
    connect(process_, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus status) {
              finishConversion(exitCode, static_cast<int>(status));
            });
    connect(process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
              appendLog(process_->errorString());
              if (error == QProcess::FailedToStart) {
                setBusy(false);
                summary_->setText(
                    "未能启动 Python。正式检测包未被替换。\n" +
                    process_->errorString());
              }
            });
  }
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("PYTHONUTF8", "1");
  environment.insert("PYTHONIOENCODING", "utf-8");
  environment.insert("PYTHONUNBUFFERED", "1");
  process_->setProcessEnvironment(environment);
  process_->setWorkingDirectory(QFileInfo(script).absolutePath());
  process_->setProgram(absolutePath(request.pythonPath));
  process_->setArguments(arguments);
  logRemainder_.clear();
  appendLog(convert ? "开始转换并探测。" : "开始校验工具链。");
  appendLog(QString("解释器：%1").arg(process_->program()));
  appendLog(QString("脚本：%1").arg(script));
  summary_->setText(
      convert ? "正在转换并探测。构建可能需要较长时间。正式检测包不会被替换。\n"
                 "输出目录：" +
                   output
               : "正在校验解释器、checkpoint 和 TensorRT。尚未构建引擎。");
  saveSettings();
  setBusy(true);
  ++startedProcesses_;
  process_->start();
}

void FfscnEngineDialog::cancelConversion() {
  if (!process_ || process_->state() == QProcess::NotRunning) return;
  appendLog("正在取消转换。已写出的正式检测包不会被改动。");
  process_->kill();
}

void FfscnEngineDialog::consumeProcessOutput() {
  if (!process_) return;
  logRemainder_ += QString::fromUtf8(process_->readAll());
  int newline = -1;
  while ((newline = logRemainder_.indexOf('\n')) >= 0) {
    auto line = logRemainder_.left(newline);
    if (line.endsWith('\r')) line.chop(1);
    logRemainder_.remove(0, newline + 1);
    log_->appendPlainText(line);
  }
}

void FfscnEngineDialog::finishConversion(int exitCode, int status) {
  consumeProcessOutput();
  if (!logRemainder_.isEmpty()) {
    log_->appendPlainText(logRemainder_);
    logRemainder_.clear();
  }
  setBusy(false);
  appendLog(QString("进程结束，exitCode=%1，status=%2")
                .arg(exitCode)
                .arg(status == static_cast<int>(QProcess::NormalExit)
                         ? "normal"
                         : "crash"));
  showReport(exitCode, status);
}

void FfscnEngineDialog::showReport(int exitCode, int status) {
  QFile file(reportPath_);
  if (!file.open(QIODevice::ReadOnly)) {
    summary_->setText(
        QString("转换进程已结束（exit %1），但没有读到报告。请看日志中的具体"
                "错误。正式检测包未被替换。")
            .arg(exitCode));
    return;
  }
  const auto document = QJsonDocument::fromJson(file.readAll());
  if (!document.isObject()) {
    summary_->setText(
        "报告不是有效 JSON。请看日志中的具体错误。正式检测包未被替换。");
    return;
  }
  const auto report = document.object();
  const auto error = report["error"].toString();
  const auto mode = report["mode"].toString();
  QString text;
  if (mode == "validate-only" && report["ok"].toBool()) {
    const auto device = report["device"].toObject();
    text = "校验通过：解释器、冻结 checkpoint 和 TensorRT 可用。尚未构建引擎，"
           "也没有替换正式检测包。";
    const auto capability = device["computeCapability"].toArray();
    if (!device.isEmpty() && capability.size() >= 2)
      text += QString("\n设备：%1，SM %2.%3，TensorRT %4，PyTorch CUDA %5")
                  .arg(device["name"].toString())
                  .arg(capability.at(0).toInt())
                  .arg(capability.at(1).toInt())
                  .arg(report["tensorRt"].toString())
                  .arg(report["torchCuda"].toString());
  } else if (report["built"].toBool() && report["probe"].isObject()) {
    const auto probe = report["probe"].toObject();
    const auto winner = probe["maximumRunnableBatch"];
    QStringList attempts;
    for (const auto& value : probe["attempts"].toArray()) {
      const auto item = value.toObject();
      attempts << QString("batch %1 形状 [%2] %3")
                      .arg(item["batch"].toInt())
                      .arg(shapeText(item["shape"].toArray()))
                      .arg(item["ok"].toBool() ? "成功" : "失败");
    }
    const auto next = probe["nextTier"].toObject();
    text = QString("引擎：%1\n精度：%2，固定 T=%3，F=%4，down_nums=11\n"
                   "最大可运行 batch：%5\n已尝试：%6")
               .arg(report["enginePath"].toString())
               .arg(report["precision"].toString())
               .arg(report["timeFrames"].toInt())
               .arg(report["frequencyBins"].toInt())
               .arg(winner.isDouble() ? QString::number(winner.toInt())
                                      : QString("无"))
               .arg(attempts.join("；"));
    if (!next.isEmpty())
      text += QString("\n下一档 batch=%1 失败原因：%2")
                  .arg(next["batch"].toInt())
                  .arg(next["error"].toString());
    else if (winner.toInt() == 16)
      text += "\n候选 16 已成功，没有更高一档。";
    const auto device = report["device"].toObject();
    const auto capability = device["computeCapability"].toArray();
    if (!device.isEmpty() && capability.size() >= 2)
      text += QString("\n设备：%1（SM %2.%3）")
                  .arg(device["name"].toString())
                  .arg(capability.at(0).toInt())
                  .arg(capability.at(1).toInt());
    text += "\n这是运行时能力探测，不是吞吐基准，也不表示端到端检测变快或检测"
            "更准。正式检测包未被替换。";
  } else if (!error.isEmpty()) {
    text = error + "\n正式检测包未被替换。";
  } else {
    text = QString("引擎尚未生成（exit %1）。请查看日志中的具体错误。正式检测包"
                   "未被替换。")
               .arg(exitCode);
  }
  if (status != static_cast<int>(QProcess::NormalExit))
    text.prepend("转换进程异常退出。\n");
  summary_->setText(text);
}

void FfscnEngineDialog::appendLog(const QString& text) {
  if (!text.isEmpty()) log_->appendPlainText(text);
}

void FfscnEngineDialog::setBusy(bool busy) {
  busy_ = busy;
  validateButton_->setEnabled(!busy);
  convertButton_->setEnabled(!busy);
  cancelButton_->setEnabled(busy);
}

void FfscnEngineDialog::closeEvent(QCloseEvent* event) {
  if (busy_ && process_ && process_->state() != QProcess::NotRunning) {
    process_->kill();
    process_->waitForFinished(2000);
  }
  if (process_) disconnect(process_, nullptr, this, nullptr);
  commitVisibleDrafts();
  saveSettings();
  QDialog::closeEvent(event);
}
}  // namespace signalstudio
