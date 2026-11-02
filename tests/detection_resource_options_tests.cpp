#include "ui/detection/detection_resource_options.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtTest>

#include "domain/detection.h"
#include "infrastructure/runtime_log.h"

using namespace signalstudio;

class DetectionResourceOptionsTests : public QObject {
  Q_OBJECT

 private slots:
  void initTestCase();
  void cleanupTestCase();
  void defaultsIgnoreWheel();
  void cancelDoesNotWrite();
  void invalidValueIsNotWritten();
  void acceptAppliesToTheNextRequestOnly();

 private:
  QTemporaryDir settingsDir_;
  QTemporaryDir logDir_;
};

void DetectionResourceOptionsTests::initTestCase() {
  QVERIFY(settingsDir_.isValid());
  QVERIFY(logDir_.isValid());
  QCoreApplication::setOrganizationName(QStringLiteral("Signal Studio"));
  QCoreApplication::setApplicationName(
      QStringLiteral("SignalStudioOptionsTest"));
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settingsDir_.path());
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                     settingsDir_.path());
  QSettings().clear();
  QSettings().sync();
}

void DetectionResourceOptionsTests::cleanupTestCase() {
  RuntimeLog::uninstall();
  QSettings().clear();
  QSettings().sync();
}

void DetectionResourceOptionsTests::defaultsIgnoreWheel() {
  QSettings().clear();
  QSettings().sync();
  DetectionResourceOptionsDialog dialog;
  QCOMPARE(dialog.windowTitle(), "选项");
  auto* retain = dialog.findChild<QCheckBox*>("retainRawModelOutput");
  auto* evidence = dialog.findChild<QSpinBox*>("evidenceBudgetMiB");
  auto* workingSet = dialog.findChild<QSpinBox*>("workingSetBudgetMiB");
  auto* scratch = dialog.findChild<QSpinBox*>("sampleScratchBudgetGiB");
  auto* path = dialog.findChild<QLabel*>("runtimeLogPath");
  auto* note = dialog.findChild<QLabel*>("runtimeLogRotationNote");
  auto* openFolder = dialog.findChild<QPushButton*>("openRuntimeLogDirectory");
  QVERIFY(retain);
  QVERIFY(evidence);
  QVERIFY(workingSet);
  QVERIFY(scratch);
  QVERIFY(path);
  QVERIFY(note);
  QVERIFY(openFolder);
  auto* rawHead = dialog.findChild<QSpinBox*>("rawHeadBudgetGiB");
  QVERIFY(rawHead);
  const auto diskDefault = detectionDiskBudgetDefaultBytes({});
  const auto diskLimit = detectionDiskBudgetLimitBytes({});
  const auto memoryDefault = detectionWorkingSetDefaultBytes();
  const auto memoryLimit = detectionWorkingSetLimitBytes();
  const auto units = [](std::uint64_t bytes, std::uint64_t unit) {
    return static_cast<int>(bytes / unit);
  };
  constexpr auto kMiB = 1024ULL * 1024ULL;
  constexpr auto kGiB = 1024ULL * kMiB;
  QVERIFY(!retain->isChecked());
  QVERIFY(!rawHead->isEnabled());
  QCOMPARE(evidence->minimum(), 1);
  QCOMPARE(evidence->value(), units(diskDefault, kMiB));
  QCOMPARE(evidence->maximum(), units(diskLimit, kMiB));
  QCOMPARE(workingSet->minimum(), 16);
  QCOMPARE(workingSet->maximum(), units(memoryLimit, kMiB));
  QCOMPARE(workingSet->value(), units(memoryDefault, kMiB));
  QCOMPARE(scratch->value(), units(diskDefault, kGiB));
  QCOMPARE(scratch->maximum(), units(diskLimit, kGiB));
  QCOMPARE(rawHead->value(), units(diskDefault, kGiB));
  QCOMPARE(rawHead->maximum(), units(diskLimit, kGiB));
  QVERIFY(units(diskDefault, kGiB) >= 64 ||
          diskDefault == diskLimit);
  QVERIFY(path->text().contains("signal-studio.log"));
  QVERIFY(note->text().contains("8 MiB"));
  QVERIFY(note->text().contains("3"));
  dialog.show();
  QVERIFY(QTest::qWaitForWindowExposed(&dialog));
  const QPoint point(8, 8);
  const auto wheel = [&](QWidget* widget) {
    QWheelEvent event(point, widget->mapToGlobal(point), QPoint(),
                      QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(widget, &event);
  };
  wheel(evidence);
  wheel(workingSet);
  wheel(scratch);
  wheel(rawHead);
  QCOMPARE(evidence->value(), units(diskDefault, kMiB));
  QCOMPARE(workingSet->value(), units(memoryDefault, kMiB));
  QCOMPARE(scratch->value(), units(diskDefault, kGiB));
  QCOMPARE(rawHead->value(), units(diskDefault, kGiB));
}

void DetectionResourceOptionsTests::cancelDoesNotWrite() {
  QSettings().clear();
  QSettings().sync();
  DetectionResourceOptionsDialog dialog;
  dialog.findChild<QCheckBox*>("retainRawModelOutput")->setChecked(true);
  dialog.findChild<QSpinBox*>("evidenceBudgetMiB")->setValue(64);
  dialog.reject();
  const auto loaded = DetectionResourceOptions::load();
  QVERIFY(!loaded.retainRawModelOutput);
  QCOMPARE(loaded.evidenceBudgetBytes, detectionDiskBudgetDefaultBytes({}));
  QVERIFY(!QSettings().contains(
      QStringLiteral("detection/resources/evidenceBudgetBytes")));
}

void DetectionResourceOptionsTests::invalidValueIsNotWritten() {
  QSettings().clear();
  QSettings().sync();
  DetectionResourceOptionsDialog dialog;
  auto* evidence = dialog.findChild<QSpinBox*>("evidenceBudgetMiB");
  auto* edit = evidence->findChild<QLineEdit*>();
  QVERIFY(edit);
  edit->setText(QString::number(evidence->maximum() + 1LL));
  evidence->setProperty("parameterDraft", true);
  dialog.accept();
  QVERIFY(dialog.result() != QDialog::Accepted);
  const auto loaded = DetectionResourceOptions::load();
  QCOMPARE(loaded.evidenceBudgetBytes, detectionDiskBudgetDefaultBytes({}));
  QVERIFY(!QSettings().contains(
      QStringLiteral("detection/resources/evidenceBudgetBytes")));
}

void DetectionResourceOptionsTests::acceptAppliesToTheNextRequestOnly() {
  QSettings().clear();
  QSettings().sync();
  RuntimeLog::uninstall();
  const auto evidenceDefault = detectionDiskBudgetDefaultBytes({});
  const auto workingDefault = detectionWorkingSetDefaultBytes();
  const auto scratchDefault = detectionDiskBudgetDefaultBytes({});
  DetectionRequest running;
  running.retainRawModelOutput = false;
  running.rawHeadBudgetBytes = 256ULL * 1024 * 1024;
  running.evidenceBudgetBytes = evidenceDefault;
  running.workingSetBudgetBytes = workingDefault;
  running.sampleScratchBudgetBytes = scratchDefault;
  running.legacyCombinedEvidence = false;

  DetectionResourceOptionsDialog dialog;
  dialog.findChild<QCheckBox*>("retainRawModelOutput")->setChecked(true);
  dialog.findChild<QSpinBox*>("evidenceBudgetMiB")->setValue(64);
  dialog.findChild<QSpinBox*>("workingSetBudgetMiB")->setValue(128);
  dialog.findChild<QSpinBox*>("sampleScratchBudgetGiB")->setValue(8);
  dialog.findChild<QLineEdit*>("runtimeLogDirectory")->setText(logDir_.path());
  dialog.accept();
  QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));

  const auto loaded = DetectionResourceOptions::load();
  QVERIFY(loaded.retainRawModelOutput);
  QCOMPARE(loaded.evidenceBudgetBytes, 64ULL * 1024 * 1024);
  QCOMPARE(loaded.workingSetBudgetBytes, 128ULL * 1024 * 1024);
  QCOMPARE(loaded.sampleScratchBudgetBytes, 8ULL * 1024 * 1024 * 1024);
  QCOMPARE(QDir(loaded.logDirectory).absolutePath(),
           QDir(logDir_.path()).absolutePath());
  QVERIFY(RuntimeLog::filePath().contains("signal-studio.log"));

  const auto rawQuota =
      static_cast<std::uint64_t>(
          dialog.findChild<QSpinBox*>("rawHeadBudgetGiB")->value()) *
      1024ULL * 1024ULL * 1024ULL;
  QCOMPARE(loaded.rawHeadBudgetBytes, rawQuota);
  DetectionRequest next;
  loaded.applyTo(next);
  QVERIFY(next.retainRawModelOutput);
  QCOMPARE(next.rawHeadBudgetBytes, rawQuota);
  QVERIFY(!next.legacyCombinedEvidence);
  QCOMPARE(next.evidenceBudgetBytes, loaded.evidenceBudgetBytes);
  QCOMPARE(next.workingSetBudgetBytes, loaded.workingSetBudgetBytes);
  QCOMPARE(next.sampleScratchBudgetBytes, loaded.sampleScratchBudgetBytes);

  QVERIFY(!running.retainRawModelOutput);
  QCOMPARE(running.rawHeadBudgetBytes, 256ULL * 1024 * 1024);
  QCOMPARE(running.evidenceBudgetBytes, evidenceDefault);
  QCOMPARE(running.workingSetBudgetBytes, workingDefault);
  QCOMPARE(running.sampleScratchBudgetBytes, scratchDefault);
}

QTEST_MAIN(DetectionResourceOptionsTests)
#include "detection_resource_options_tests.moc"
