#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QInputDialog>
#include <QMenu>
#include <QTreeWidget>
#include <QElapsedTimer>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <QTest>

#include <algorithm>
#include <map>
#include <optional>

#include "app/main_window.h"
#include "infrastructure/annotation_json.h"
#include "ui/detection/detection_card.h"
#include "ui/detection/detection_panel.h"
#include "ui/annotations/annotation_panel.h"
#include "infrastructure/project_file.h"
#include "infrastructure/project_store.h"

using namespace signalstudio;

class SsDeRpoStartTests : public QObject {
  Q_OBJECT

 private slots:
  void staleRejectionDoesNotUnlockTheCard();
  void switchingSourceDropsProvisionalUiState();
  void preparingCancelReachesThePanelBeforePublish();
  void provisionalCancelBeforePublishKeepsFormalResult();
  void cancelAfterCommittedStartCancelsQueuedRun();
  void acceptedStartContextChangeNeverRestoresFormal_data();
  void acceptedStartContextChangeNeverRestoresFormal();
  void renameResolvesChannelAfterModalProjectSwap();
  void sourceLoadSurvivesStartCommitInstall();
  void sourceLoadSurvivesPublicationCommitInstall();
  void cancelledStartCommitAppliesFinishedSourceLoad();
  void completedWorkerWaitsForProjectIoLease();
  void closingWhilePublicationWaitsForGuiDoesNotDeadlock();
  void annotationControlsLeaveTheLockedProjectUntouched();
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
  card->refresh();
  QTest::qWait(100);
  card->refresh();
  QVERIFY2(preflight->text().contains(QStringLiteral("检测事务保存失败")),
           qPrintable(preflight->text()));
  QVERIFY2(preflight->text().contains(QStringLiteral("上次正式结果仍在")),
           qPrintable(preflight->text()));
  QVERIFY(!preflight->text().contains(QStringLiteral("旧回调")));
  QVERIFY(!preflight->text().contains(QStringLiteral("配置就绪")));
  const auto firstId = card->startLatchRunId();
  run->click();
  QVERIFY(card->startLatchActive());
  QVERIFY(card->startLatchRunId() != firstId);
  QTRY_VERIFY_WITH_TIMEOUT(!card->startLatchActive(), 10000);
  QVERIFY(run->isEnabled());
}

void SsDeRpoStartTests::switchingSourceDropsProvisionalUiState() {
  armFixture();
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareProject(directory, window, error), qPrintable(error));
  const auto first = window.session().activeFile()->metadata.id;
  const auto path = directory.filePath("second_FS1Msps_FC100MHz.dat");
  QVERIFY2(writeIq(path, error), qPrintable(error));
  QVERIFY2(window.addIqFile(path, &error), qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(window.session().activeFile()->metadata.availability.status == LoadStatus::Ready, 10000);
  const auto second = window.session().activeFile()->metadata.id;
  QVERIFY(first != second);
  QVERIFY(window.session().activateFile(first));
  window.refresh();
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  auto* model = card ? card->findChild<QComboBox*>("detectionModel") : nullptr;
  auto* range = card ? card->findChild<QComboBox*>("detectionRangeOrigin") : nullptr;
  QVERIFY(card && panel && run && model && range);
  model->setCurrentIndex(model->findData("pipeline-fixture-v1"));
  range->setCurrentIndex(range->findData("available-file"));
  // Keep preparation pending at the card boundary. The real atomic project
  // commit locks source navigation; its cancellation is exercised below.
  QObject::disconnect(card, &DetectionCard::requestSubmitted, nullptr, nullptr);
  run->click();
  const auto id = QString::fromStdString(card->startLatchRunId());
  QVERIFY(!id.isEmpty());
  QVERIFY(card->startLatchActive());
  QVERIFY(window.session().activateFile(second));
  window.refresh();
  QVERIFY(!card->startLatchActive());
  card->noteStartRejected(id.toStdString(), QStringLiteral("上次正式结果仍在"));
  card->refresh();
  auto* preflight = card->findChild<QLabel*>("detectionPreflight");
  QVERIFY(preflight);
  QVERIFY(!preflight->text().contains(QStringLiteral("上次正式结果仍在")));
  QCOMPARE(window.session().activeFile()->metadata.id, second);
  QVERIFY(!card->startLatchActive());
  QVERIFY(run->isEnabled());
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
  const auto files = QDir(directory.path()).entryList({"*.ssproj"}, QDir::Files);
  QCOMPARE(files.size(), 1);
  Project reopened;
  QVERIFY2(ProjectFileCodec::load(directory.filePath(files.front()), reopened, error), qPrintable(error));
  QVERIFY(!reopened.files.empty());
  const auto* savedSlot = currentDetectionSlot(reopened.files.front(), 0);
  QVERIFY(savedSlot && savedSlot->committedRunId.empty());
  QVERIFY(savedSlot->suppressFormalResults);
}

void SsDeRpoStartTests::acceptedStartContextChangeNeverRestoresFormal_data() {
  QTest::addColumn<bool>("newProject");
  QTest::newRow("late-channel-context") << false;
  QTest::newRow("replacement-project") << true;
}

void SsDeRpoStartTests::acceptedStartContextChangeNeverRestoresFormal() {
  QFETCH(bool, newProject);
  armFixture();
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareProject(directory, window, error), qPrintable(error));
  const auto sourceId = window.session().activeFile()->metadata.id;
  const auto iqPath = directory.filePath("other_FS1Msps_FC100MHz.dat");
  QVERIFY2(writeIq(iqPath, error), qPrintable(error));
  QVERIFY2(window.addIqFile(iqPath, &error), qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(window.session().activeFile()->metadata.availability.status == LoadStatus::Ready, 10000);
  const auto otherId = window.session().activeFile()->metadata.id;
  const auto mark = window.session().addMark({{100, 10000}, {99.9e6, 100.1e6}});
  QVERIFY(!mark.empty());
  window.session().selectMarks({mark}, mark);
  QVERIFY(window.session().createChannelFromActiveMark());
  const auto channelId = window.session().activeChannel()->id;
  QVERIFY(window.session().activateFile(sourceId));
  window.refresh();
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(panel);
  QVERIFY2(panel->startRequest(fixtureRequest(window, "formal-seed"), error), qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(activeSlot(window) && activeSlot(window)->committedRunId == "formal-seed", 30000);
  QVERIFY(!formalAnnotations(window).isEmpty());

  auto gate = std::make_shared<std::atomic<bool>>(false);
  panel->setStartCompletionGateForTest(gate);
  QVERIFY2(panel->startRequest(fixtureRequest(window, "accepted-context-change"), error), qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(panel->startCompletionGateBlockedForTest(), 20000);
  QVERIFY(panel->projectCommitInProgress());
  const auto originalBandwidth = window.session().activeFile()->metadata.effectiveBandwidthHz;
  QVERIFY(!window.session().setEffectiveBandwidthHz(originalBandwidth * 0.8));
  QCOMPARE(window.session().activeFile()->metadata.effectiveBandwidthHz, originalBandwidth);
  const auto originalFrequencyMode = window.session().activeFile()->display.absoluteFrequency;
  auto* frequencyMode = window.findChild<QComboBox*>("freqMode");
  QVERIFY(frequencyMode);
  frequencyMode->setCurrentIndex(1 - frequencyMode->currentIndex());
  QCOMPARE(window.session().activeFile()->display.absoluteFrequency, originalFrequencyMode);
  // This is the real user route from the independent review. It must now be
  // blocked throughout the transaction, including after atomic acceptance.
  QVERIFY(!window.session().activateChannel(channelId));
  QCOMPARE(window.session().project().activeFileId, sourceId);

  QByteArray replacementToken;
  if (newProject) {
    // Model a forced controller replacement after acceptance, bypassing the
    // ordinary write guard to exercise the stale callback defense as well.
    Project replacement;
    replacement.name = "replacement-context";
    replacement.projectId = "replacement-context-project";
    window.session().setProjectWriteGuard({});
    window.session().replaceProject(std::move(replacement));
    replacementToken = ProjectStore::stateToken(window.session().project());
  } else {
    window.session().project().activeFileId = otherId;
    window.session().project().activeChannelId = channelId;
    window.session().project().narrowbandWorkspaceOpen = true;
  }
  gate->store(true);
  QTRY_VERIFY_WITH_TIMEOUT(!panel->projectCommitInProgress(), 20000);
  const auto run = snapshotRun(*panel, "accepted-context-change");
  QVERIFY(run && (run->lifecycle == DetectionLifecycle::Cancelled || run->lifecycle == DetectionLifecycle::Cancelling));
  if (newProject) {
    QCOMPARE(ProjectStore::stateToken(window.session().project()), replacementToken);
  } else {
    QCOMPARE(window.session().project().activeFileId, otherId);
    QCOMPARE(window.session().project().activeChannelId, channelId);
    const auto& files = window.session().project().files;
    const auto found = std::find_if(files.begin(), files.end(), [&](const auto& f) { return f.metadata.id == sourceId; });
    QVERIFY(found != files.end());
    const auto* cleared = currentDetectionSlot(*found, 0);
    QVERIFY(cleared && cleared->committedRunId.empty() && cleared->suppressFormalResults);
  }
  const auto paths = QDir(directory.path()).entryList({"*.ssproj"}, QDir::Files);
  QCOMPARE(paths.size(), 1);
  Project reopened;
  QVERIFY2(ProjectFileCodec::load(directory.filePath(paths.front()), reopened, error), qPrintable(error));
  const auto found = std::find_if(reopened.files.begin(), reopened.files.end(), [&](const auto& f) { return f.metadata.id == sourceId; });
  QVERIFY(found != reopened.files.end());
  const auto* saved = currentDetectionSlot(*found, 0);
  QVERIFY(saved && saved->committedRunId.empty() && saved->suppressFormalResults);
}

void SsDeRpoStartTests::completedWorkerWaitsForProjectIoLease() {
  armFixture();
  QTemporaryDir directory;
  MainWindow window;
  QString error;
  QVERIFY2(prepareProject(directory, window, error), qPrintable(error));
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(panel);
  bool held = false;
  bool leaseAccepted = false;
  auto release = qScopeGuard([&] { if (held) panel->endProjectIo(); });
  QObject::connect(panel, &DetectionPanel::commitSettled, &window,
      [&](bool publication, bool installed) {
        if (!publication && installed) {
          leaseAccepted = panel->beginProjectIo(error);
          held = leaseAccepted;
        }
      });
  QVERIFY2(panel->startRequest(fixtureRequest(window, "save-lease"), error), qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(leaseAccepted, 20000);
  QTRY_VERIFY_WITH_TIMEOUT(snapshotRun(*panel, "save-lease") &&
      snapshotRun(*panel, "save-lease")->lifecycle == DetectionLifecycle::Completed, 30000);
  // Exercise several real timer polls after Worker completion. Publication
  // stays in the task manager; no snapshot or shard is consumed by the lease.
  QTest::qWait(250);
  QVERIFY(panel->projectCommitInProgress());
  QVERIFY(!panel->publicationCommitInProgress());
  QVERIFY(panel->hasPendingPublications());
  QVERIFY(activeSlot(window) && activeSlot(window)->committedRunId.empty());
  QVERIFY(!panel->startRequest(fixtureRequest(window, "rejected-save-lease"), error));
  QCOMPARE(panel->acceptedRunId(), std::string("save-lease"));
  panel->endProjectIo(); held = false;
  QTRY_VERIFY_WITH_TIMEOUT(activeSlot(window) &&
      activeSlot(window)->committedRunId == "save-lease", 30000);
  QVERIFY(!panel->projectCommitInProgress());
  QVERIFY(!panel->hasPendingPublications());
  QTRY_VERIFY(panel->lastPublicationCommitReleasedForTest());
}

void SsDeRpoStartTests::closingWhilePublicationWaitsForGuiDoesNotDeadlock() {
  armFixture();
  QTemporaryDir directory;
  auto window = std::make_unique<MainWindow>();
  QString error;
  QVERIFY2(prepareProject(directory, *window, error), qPrintable(error));
  auto* panel = window->findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(panel);
  auto gate = std::make_shared<std::atomic<bool>>(false);
  panel->setPublicationGate(gate);
  QVERIFY2(panel->startRequest(fixtureRequest(*window, "close-at-ui-check"), error),
           qPrintable(error));
  QTRY_VERIFY_WITH_TIMEOUT(panel->publicationCommitInProgress(), 30000);
  gate->store(true);
  QElapsedTimer wait;
  wait.start();
  // Deliberately keep the GUI from dispatching the queued context check. This
  // bounded test barrier places destruction at the old join/invoke deadlock.
  while (!panel->publicationUiCheckPendingForTest() && wait.elapsed() < 5000)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  QVERIFY(panel->publicationUiCheckPendingForTest());
  QElapsedTimer closing;
  closing.start();
  window.reset();
  QVERIFY2(closing.elapsed() < 5000, "GUI shutdown did not release publication context wait");
}

void SsDeRpoStartTests::annotationControlsLeaveTheLockedProjectUntouched() {
  armFixture();
  QTemporaryDir directory;
  MainWindow window;
  QString error;
  QVERIFY2(prepareProject(directory, window, error), qPrintable(error));
  const auto mark = window.session().addMark({{1000, 5000}, {99.9e6, 100.1e6}});
  QVERIFY(!mark.empty());
  auto* annotations = window.findChild<AnnotationPanel*>();
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(annotations && panel);
  annotations->setSourceTab(AnnotationSource::Manual);
  QVERIFY(window.session().selectMarks({mark}, mark));
  window.refresh();
  auto* status = window.findChild<QComboBox*>("annotationReviewFilter");
  auto* score = window.findChild<QDoubleSpinBox*>("annotationMinimumMeanScore");
  auto* clear = window.findChild<QPushButton*>("clearAnnotationSelection");
  QVERIFY(status && score && clear);
  QVERIFY(panel->beginProjectIo(error));
  const auto release = qScopeGuard([&] { panel->endProjectIo(); });
  const auto before = ProjectStore::stateToken(window.session().project());
  const auto epoch = window.session().contentEpoch();
  annotations->setSourceTab(AnnotationSource::Algorithm);
  status->setCurrentIndex(2);
  score->setValue(0.7);
  clear->click();
  QCoreApplication::processEvents();
  QCOMPARE(annotations->sourceTab(), AnnotationSource::Manual);
  QCOMPARE(ProjectStore::stateToken(window.session().project()), before);
  QCOMPARE(window.session().contentEpoch(), epoch);
  QCOMPARE(status->currentIndex(), 0);
  QCOMPARE(score->value(), 0.0);
  panel->endProjectIo();
  status->setCurrentIndex(2);
  QCOMPARE(window.session().activeFile()->annotationFilter.review, 1);
  annotations->setSourceTab(AnnotationSource::Algorithm);
  QCOMPARE(annotations->sourceTab(), AnnotationSource::Algorithm);
}

FileState* fileById(MainWindow& window, const std::string& id) {
  auto& files = window.session().project().files;
  const auto found = std::find_if(files.begin(), files.end(), [&](const FileState& file) {
    return file.metadata.id == id;
  });
  return found == files.end() ? nullptr : &*found;
}

struct HeldSourceLoad {
  std::string activeId;
  std::string loadingId;
  SampleIndex sampleCount = 0;
  std::uint64_t loadGeneration = 0;
};

bool armHeldSourceLoad(QTemporaryDir& directory, MainWindow& window,
                       HeldSourceLoad& held, QString& error) {
  const auto* active = window.session().activeFile();
  if (!active || active->metadata.availability.status != LoadStatus::Ready) {
    error = QStringLiteral("active file is not ready");
    return false;
  }
  held.activeId = active->metadata.id;
  window.setSourceLoadPollingForTest(false);
  const auto iqPath = directory.filePath("held_FS1Msps_FC100MHz.dat");
  if (!writeIq(iqPath, error) || !window.addIqFile(iqPath, &error)) return false;
  const auto* loading = window.session().activeFile();
  if (!loading) {
    error = QStringLiteral("second file was not added");
    return false;
  }
  held.loadingId = loading->metadata.id;
  held.sampleCount = loading->metadata.sampleCount;
  held.loadGeneration = loading->metadata.availability.generation;
  if (held.loadingId == held.activeId || held.sampleCount == 0) {
    error = QStringLiteral("second file identity is invalid");
    return false;
  }
  const auto loadingKey = QString::fromStdString(held.loadingId);
  if (!QTest::qWaitFor(
          [&] {
            return window.sourceLoadFinishedForTest(loadingKey) &&
                   window.sourceLoadLoadedForTest(loadingKey) == held.sampleCount;
          },
          10000)) {
    error = QStringLiteral("second file reader did not finish the physical sample count");
    return false;
  }
  if (!window.session().activateFile(held.activeId)) {
    error = QStringLiteral("could not return to the ready detection file");
    return false;
  }
  const auto* ready = window.session().activeFile();
  const auto* paused = fileById(window, held.loadingId);
  if (!ready || ready->metadata.id != held.activeId ||
      ready->metadata.availability.status != LoadStatus::Ready || !paused ||
      paused->metadata.availability.status != LoadStatus::Loading ||
      paused->metadata.availability.availableSamples != 0 ||
      !paused->navigationEnvelope.empty() ||
      paused->metadata.availability.generation != held.loadGeneration ||
      !window.sourceLoadActiveForTest(loadingKey)) {
    error = QStringLiteral("held load was published before the detection lock");
    return false;
  }
  return true;
}

bool loadStillUnpublished(MainWindow& window, const HeldSourceLoad& held, QString& why) {
  const auto* active = window.session().activeFile();
  const auto* loading = fileById(window, held.loadingId);
  const auto key = QString::fromStdString(held.loadingId);
  if (!active || active->metadata.id != held.activeId ||
      active->metadata.availability.status != LoadStatus::Ready) {
    why = QStringLiteral("active ready target changed");
    return false;
  }
  if (!loading) {
    why = QStringLiteral("second file disappeared");
    return false;
  }
  if (loading->metadata.availability.status != LoadStatus::Loading ||
      loading->metadata.availability.availableSamples != 0 ||
      !loading->navigationEnvelope.empty() ||
      loading->metadata.availability.generation != held.loadGeneration) {
    why = QString("project prefix overwritten status=%1 samples=%2 envelope=%3 generation=%4")
              .arg(static_cast<int>(loading->metadata.availability.status))
              .arg(QString::number(loading->metadata.availability.availableSamples))
              .arg(QString::number(loading->navigationEnvelope.size()))
              .arg(QString::number(loading->metadata.availability.generation));
    return false;
  }
  if (!window.sourceLoadActiveForTest(key) || !window.sourceLoadFinishedForTest(key) ||
      window.sourceLoadLoadedForTest(key) != held.sampleCount) {
    why = QString("finished loader was consumed active=%1 finished=%2 loaded=%3")
              .arg(window.sourceLoadActiveForTest(key) ? 1 : 0)
              .arg(window.sourceLoadFinishedForTest(key) ? 1 : 0)
              .arg(QString::number(window.sourceLoadLoadedForTest(key)));
    return false;
  }
  return true;
}

bool loadPublishedFromHeldReader(MainWindow& window, const HeldSourceLoad& held, QString& why) {
  const auto* loading = fileById(window, held.loadingId);
  const auto key = QString::fromStdString(held.loadingId);
  if (!loading) {
    why = QStringLiteral("second file disappeared before publish");
    return false;
  }
  if (loading->metadata.availability.status != LoadStatus::Ready ||
      loading->metadata.sampleCount != held.sampleCount ||
      loading->metadata.availability.availableSamples != held.sampleCount ||
      availableSamples(loading->metadata) != held.sampleCount ||
      loading->navigationEnvelope.empty() ||
      loading->metadata.availability.generation != held.loadGeneration ||
      window.sourceLoadActiveForTest(key)) {
    why = QString("held reader was not applied status=%1 samples=%2/%3 envelope=%4 generation=%5 loader=%6")
              .arg(static_cast<int>(loading->metadata.availability.status))
              .arg(QString::number(loading->metadata.availability.availableSamples))
              .arg(QString::number(loading->metadata.sampleCount))
              .arg(QString::number(loading->navigationEnvelope.size()))
              .arg(QString::number(loading->metadata.availability.generation))
              .arg(window.sourceLoadActiveForTest(key) ? 1 : 0);
    return false;
  }
  return true;
}

QString projectFile(const QTemporaryDir& directory) {
  const auto files = QDir(directory.path()).entryList({QStringLiteral("*.ssproj")}, QDir::Files);
  return files.size() == 1 ? directory.filePath(files.front()) : QString();
}

bool reopenRetainsFullPrefix(const QString& path, const HeldSourceLoad& held, QString& error) {
  Project stored;
  if (!ProjectFileCodec::load(path, stored, error)) return false;
  const auto storedFile = std::find_if(stored.files.begin(), stored.files.end(),
                                        [&](const FileState& file) {
                                          return file.metadata.id == held.loadingId;
                                        });
  if (storedFile == stored.files.end() ||
      storedFile->metadata.sampleCount != held.sampleCount ||
      storedFile->metadata.availability.availableSamples != held.sampleCount ||
      storedFile->metadata.availability.status != LoadStatus::Ready) {
    const auto samples = storedFile == stored.files.end()
                             ? SampleIndex{0}
                             : storedFile->metadata.availability.availableSamples;
    const auto count = storedFile == stored.files.end() ? SampleIndex{0}
                                                        : storedFile->metadata.sampleCount;
    const auto status = storedFile == stored.files.end()
                            ? -1
                            : static_cast<int>(storedFile->metadata.availability.status);
    error = QString("stored prefix %1/%2 status %3")
                .arg(QString::number(samples))
                .arg(QString::number(count))
                .arg(status);
    return false;
  }
  MainWindow reopened;
  if (!reopened.openProject(path)) {
    error = QStringLiteral("openProject failed");
    return false;
  }
  if (!QTest::qWaitFor(
          [&] {
            const auto* file = fileById(reopened, held.loadingId);
            return file && file->metadata.availability.status != LoadStatus::Loading;
          },
          10000)) {
    error = QStringLiteral("reopened load did not settle");
    return false;
  }
  const auto* file = fileById(reopened, held.loadingId);
  if (!file || file->metadata.sampleCount != held.sampleCount ||
      file->metadata.availability.status != LoadStatus::Ready ||
      availableSamples(file->metadata) != held.sampleCount) {
    error = QStringLiteral("reopened project did not keep the full prefix");
    return false;
  }
  return true;
}

void SsDeRpoStartTests::sourceLoadSurvivesStartCommitInstall() {
  armFixture();
  QTimer modals;
  QString modalFailure;
  rejectUnexpectedModals(modals, modalFailure);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareProject(directory, window, error), qPrintable(error));
  HeldSourceLoad held;
  QVERIFY2(armHeldSourceLoad(directory, window, held, error), qPrintable(error));
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(panel);
  auto gate = std::make_shared<std::atomic<bool>>(false);
  panel->setStartCompletionGateForTest(gate);
  bool startInstalled = false;
  QObject::connect(panel, &DetectionPanel::commitSettled, &window,
                   [&](bool publication, bool installed) {
                     if (!publication) startInstalled = installed;
                   });
  QVERIFY2(panel->startRequest(fixtureRequest(window, "source-load-start"), error),
           qPrintable(error));
  QVERIFY2(QTest::qWaitFor([&] { return panel->startCompletionGateBlockedForTest(); }, 20000),
           qPrintable(modalFailure));
  QVERIFY(panel->projectCommitInProgress());
  const auto epoch = window.session().contentEpoch();
  window.pollSourceLoadsForTest();
  QCOMPARE(window.session().contentEpoch(), epoch);
  QVERIFY2(loadStillUnpublished(window, held, error), qPrintable(error));
  gate->store(true);
  QVERIFY2(QTest::qWaitFor([&] { return !panel->projectCommitInProgress(); }, 20000),
           qPrintable(modalFailure));
  QVERIFY2(startInstalled, qPrintable(modalFailure));
  QVERIFY2(loadStillUnpublished(window, held, error), qPrintable(error));
  window.pollSourceLoadsForTest();
  QVERIFY2(loadPublishedFromHeldReader(window, held, error), qPrintable(error));
  const auto path = projectFile(directory);
  QVERIFY2(!path.isEmpty() && window.saveProject(path), qPrintable(path));
  QVERIFY2(reopenRetainsFullPrefix(path, held, error), qPrintable(error));
}

void SsDeRpoStartTests::sourceLoadSurvivesPublicationCommitInstall() {
  armFixture();
  QTimer modals;
  QString modalFailure;
  rejectUnexpectedModals(modals, modalFailure);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareProject(directory, window, error), qPrintable(error));
  HeldSourceLoad held;
  QVERIFY2(armHeldSourceLoad(directory, window, held, error), qPrintable(error));
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(panel);
  auto gate = std::make_shared<std::atomic<bool>>(false);
  panel->setPublicationGate(gate);
  bool publicationInstalled = false;
  QObject::connect(panel, &DetectionPanel::commitSettled, &window,
                   [&](bool publication, bool installed) {
                     if (publication) publicationInstalled = installed;
                   });
  QVERIFY2(panel->startRequest(fixtureRequest(window, "source-load-publication"), error),
           qPrintable(error));
  QVERIFY2(QTest::qWaitFor([&] { return panel->publicationCommitInProgress(); }, 30000),
           qPrintable(modalFailure));
  const auto epoch = window.session().contentEpoch();
  window.pollSourceLoadsForTest();
  QCOMPARE(window.session().contentEpoch(), epoch);
  QVERIFY2(loadStillUnpublished(window, held, error), qPrintable(error));
  gate->store(true);
  QVERIFY2(QTest::qWaitFor([&] { return !panel->publicationCommitInProgress(); }, 30000),
           qPrintable(modalFailure));
  QVERIFY2(publicationInstalled, qPrintable(modalFailure));
  QVERIFY2(loadStillUnpublished(window, held, error), qPrintable(error));
  window.pollSourceLoadsForTest();
  QVERIFY2(loadPublishedFromHeldReader(window, held, error), qPrintable(error));
  const auto path = projectFile(directory);
  QVERIFY2(!path.isEmpty() && window.saveProject(path), qPrintable(path));
  QVERIFY2(reopenRetainsFullPrefix(path, held, error), qPrintable(error));
}

void SsDeRpoStartTests::cancelledStartCommitAppliesFinishedSourceLoad() {
  armFixture();
  QTimer modals;
  QString modalFailure;
  rejectUnexpectedModals(modals, modalFailure);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareProject(directory, window, error), qPrintable(error));
  HeldSourceLoad held;
  QVERIFY2(armHeldSourceLoad(directory, window, held, error), qPrintable(error));
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(panel);
  auto gate = std::make_shared<std::atomic<bool>>(false);
  panel->setStartCommitGate(gate);
  bool rejected = false;
  QObject::connect(panel, &DetectionPanel::startRejected, &window,
                   [&](const QString& runId, const QString&) {
                     rejected = runId == QStringLiteral("source-load-cancel");
                   });
  QVERIFY2(panel->startRequest(fixtureRequest(window, "source-load-cancel"), error),
           qPrintable(error));
  QVERIFY2(QTest::qWaitFor([&] { return panel->startCommitGateBlocked(); }, 20000),
           qPrintable(modalFailure));
  QVERIFY(panel->projectCommitInProgress());
  const auto epoch = window.session().contentEpoch();
  window.pollSourceLoadsForTest();
  QCOMPARE(window.session().contentEpoch(), epoch);
  QVERIFY2(loadStillUnpublished(window, held, error), qPrintable(error));
  panel->cancelRun(QStringLiteral("source-load-cancel"));
  gate->store(true);
  QVERIFY2(QTest::qWaitFor(
               [&] { return rejected && !panel->projectCommitInProgress(); }, 20000),
           qPrintable(modalFailure));
  const auto* slot = activeSlot(window);
  QVERIFY(!slot || slot->committedRunId.empty());
  window.pollSourceLoadsForTest();
  QVERIFY2(loadPublishedFromHeldReader(window, held, error), qPrintable(error));
  QCOMPARE(window.session().contentEpoch(), epoch);
}

void SsDeRpoStartTests::renameResolvesChannelAfterModalProjectSwap() {
  armFixture();
  QTemporaryDir directory;
  MainWindow window;
  QString error;
  QVERIFY2(prepareProject(directory, window, error), qPrintable(error));
  const auto mark = window.session().addMark({{100, 10000}, {99.9e6, 100.1e6}});
  QVERIFY(!mark.empty());
  window.session().selectMarks({mark}, mark);
  QVERIFY(window.session().createChannelFromActiveMark());
  const auto id = window.session().activeChannel()->id;
  window.refresh();
  auto* tree = window.findChild<QTreeWidget*>("projectTree");
  QVERIFY(tree);
  QTreeWidgetItem* target = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT(([&] {
    for (auto* item : tree->findItems("*", Qt::MatchWildcard | Qt::MatchRecursive))
      if (item->data(0, Qt::UserRole).toString() == "channel" &&
          item->data(0, Qt::UserRole + 1).toString().toStdString() == id) {
        target = item;
        return true;
      }
    return false;
  })(), 5000);
  tree->expandAll();
  tree->scrollToItem(target);
  const auto point = tree->visualItemRect(target).center();
  bool opened = false, swapped = false, timedOut = false;
  QElapsedTimer elapsed;
  elapsed.start();
  QTimer driver;
  connect(&driver, &QTimer::timeout, &window, [&] {
    if (elapsed.elapsed() > 10000) {
      timedOut = true;
      if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
      if (auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget())) popup->close();
      return;
    }
    if (auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
      // Background publication replaces the Project without changing its
      // identity. Preserve a concurrent channel field when applying the name.
      auto replacement = window.session().project();
      for (auto& file : replacement.files)
        for (auto& channel : file.channels)
          if (channel.id == id) channel.psdAxisMinimum = -137.25;
      window.session().project() = std::move(replacement);
      swapped = true;
      input->setTextValue("Renamed after publication");
      input->accept();
      return;
    }
    if (!opened)
      if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
        for (auto* action : menu->actions())
          if (action->text().contains(QStringLiteral("重命名通道"))) {
            opened = true;
            const auto position = menu->actionGeometry(action).center();
            QTimer::singleShot(0, &window, [popup = QPointer<QMenu>(menu), position] {
              if (popup) QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier, position);
            });
            return;
          }
  });
  driver.start(10);
  QVERIFY(QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, point)));
  driver.stop();
  QVERIFY(!timedOut && opened && swapped);
  auto* file = window.session().fileForChannel(id);
  QVERIFY(file);
  const auto found = std::find_if(file->channels.begin(), file->channels.end(), [&](const auto& channel) { return channel.id == id; });
  QVERIFY(found != file->channels.end());
  QCOMPARE(found->name, std::string("Renamed after publication"));
  QCOMPARE(found->psdAxisMinimum, -137.25);
}

QTEST_MAIN(SsDeRpoStartTests)
#include "ss_de_rpo_start_tests.moc"
