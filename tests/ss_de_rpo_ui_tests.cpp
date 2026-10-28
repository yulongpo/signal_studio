#include <QApplication>
#include <QComboBox>
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
};

void SsDeRpoUiTests::doubleClickArmsOneStart() {
  qApp->setProperty("enableDetectionTestFixture", true);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto iqPath = directory.filePath("IQ_FS1Msps_FC100MHz.dat");
  QFile iq(iqPath);
  QVERIFY(iq.open(QIODevice::WriteOnly));
  QByteArray bytes(20000 * 4, '\0');
  for (int index = 0; index < 20000; ++index)
    qToLittleEndian<qint16>(12000, bytes.data() + index * 4);
  QCOMPARE(iq.write(bytes), static_cast<qint64>(bytes.size()));
  iq.close();

  MainWindow window;
  QString error;
  QVERIFY2(window.createProject(directory.path(), "ss-de-rpo-ui", &error),
           qPrintable(error));
  QVERIFY2(window.addIqFile(iqPath, &error), qPrintable(error));
  window.show();
  QTRY_VERIFY_WITH_TIMEOUT(
      window.session().activeFile() &&
          window.session().activeFile()->metadata.availability.status ==
              LoadStatus::Ready,
      10000);

  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  auto* range = card ? card->findChild<QComboBox*>("detectionRangeOrigin") : nullptr;
  auto* model = card ? card->findChild<QComboBox*>("detectionModel") : nullptr;
  QVERIFY(card && run && range && model);
  const auto fixture = model->findData("pipeline-fixture-v1");
  const auto available = range->findData("available-file");
  QVERIFY(fixture >= 0);
  QVERIFY(available >= 0);
  model->setCurrentIndex(fixture);
  range->setCurrentIndex(available);
  QCoreApplication::processEvents();

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

QTEST_MAIN(SsDeRpoUiTests)
#include "ss_de_rpo_ui_tests.moc"
