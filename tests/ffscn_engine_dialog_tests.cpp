#include "ui/detection/ffscn_engine_dialog.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtTest>

using namespace signalstudio;

class FfscnEngineDialogTests : public QObject {
  Q_OBJECT
 private slots:
  void defaultsStayOnFp32AndIgnoreWheel();
  void missingCheckpointDoesNotSpawn();
  void requestRulesRejectUnsafeShapesAndOutputs();
};

void FfscnEngineDialogTests::defaultsStayOnFp32AndIgnoreWheel() {
  FfscnEngineDialog dialog(nullptr, false);
  QVERIFY(!dialog.isModal());
  QCOMPARE(dialog.windowTitle(), "FFSCN 引擎转换");
  auto* help = dialog.findChild<QLabel*>("ffscnEngineHelp");
  QVERIFY(help);
  QVERIFY(help->text().contains("不是吞吐基准"));
  QVERIFY(help->text().contains("detection-models"));
  QVERIFY(help->text().contains("down_nums=11"));
  QVERIFY(help->text().contains("不支持 INT8"));
  auto* precision = dialog.findChild<QComboBox*>("ffscnPrecision");
  QVERIFY(precision);
  QCOMPARE(precision->currentData().toString(), "FP32");
  QCOMPARE(precision->count(), 2);
  QCOMPARE(precision->itemData(1).toString(), "FP16");
  for (int index = 0; index < precision->count(); ++index)
    QVERIFY(!precision->itemText(index).contains("INT8"));
  auto* frequency = dialog.findChild<QComboBox*>("ffscnFrequencyBins");
  QVERIFY(frequency);
  QCOMPARE(frequency->currentData().toInt(), 65536);
  for (int index = 0; index < frequency->count(); ++index) {
    const int bins = frequency->itemData(index).toInt();
    QVERIFY(bins >= 2048);
    QVERIFY((bins & (bins - 1)) == 0);
  }
  auto* time = dialog.findChild<QSpinBox*>("ffscnTimeFrames");
  QVERIFY(time);
  QCOMPARE(time->value(), 10);
  dialog.show();
  QVERIFY(QTest::qWaitForWindowExposed(&dialog));
  const QPoint point(8, 8);
  const auto wheel = [&](QWidget* widget) {
    QWheelEvent event(point, widget->mapToGlobal(point), QPoint(),
                      QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(widget, &event);
  };
  wheel(time);
  wheel(frequency);
  wheel(precision);
  QCOMPARE(time->value(), 10);
  QCOMPARE(frequency->currentData().toInt(), 65536);
  QCOMPARE(precision->currentData().toString(), "FP32");
  QCOMPARE(dialog.startedProcesses(), 0);
}

void FfscnEngineDialogTests::missingCheckpointDoesNotSpawn() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto python = directory.filePath("python.exe");
  const auto trt = directory.filePath("tensorrt");
  const auto output = directory.filePath("out/tasks/ffscn-engine-tool");
  QVERIFY(QFile(python).open(QIODevice::WriteOnly));
  QVERIFY(QDir().mkpath(trt));
  FfscnEngineDialog dialog(nullptr, false);
  dialog.findChild<QLineEdit*>("ffscnPythonPath")->setText(python);
  dialog.findChild<QLineEdit*>("ffscnCheckpointPath")
      ->setText(directory.filePath("missing-epoch.pth"));
  dialog.findChild<QLineEdit*>("ffscnTrtRoot")->setText(trt);
  dialog.findChild<QLineEdit*>("ffscnOutputDir")->setText(output);
  auto* time = dialog.findChild<QSpinBox*>("ffscnTimeFrames");
  auto* edit = time->findChild<QLineEdit*>();
  dialog.show();
  QVERIFY(QTest::qWaitForWindowExposed(&dialog));
  edit->setFocus();
  edit->selectAll();
  QTest::keyClicks(edit, "12");
  QCOMPARE(time->value(), 10);
  QTest::mouseClick(dialog.findChild<QPushButton*>("ffscnValidateButton"),
                    Qt::LeftButton);
  QCOMPARE(time->value(), 12);
  QVERIFY(dialog.summaryText().contains("未找到 checkpoint"));
  QVERIFY(dialog.summaryText().contains("未被替换"));
  QCOMPARE(dialog.startedProcesses(), 0);
  QVERIFY(!dialog.conversionRunning());
  edit->setFocus();
  edit->selectAll();
  QTest::keyClicks(edit, "14");
  QTest::keyClick(edit, Qt::Key_Tab);
  QCOMPARE(time->value(), 14);
  QTest::mouseClick(dialog.findChild<QPushButton*>("ffscnConvertButton"),
                    Qt::LeftButton);
  QCOMPARE(dialog.startedProcesses(), 0);
  QVERIFY(dialog.summaryText().contains("未找到 checkpoint"));
}

void FfscnEngineDialogTests::requestRulesRejectUnsafeShapesAndOutputs() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto python = directory.filePath("python.exe");
  const auto checkpoint = directory.filePath("epoch.pth");
  const auto script = directory.filePath("ffscn_trt_convert.py");
  const auto trt = directory.filePath("tensorrt");
  QVERIFY(QFile(python).open(QIODevice::WriteOnly));
  QVERIFY(QFile(checkpoint).open(QIODevice::WriteOnly));
  QFile scriptFile(script);
  QVERIFY(scriptFile.open(QIODevice::WriteOnly));
  scriptFile.write("print('stub')\n");
  scriptFile.close();
  QVERIFY(QDir().mkpath(trt));
  FfscnEngineRequest request;
  request.pythonPath = python;
  request.checkpointPath = checkpoint;
  request.trtRoot = trt;
  request.outputDirectory = directory.filePath("out/tasks/ffscn");
  request.repoRoot = directory.path();
  request.scriptPath = script;
  request.applicationDirectory = directory.filePath("app-bin");
  request.timeFrames = 10;
  request.frequencyBins = 4096;
  request.precision = "FP32";
  request.workspaceMiB = 1024;
  QVERIFY(ffscnEngineRequestErrors(request).isEmpty());
  request.precision = "FP16";
  QVERIFY(ffscnEngineRequestErrors(request).isEmpty());
  request.precision = "INT8";
  QVERIFY(ffscnEngineRequestErrors(request).join("\n").contains("INT8"));
  request.precision = "FP32";
  request.frequencyBins = 3072;
  QVERIFY(ffscnEngineRequestErrors(request).join("\n").contains("2 的幂"));
  request.frequencyBins = 4096;
  request.timeFrames = 0;
  QVERIFY(ffscnEngineRequestErrors(request).join("\n").contains("时间帧"));
  request.timeFrames = 10;
  request.outputDirectory =
      directory.filePath("detection-models/ffscn-gui-iq-v1");
  const auto deployed = ffscnEngineRequestErrors(request).join("\n");
  QVERIFY(deployed.contains("detection-models"));
  request.outputDirectory = directory.filePath("tools/models");
  QVERIFY(ffscnEngineRequestErrors(request).join("\n").contains("源码目录"));
  request.outputDirectory = directory.filePath("out/tasks/ffscn");
  request.checkpointPath = directory.filePath("missing.pth");
  QVERIFY(ffscnEngineRequestErrors(request).join("\n").contains(
      "未找到 checkpoint"));
}

QTEST_MAIN(FfscnEngineDialogTests)
#include "ffscn_engine_dialog_tests.moc"
