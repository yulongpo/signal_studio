#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtEndian>
#include <QTest>

#include "app/main_window.h"
#include "ui/detection/detection_card.h"

using namespace signalstudio;

class SsDeRpoUiTests : public QObject {
  Q_OBJECT

 private slots:
  void doubleClickArmsOneStart();
  void synchronousRejectionKeepsTheError();
};

bool prepareCard(QTemporaryDir& directory, MainWindow& window, QString& error) {
  const auto iqPath = directory.filePath("IQ_FS1Msps_FC100MHz.dat");
  QFile iq(iqPath);
  if (!iq.open(QIODevice::WriteOnly)) {
    error = iq.errorString();
    return false;
  }
  QByteArray bytes(20000 * 4, '\0');
  for (int index = 0; index < 20000; ++index)
    qToLittleEndian<qint16>(12000, bytes.data() + index * 4);
  if (iq.write(bytes) != static_cast<qint64>(bytes.size())) {
    error = iq.errorString();
    return false;
  }
  iq.close();
  if (!window.createProject(directory.path(), "ss-de-rpo-ui", &error) ||
      !window.addIqFile(iqPath, &error))
    return false;
  window.show();
  const auto ready = QTest::qWaitFor(
      [&] {
        const auto* file = window.session().activeFile();
        return file && file->metadata.availability.status == LoadStatus::Ready;
      },
      10000);
  if (!ready) {
    error = QStringLiteral("IQ did not become ready");
    return false;
  }
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* range = card ? card->findChild<QComboBox*>("detectionRangeOrigin") : nullptr;
  auto* model = card ? card->findChild<QComboBox*>("detectionModel") : nullptr;
  if (!card || !range || !model || model->findData("pipeline-fixture-v1") < 0 ||
      range->findData("available-file") < 0) {
    error = QStringLiteral("detection card controls are missing");
    return false;
  }
  model->setCurrentIndex(model->findData("pipeline-fixture-v1"));
  range->setCurrentIndex(range->findData("available-file"));
  QCoreApplication::processEvents();
  return true;
}

void SsDeRpoUiTests::doubleClickArmsOneStart() {
  qApp->setProperty("enableDetectionTestFixture", true);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareCard(directory, window, error), qPrintable(error));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  QVERIFY(card && run);

  int emissions = 0;
  bool preparing = false;
  QObject::connect(card, &DetectionCard::requestSubmitted, &window,
                   [&](const DetectionRequest&) {
                     if (emissions == 0) {
                       preparing = card->startLatchActive() && !run->isEnabled() &&
                                   run->text().contains(QStringLiteral("准备检测中"));
                       run->click();
                     }
                     ++emissions;
                   });
  auto* preflight = card->findChild<QLabel*>("detectionPreflight");
  QElapsedTimer timer;
  timer.start();
  run->click();
  const auto elapsedMs = timer.elapsed();
  for (int extra = 0; extra < 4 && card->startLatchActive(); ++extra) run->click();
  qInfo().nospace() << "R-08 click handler returned in " << elapsedMs
                    << " ms; emissions=" << emissions
                    << " latch=" << card->startLatchActive();
  if (emissions == 0 && preflight)
    QFAIL(qPrintable(QStringLiteral("start did not arm: ") + preflight->text()));
  QCOMPARE(emissions, 1);
  QVERIFY(preparing);
  QVERIFY2(elapsedMs < 1000, "the click handler blocked for more than one second");
}

void SsDeRpoUiTests::synchronousRejectionKeepsTheError() {
  qApp->setProperty("enableDetectionTestFixture", true);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareCard(directory, window, error), qPrintable(error));
  const auto projects = QDir(directory.path()).entryList({QStringLiteral("*.ssproj")}, QDir::Files);
  QCOMPARE(projects.size(), 1);
  QVERIFY(QFile::remove(directory.filePath(projects.front())));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  auto* preflight = card ? card->findChild<QLabel*>("detectionPreflight") : nullptr;
  QVERIFY(card && run && preflight);
  run->click();
  QVERIFY(!card->startLatchActive());
  QVERIFY(run->isEnabled());
  QVERIFY2(preflight->text().contains(QStringLiteral("检测事务需要当前已保存 .ssproj")),
           qPrintable(preflight->text()));
  QVERIFY(!preflight->text().contains(QStringLiteral("配置就绪")));
}

QTEST_MAIN(SsDeRpoUiTests)
#include "ss_de_rpo_ui_tests.moc"
