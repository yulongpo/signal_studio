#include "ui/detection/ffscn_engine_switch_dialog.h"

#include <QDir>
#include <utility>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui/parameter_input.h"

namespace signalstudio {
namespace {
QString shaText(const QString& value) {
  return value.isEmpty() ? QStringLiteral("（清单未记录）") : value;
}
}  // namespace

FfscnEngineSwitchDialog::FfscnEngineSwitchDialog(QWidget* parent,
                                                 QString modelRoot)
    : QDialog(parent),
      modelRoot_(modelRoot.isEmpty() ? detectionModelsRoot()
                                     : std::move(modelRoot)) {
  setObjectName("ffscnEngineSwitchDialog");
  setWindowTitle("切换 FFSCN 引擎");
  setModal(true);
  setMinimumSize(720, 460);
  buildUi();
  inputPolicy_ = new ParameterInputPolicy(this);
  refreshCurrentPackage();
}

void FfscnEngineSwitchDialog::setBusyCheck(std::function<bool()> busy) {
  busyCheck_ = std::move(busy);
}

void FfscnEngineSwitchDialog::buildUi() {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(18, 16, 18, 14);
  outer->setSpacing(10);
  auto* help = new QLabel(
      "选择包含 manifest.json 和引擎文件的 FFSCN 包目录。"
      "确认后，下次检测会使用该包；已提交的检测结果会保留，"
      "直到一次完整的新检测成功后才替换。检测进行中不能切换，"
      "正在运行的任务也不会改用新引擎。SCN 包不能从这里加载。"
      "转换窗口只把引擎写到所选输出目录，不会自动替换活动包。"
      "FP16 以及不符合 FP32、down_nums=11 和频率长度契约的清单会被拒绝。");
  help->setObjectName("ffscnSwitchHelp");
  help->setWordWrap(true);
  help->setProperty("uiRole", "help");
  outer->addWidget(help);

  current_ = new QLabel;
  current_->setObjectName("ffscnSwitchCurrent");
  current_->setWordWrap(true);
  current_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  current_->setProperty("uiRole", "summary");
  outer->addWidget(current_);

  auto* form = new QFormLayout;
  form->setSpacing(8);
  auto* row = new QWidget;
  auto* rowLayout = new QHBoxLayout(row);
  rowLayout->setContentsMargins(0, 0, 0, 0);
  rowLayout->setSpacing(6);
  directory_ = new ParameterTextEdit;
  directory_->setObjectName("ffscnSwitchDirectory");
  directory_->setPlaceholderText("包含 manifest.json 的引擎目录");
  rowLayout->addWidget(directory_, 1);
  auto* browse = new QPushButton("浏览…");
  browse->setObjectName("ffscnSwitchBrowse");
  browse->setAutoDefault(false);
  browse->setDefault(false);
  rowLayout->addWidget(browse);
  connect(browse, &QPushButton::clicked, this, [this] {
    const auto selected = QFileDialog::getExistingDirectory(
        this, "选择 FFSCN 引擎目录", directory_->text());
    if (selected.isEmpty()) return;
    directory_->setProperty("parameterDraft", false);
    directory_->displayText(QDir::toNativeSeparators(selected));
    summary_->setText("已选择目录。请先检查，再确认切换。");
  });
  form->addRow("引擎目录", row);
  outer->addLayout(form);

  auto* buttons = new QHBoxLayout;
  auto* inspect = new QPushButton("检查");
  inspect->setObjectName("ffscnSwitchInspect");
  auto* apply = new QPushButton("切换");
  apply->setObjectName("ffscnSwitchApply");
  apply->setProperty("uiRole", "primary");
  auto* restore = new QPushButton("恢复已部署的包");
  restore->setObjectName("ffscnSwitchRestore");
  auto* close = new QPushButton("关闭");
  close->setObjectName("ffscnSwitchClose");
  for (auto* button : {inspect, apply, restore, close}) {
    button->setAutoDefault(false);
    button->setDefault(false);
    buttons->addWidget(button);
  }
  buttons->addStretch();
  outer->addLayout(buttons);
  connect(inspect, &QPushButton::clicked, this,
          &FfscnEngineSwitchDialog::inspectSelection);
  connect(apply, &QPushButton::clicked, this,
          &FfscnEngineSwitchDialog::activateSelection);
  connect(restore, &QPushButton::clicked, this,
          &FfscnEngineSwitchDialog::restoreDeployedPackage);
  connect(close, &QPushButton::clicked, this, &QDialog::reject);

  summary_ = new QLabel("尚未检查。活动引擎不会在检测进行时更换。");
  summary_->setObjectName("ffscnSwitchSummary");
  summary_->setWordWrap(true);
  summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  summary_->setProperty("uiRole", "summary");
  outer->addWidget(summary_, 1);
  setStyleSheet(
      "QDialog { background:#101d2f; color:#dbe8f8; }"
      "QLabel { color:#d5e6f5; background:transparent; }"
      "QLabel[uiRole=\"help\"] { color:#8eabc4; }"
      "QLabel[uiRole=\"summary\"] { color:#d7ecff; background:#0e1c2e; "
      "border:1px solid #2c4a66; border-radius:4px; padding:8px; }"
      "QLineEdit { background:#0e1a2a; color:#dbe8f8; "
      "border:1px solid #314963; border-radius:4px; padding:4px 6px; }"
      "QPushButton { background:#1b2d41; color:#d3e3f1; "
      "border:1px solid #456078; border-radius:4px; padding:6px 12px; }"
      "QPushButton:hover { background:#294963; }"
      "QPushButton[uiRole=\"primary\"] { background:#1a4e76; color:#bdebff; }");
}

void FfscnEngineSwitchDialog::refreshCurrentPackage() {
  FfscnActivePackage active;
  QString error;
  if (!readFfscnActivePackage(modelRoot_, active, error)) {
    current_->setText(error);
    return;
  }
  if (!active.explicitSelection) {
    current_->setText("当前使用已部署目录：\n" + active.packageDirectory +
                      "\n尚未另行切换。");
    return;
  }
  current_->setText("当前活动目录：\n" + active.packageDirectory +
                    "\n引擎 SHA-256：\n" + shaText(active.engineSha256) +
                    "\n权重 SHA-256：\n" + shaText(active.checkpointSha256));
}

void FfscnEngineSwitchDialog::commitVisibleDrafts() {
  for (auto* widget : findChildren<QWidget*>())
    if (auto* draft = dynamic_cast<ParameterDraft*>(widget)) draft->commit();
}

bool FfscnEngineSwitchDialog::detectionBusy() const {
  return busyCheck_ && busyCheck_();
}

void FfscnEngineSwitchDialog::refuseWhileBusy() {
  QMessageBox::warning(
      this, "暂不能切换 FFSCN 引擎",
      "检测仍在进行。请等待当前任务结束或取消完成后再切换。"
      "正在运行的任务不会改用新引擎。");
}

void FfscnEngineSwitchDialog::inspectSelection() {
  commitVisibleDrafts();
  FfscnActivePackage inspected;
  QString error;
  if (!inspectFfscnEnginePackage(directory_->text(), inspected, error)) {
    summary_->setText(error);
    return;
  }
  summary_->setText("可以切换。\n目录：\n" + inspected.packageDirectory +
                    "\n引擎 SHA-256：\n" + inspected.engineSha256 +
                    "\n权重 SHA-256：\n" + shaText(inspected.checkpointSha256) +
                    "\n下次检测将使用该包。已有检测结果不会被清除。");
}

void FfscnEngineSwitchDialog::activateSelection() {
  commitVisibleDrafts();
  if (detectionBusy()) {
    refuseWhileBusy();
    return;
  }
  confirmAndActivate(directory_->text(), "确认切换 FFSCN 引擎",
                     "这只更改活动引擎配置。");
}

void FfscnEngineSwitchDialog::restoreDeployedPackage() {
  if (detectionBusy()) {
    refuseWhileBusy();
    return;
  }
  confirmAndActivate(QDir(modelRoot_).filePath("ffscn-gui-iq-v1"),
                     "恢复已部署的 FFSCN 包",
                     "活动路径将改回程序目录中的 ffscn-gui-iq-v1。");
}

bool FfscnEngineSwitchDialog::confirmAndActivate(const QString& directory,
                                                 const QString& title,
                                                 const QString& detail) {
  FfscnActivePackage inspected;
  QString error;
  if (!inspectFfscnEnginePackage(directory, inspected, error)) {
    summary_->setText(error);
    return false;
  }
  const auto answer = QMessageBox::question(
      this, title,
      QString("下次检测将使用：\n%1\n\n引擎 SHA-256：\n%2\n权重 SHA-256：\n%3\n\n"
              "%4已提交的检测结果会保留，直到一次完整的新检测成功后才替换。"
              "SCN 包不会改变，原来的检测包文件也不会删除。")
          .arg(inspected.packageDirectory, inspected.engineSha256,
               shaText(inspected.checkpointSha256), detail),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (answer != QMessageBox::Yes) return false;
  if (detectionBusy()) {
    refuseWhileBusy();
    return false;
  }
  FfscnActivePackage activated;
  if (!activateFfscnEnginePackage(directory, modelRoot_, activated, error)) {
    summary_->setText(error);
    return false;
  }
  applied_ = true;
  activated_ = activated;
  refreshCurrentPackage();
  summary_->setText("已切换。下次检测使用：\n" + activated.packageDirectory +
                    "\n引擎 SHA-256：\n" + activated.engineSha256 +
                    "\n权重 SHA-256：\n" + shaText(activated.checkpointSha256) +
                    "\n已有检测结果保持不变。");
  emit engineActivated(activated.packageDirectory, activated.engineSha256);
  return true;
}
}  // namespace signalstudio
