#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <QTest>

#include <map>
#include <optional>

#include "app/main_window.h"
#include "infrastructure/annotation_json.h"
#include "ui/detection/detection_card.h"
#include "ui/detection/detection_panel.h"

using namespace signalstudio;

class SsDeRpoStartTests : public QObject {
  Q_OBJECT

 private slots:
  void staleRejectionDoesNotUnlockTheCard();
  void preparingCancelReachesThePanelBeforePublish();
  void provisionalCancelBeforePublishKeepsFormalResult();
  void cancelAfterCommittedStartCancelsQueuedRun();
};

void armFixture() { qApp->setProperty("enableDetectionTestFixture", true); }

void rejectUnexpectedModals(QTimer& timer, QString& failure) {
  QObject::connect(&timer, &QTimer::timeout, &timer, [&] {
    if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
      failure = message->windowTitle() + ": " + message->text();
      message->reject();
    }
  });
  timer.start(25);
}

bool writeIq(const QString& path, QString& error) {
  QFile iq(path);
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
  return true;
}

bool prepareProject(QTemporaryDir& directory, MainWindow& window, QString& error) {
  const auto iqPath = directory.filePath("IQ_FS1Msps_FC100MHz.dat");
  if (!writeIq(iqPath, error) ||
      !window.createProject(directory.path(), "ss-de-rpo-start", &error) ||
      !window.addIqFile(iqPath, &error))
    return false;
  window.show();
  return QTest::qWaitFor(
      [&] {
        const auto* file = window.session().activeFile();
        return file && file->metadata.availability.status == LoadStatus::Ready;
      },
      10000);
}

DetectionRequest fixtureRequest(MainWindow& window, const char* runId) {
  const auto* file = window.session().activeFile();
  DetectionRequest request;
  request.runId = runId;
  request.sourceIdentity = annotationSourceIdentity(
      file->metadata, window.session().projectGeneration());
  request.profile = *findDetectionProfile("pipeline-fixture-v1");
  request.requestedRange = fullRange(file->metadata);
  request.contextRange = request.requestedRange;
  request.rangeOrigin = "available-file";
  return request;
}

const CurrentDetectionSlot* activeSlot(MainWindow& window) {
  const auto* file = window.session().activeFile();
  return file ? currentDetectionSlot(*file, file->metadata.sampleFormat.selectedChannel)
              : nullptr;
}

QStringList formalAnnotations(MainWindow& window) {
  QStringList encoded;
  const auto* file = window.session().activeFile();
  if (!file) return encoded;
  for (const auto& mark : file->marks) {
    if (mark.sourceKind != AnnotationSource::Algorithm) continue;
    encoded.push_back(QString::fromUtf8(
        QJsonDocument(encodeAnnotation(mark)).toJson(QJsonDocument::Compact)));
  }
  return encoded;
}

std::map<QString, quint64> timelineMarks(const DetectionRun& run) {
  std::map<QString, quint64> marks;
  const auto document = QJsonDocument::fromJson(
      QByteArray::fromStdString(run.performanceJson));
  const auto values = document.object()
                          .value(QStringLiteral("timeline"))
                          .toObject()
                          .value(QStringLiteral("marks"))
                          .toArray();
  for (const auto& value : values) {
    const auto object = value.toObject();
    bool ok = false;
    const auto stamp = object.value(QStringLiteral("steadyNs")).toString().toULongLong(&ok);
    if (!ok) continue;
    marks.emplace(object.value(QStringLiteral("name")).toString(),
                  static_cast<quint64>(stamp));
  }
  return marks;
}

std::optional<DetectionRun> snapshotRun(DetectionPanel& panel, const QString& runId) {
  for (const auto& run : panel.tasks().snapshots())
    if (QString::fromStdString(run.request.runId) == runId) return run;
  return std::nullopt;
}

void SsDeRpoStartTests::staleRejectionDoesNotUnlockTheCard() {
  armFixture();
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareProject(directory, window, error), qPrintable(error));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  auto* cancel = window.findChild<QPushButton*>("cancelDetectionCardRun");
  auto* range = card ? card->findChild<QComboBox*>("detectionRangeOrigin") : nullptr;
  auto* model = card ? card->findChild<QComboBox*>("detectionModel") : nullptr;
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(card && run && cancel && range && model && panel);
  model->setCurrentIndex(model->findData("pipeline-fixture-v1"));
  range->setCurrentIndex(range->findData("available-file"));
  panel->setStartSaveFault(true);

  bool sawLatch = false;
  bool sawDisabled = false;
  bool sawCancel = false;
  QObject::connect(card, &DetectionCard::requestSubmitted, &window,
                   [&](const DetectionRequest&) {
                     card->noteStartRejected("stale-run", QStringLiteral("旧回调"));
                     sawLatch = card->startLatchActive() &&
                                card->startLatchRunId() != "stale-run";
                     sawDisabled = !run->isEnabled() &&
                                   run->text().contains(QStringLiteral("准备检测中"));
                     sawCancel = cancel->isEnabled();
                   });
  run->click();
  QVERIFY(sawLatch);
  QVERIFY(sawDisabled);
  QVERIFY(sawCancel);
  QTRY_VERIFY_WITH_TIMEOUT(!card->startLatchActive(), 10000);
  QVERIFY(run->isEnabled());
  QVERIFY(!cancel->isEnabled());
  auto* preflight = card->findChild<QLabel*>("detectionPreflight");
  QVERIFY(preflight);
  QVERIFY2(preflight->text().contains(QStringLiteral("检测事务保存失败")),
           qPrintable(preflight->text()));
  QVERIFY2(preflight->text().contains(QStringLiteral("上次正式结果仍在")),
           qPrintable(preflight->text()));
  QVERIFY(!preflight->text().contains(QStringLiteral("旧回调")));
  QVERIFY(!preflight->text().contains(QStringLiteral("配置就绪")));
}

void SsDeRpoStartTests::preparingCancelReachesThePanelBeforePublish() {
  armFixture();
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareProject(directory, window, error), qPrintable(error));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  auto* cancel = window.findChild<QPushButton*>("cancelDetectionCardRun");
  auto* range = card ? card->findChild<QComboBox*>("detectionRangeOrigin") : nullptr;
  auto* model = card ? card->findChild<QComboBox*>("detectionModel") : nullptr;
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(card && run && cancel && range && model && panel);
  model->setCurrentIndex(model->findData("pipeline-fixture-v1"));
  range->setCurrentIndex(range->findData("available-file"));
  auto gate = std::make_shared<std::atomic<bool>>(false);
  panel->setStartCommitGate(gate);
  QSignalSpy cancelled(card, &DetectionCard::cancelRunRequested);
  QString panelCancelId;
  QObject::connect(card, &DetectionCard::cancelRunRequested, &window,
                   [&](const QString& runId) {
                     panelCancelId = runId;
                     panel->cancelRun(runId);
                   });
  bool armed = false;
  QObject::connect(card, &DetectionCard::requestSubmitted, &window,
                   [&](const DetectionRequest& request) {
                     armed = card->startLatchActive() && cancel->isEnabled() &&
                             !run->isEnabled();
                     if (armed) cancel->click();
                     Q_UNUSED(request);
                   });
  run->click();
  QVERIFY(armed);
  QCOMPARE(cancelled.count(), 1);
  const auto startedId = cancelled.takeFirst().at(0).toString();
  QCOMPARE(startedId.toStdString(), card->startLatchRunId());
  QCOMPARE(panelCancelId, startedId);
  gate->store(true);
  QTRY_VERIFY_WITH_TIMEOUT(!card->startLatchActive(), 20000);
  const auto* file = window.session().activeFile();
  QVERIFY(file);
  QVERIFY(file->detectionRuns.empty());
  const auto* slot = activeSlot(window);
  QVERIFY(!slot || slot->committedRunId.empty());
  QVERIFY(!snapshotRun(*panel, startedId));
}

void SsDeRpoStartTests::provisionalCancelBeforePublishKeepsFormalResult() {
  armFixture();
  QTimer modals;
  QString modalFailure;
  rejectUnexpectedModals(modals, modalFailure);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareProject(directory, window, error), qPrintable(error));
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(panel);
  QVERIFY2(panel->startRequest(fixtureRequest(window, "formal-seed"), error),
           qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(activeSlot(window) &&
                               activeSlot(window)->committedRunId == "formal-seed" &&
                               !activeSlot(window)->stale,
                           30000);
  const auto formal = formalAnnotations(window);
  QVERIFY(!formal.isEmpty());
  auto gate = std::make_shared<std::atomic<bool>>(false);
  panel->setStartCommitGate(gate);
  bool rejected = false;
  QObject::connect(panel, &DetectionPanel::startRejected, &window,
                   [&](const QString& runId, const QString&) {
                     rejected = runId == QStringLiteral("formal-replacement");
                   });
  QVERIFY2(panel->startRequest(fixtureRequest(window, "formal-replacement"), error),
           qPrintable(error));
  QVERIFY2(QTest::qWaitFor([&] { return panel->startCommitGateBlocked(); }, 20000),
           qPrintable(modalFailure));
  panel->cancelRun(QStringLiteral("formal-replacement"));
  gate->store(true);
  QTRY_VERIFY_WITH_TIMEOUT(rejected, 20000);
  const auto* slot = activeSlot(window);
  QVERIFY(slot);
  QCOMPARE(slot->committedRunId, std::string("formal-seed"));
  QVERIFY(!slot->suppressFormalResults);
  QCOMPARE(formalAnnotations(window), formal);
  QVERIFY(!snapshotRun(*panel, QStringLiteral("formal-replacement")));
}

void SsDeRpoStartTests::cancelAfterCommittedStartCancelsQueuedRun() {
  armFixture();
  QTimer modals;
  QString modalFailure;
  rejectUnexpectedModals(modals, modalFailure);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareProject(directory, window, error), qPrintable(error));
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(panel);
  QVERIFY2(panel->startRequest(fixtureRequest(window, "formal-seed"), error),
           qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(activeSlot(window) &&
                               activeSlot(window)->committedRunId == "formal-seed",
                           30000);
  // A stamp of 1 stays below any live steady_clock reading, so order checks
  // do not depend on how long this machine has been up.
  const auto clickNs = static_cast<std::uint64_t>(1);
  panel->setCancelAfterStartCommitForTest(true);
  QVERIFY2(panel->startRequest(fixtureRequest(window, "queued-cancel"), error, clickNs),
           qPrintable(error));
  QVERIFY2(QTest::qWaitFor(
               [&] {
                 const auto run = snapshotRun(*panel, QStringLiteral("queued-cancel"));
                 const auto* slot = activeSlot(window);
                 return run &&
                        (run->lifecycle == DetectionLifecycle::Cancelled ||
                         run->lifecycle == DetectionLifecycle::Cancelling) &&
                        slot && slot->committedRunId != "formal-seed" &&
                        slot->committedRunId != "queued-cancel";
               },
               20000),
           qPrintable(modalFailure));
  QVERIFY2(QTest::qWaitFor(
               [&] {
                 const auto run = snapshotRun(*panel, QStringLiteral("queued-cancel"));
                 if (!run) return false;
                 const auto marks = timelineMarks(*run);
                 return marks.count(QStringLiteral("t_terminal")) == 1 &&
                        marks.count(QStringLiteral("t_click")) == 1;
               },
               5000),
           qPrintable(modalFailure));
  const auto run = snapshotRun(*panel, QStringLiteral("queued-cancel"));
  QVERIFY2(run.has_value(), qPrintable(modalFailure));
  const auto marks = timelineMarks(*run);
  QCOMPARE(marks.at(QStringLiteral("t_click")), static_cast<quint64>(clickNs));
  QVERIFY(marks.count(QStringLiteral("t_accepted")) == 1);
  QVERIFY(marks.count(QStringLiteral("t_model_snapshot_ready")) == 1);
  QVERIFY(marks.count(QStringLiteral("t_preflight_start")) == 1);
  QVERIFY(marks.count(QStringLiteral("t_preflight_end")) == 1);
  QVERIFY(marks.count(QStringLiteral("t_manifest_committed")) == 1);
  QVERIFY(marks.count(QStringLiteral("t_start_project_commit")) == 1);
  QVERIFY(marks.count(QStringLiteral("t_durable_publication")) == 0);
  QVERIFY(marks.at(QStringLiteral("t_click")) <= marks.at(QStringLiteral("t_accepted")));
  QVERIFY(marks.at(QStringLiteral("t_accepted")) <=
          marks.at(QStringLiteral("t_model_snapshot_ready")));
  QVERIFY(marks.at(QStringLiteral("t_model_snapshot_ready")) <=
          marks.at(QStringLiteral("t_preflight_start")));
  QVERIFY(marks.at(QStringLiteral("t_preflight_end")) <=
          marks.at(QStringLiteral("t_manifest_committed")));
  QVERIFY(marks.at(QStringLiteral("t_manifest_committed")) <=
          marks.at(QStringLiteral("t_start_project_commit")));
  QVERIFY(marks.at(QStringLiteral("t_start_project_commit")) <=
          marks.at(QStringLiteral("t_terminal")));
}

QTEST_MAIN(SsDeRpoStartTests)
#include "ss_de_rpo_start_tests.moc"
