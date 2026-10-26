#pragma once

#include <QDialog>
#include <QFileDialog>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QPersistentModelIndex>
#include <QScopeGuard>
#include <QTextBrowser>
#include <QTimer>
#include "infrastructure/detection_store.h"

inline void UxTests::uxR01_lateRunFilterPopupAndRefresh() {
  // UX-R5 removes the historical run-filter popup. Preserve this regression's
  // late-publication/refresh purpose on the authoritative current-result page.
  QTemporaryDir directory;
  MainWindow window;
  QString issue;
  QVERIFY(window.createProject(directory.path(), "current-publication", &issue));
  issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
  QVERIFY2(issue.isEmpty(), qPrintable(issue));
  expose(window);
  QTest::mouseClick(window.findChild<QAbstractButton*>("detectionResultsTab"), Qt::LeftButton);
  QVERIFY(!window.findChild<QWidget*>("annotationRunFilter"));
  const auto source = window.session().activeFile()->metadata;
  DetectionRequest request;
  request.runId = "ux-current-run-0123456789";
  request.sourceIdentity = annotationSourceIdentity(source, window.session().projectGeneration());
  request.profile = *findDetectionProfile("pipeline-fixture-v1");
  request.requestedRange = fullRange(source);
  request.contextRange = request.requestedRange;
  request.rangeOrigin = "available-file";
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY2(panel->startRequest(request, issue), qPrintable(issue));
  QTRY_VERIFY_WITH_TIMEOUT(currentDetectionSlot(*window.session().activeFile(), 0) &&
      currentDetectionSlot(*window.session().activeFile(), 0)->committedRunId == request.runId, 30000);
  auto* table = window.findChild<QTableView*>("detectionResultTable");
  const auto rows = table->model()->rowCount();
  const auto firstId = rows ? table->model()->index(0,1).data(Qt::UserRole) : QVariant{};
  for (int refresh = 0; refresh < 5; ++refresh) window.refresh();
  QCOMPARE(table->model()->rowCount(), rows);
  if (rows) QCOMPARE(table->model()->index(0,1).data(Qt::UserRole), firstId);
  QCOMPARE(window.session().activeFile()->detectionRuns.size(), std::size_t(1));
  panel->markConfigurationStale();
  QCOMPARE(table->model()->rowCount(), 0);
  QVERIFY(window.findChild<QLabel*>("currentDetectionStatus")->text().contains("待更新"));
  QCOMPARE(currentDetectionSlot(*window.session().activeFile(), 0)->committedRunId, request.runId);
}

inline void UxTests::uxD01_topDerivationDiscardsCrossFileDraft() {
  QTemporaryDir directory;
  MainWindow window;
  QString issue;
  QVERIFY(window.createProject(directory.path(), "derive-draft", &issue));
  issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
  QVERIFY2(issue.isEmpty(), qPrintable(issue));
  const auto firstId = window.session().activeFile()->metadata.id;
  issue = addSource(window, directory.path(), "IQ_B_FS1Msps_FC200MHz.dat");
  QVERIFY2(issue.isEmpty(), qPrintable(issue));
  auto history = historyFor(window);
  history.request.runId = "history-b-scn";
  history.request.profile = *findDetectionProfile("scn-max-psd-v1");
  history.request.spectrumMode = DetectionSpectrumMode::ModelDefault;
  window.session().activeFile()->detectionRuns.push_back(history);
  QVERIFY(window.session().activateFile(firstId));
  expose(window);
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  panel->refresh(); focusHistory(window, "history-b-scn");
  QVERIFY(uxDetectionPage(window));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* score = card->findChild<AdaptiveValueEdit*>("detectionScoreThreshold");
  QVERIFY(uxDetectionReveal(window, score));
  const auto accepted = card->request().parameters.scoreThreshold;
  score->setFocus(); score->selectAll(); QTest::keyClicks(score, "0.83");
  QVERIFY(score->property("parameterDraft").toBool());
  auto* derive = panel->findChild<QPushButton*>("deriveWidebandDetection");
  QVERIFY(derive && derive->isVisible() && derive->isEnabled());
  QTest::mouseClick(derive, Qt::LeftButton);
  QCOMPARE(card->request().sourceIdentity.fileId, history.request.sourceIdentity.fileId);
  QCOMPARE(card->request().profile.id, std::string("scn-max-psd-v1"));
  QCOMPARE(card->request().parentRunId, std::optional<std::string>("history-b-scn"));
  QVERIFY(!score->property("parameterDraft").toBool());
  auto* model = card->findChild<QComboBox*>("detectionModel");
  model->setFocus(); QTest::keyClick(model, Qt::Key_Home);
  QCOMPARE(card->request().profile.id, std::string("ffscn-gui-iq-v1"));
  QCOMPARE(card->request().parameters.scoreThreshold, accepted);
  QVERIFY(window.session().activateFile(firstId)); window.refresh();
  QCOMPARE(card->request().parameters.scoreThreshold, accepted);
  QVERIFY(window.session().activeFile()->detectionRuns.empty());
}

inline void UxTests::uxR03_taskRowActions() {
  QTemporaryDir directory;
  MainWindow window;
  QString issue;
  QVERIFY(window.createProject(directory.path(), "task-row", &issue));
  issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
  QVERIFY2(issue.isEmpty(), qPrintable(issue));
  auto first = historyFor(window), second = first;
  second.request.runId = "other-row";
  auto* source = window.session().activeFile();
  source->detectionRuns = {first, second};
  expose(window);
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  panel->refresh(); focusHistory(window, "ux-history");
  auto* list = window.findChild<QListView*>("detectionRunList");
  const auto row = list->model()->index(0, 0);
  list->scrollTo(row, QAbstractItemView::PositionAtTop);
  QTest::qWait(30);
  const auto rect = list->visualRect(row);
  // At the approved fullscreen logical width the four labelled actions are
  // in one row; click their actual painted hit areas, then assert identity.
  QVERIFY(rect.width() > 700 && rect.bottom() < list->viewport()->height());
  const auto actionPoint = [&](int action) { return QPoint(rect.left() + 12 + action * 168 + 80, rect.bottom() - 26); };
  QSignalSpy focused(panel, &DetectionPanel::runFocused);
  QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, actionPoint(0));
  QTRY_COMPARE(focused.count(), 1);
  QCOMPARE(focused.first().first().toString(), QString("ux-history"));
  bool detailsSeen = false;
  QTimer::singleShot(20, &window, [&] {
    if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
      auto* facts = dialog->findChild<QTextBrowser*>("detectionRunFacts");
      detailsSeen = facts && facts->toPlainText().contains("ux-history");
      QTest::keyClick(dialog, Qt::Key_Escape);
    }
  });
  QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, actionPoint(1));
  QVERIFY(detailsSeen);
  window.activateWindow();
  QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, actionPoint(2));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  QCOMPARE(card->request().parentRunId, std::optional<std::string>("ux-history"));
  QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, actionPoint(3));
  QCOMPARE(source->detectionRuns.front().lifecycle, DetectionLifecycle::Completed);
  const auto display = row.data(Qt::AccessibleTextRole).toString();
  QVERIFY2(display.contains("完成") && display.contains("模型") && display.contains("阶段") && display.contains("结果"), qPrintable(display));
  QSignalSpy configured(panel, &DetectionPanel::configurationRequested);
  QTest::mousePress(list->viewport(), Qt::LeftButton, Qt::NoModifier, actionPoint(2));
  source->detectionRuns.erase(source->detectionRuns.begin());
  panel->refresh();
  QTest::mouseRelease(list->viewport(), Qt::LeftButton, Qt::NoModifier, actionPoint(2));
  QCOMPARE(configured.count(), 0);
  QCOMPARE(card->request().parentRunId, std::optional<std::string>("ux-history"));
}

inline void UxTests::uxR03_taskKeyboardAndDetails() {
  QTemporaryDir directory;
  MainWindow window;
  QString issue;
  QVERIFY(window.createProject(directory.path(), "task-keyboard", &issue));
  issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
  QVERIFY2(issue.isEmpty(), qPrintable(issue));
  const auto history = historyFor(window);
  window.session().activeFile()->detectionRuns.push_back(history);
  expose(window);
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  panel->refresh();
  focusHistory(window, QString::fromStdString(history.request.runId));
  QVERIFY(uxDetectionPage(window));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* score = card->findChild<AdaptiveValueEdit*>("detectionScoreThreshold");
  QVERIFY(uxDetectionReveal(window, score));
  score->setFocus(); score->selectAll(); QTest::keyClicks(score, "0.83");
  const auto committed = card->request().parameters.scoreThreshold;
  auto* list = window.findChild<QListView*>("detectionRunList");
  QVERIFY(list && list->isVisible());
  list->setFocus();
  QTRY_VERIFY(list->hasFocus());
  QSignalSpy focused(panel, &DetectionPanel::runFocused);
  QTest::keyClick(list, Qt::Key_Return);
  QTRY_COMPARE(focused.count(), 1);
  QCOMPARE(focused.first().first().toString(), QString("ux-history"));
  QCOMPARE(score->text(), QString("0.83"));
  QVERIFY(score->property("parameterDraft").toBool());
  QCOMPARE(card->request().parameters.scoreThreshold, committed);

  bool detailsSeen = false, factsCorrect = false, technicalCorrect = false;
  QTimer::singleShot(30, &window, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (!dialog) return;
    detailsSeen = true;
    if (auto* facts = dialog->findChild<QTextBrowser*>("detectionRunFacts"))
      factsCorrect = facts->toPlainText().contains("ux-history") &&
                     facts->toPlainText().contains("冻结请求") &&
                     facts->toPlainText().contains("已覆盖");
    if (auto* toggle = dialog->findChild<QToolButton*>("detectionRunTechnicalToggle")) {
      QTest::mouseClick(toggle, Qt::LeftButton);
      if (auto* json = dialog->findChild<QPlainTextEdit*>("detectionRunTechnicalJson")) {
        const auto object = QJsonDocument::fromJson(json->toPlainText().toUtf8()).object();
        technicalCorrect = json->isVisible() && object == encodeDetectionRun(history);
      }
    }
    QTest::keyClick(dialog, Qt::Key_Escape);
  });
  list->setFocus();
  QTest::keyClick(list, Qt::Key_Return, Qt::AltModifier);
  QVERIFY(detailsSeen && factsCorrect && technicalCorrect);
  QCOMPARE(focused.count(), 1);
  QCOMPARE(score->text(), QString("0.83"));
  QVERIFY(score->property("parameterDraft").toBool());
  QCOMPARE(card->request().parameters.scoreThreshold, committed);
  QVERIFY(!card->request().parentRunId);

  QTest::qWait(30);
  window.activateWindow();
  list->setFocus();
  QTRY_VERIFY(list->hasFocus());
  QSignalSpy derived(panel->findChild<QAction*>("taskDeriveAction"), &QAction::triggered);
  QTest::keyClick(list, Qt::Key_Return, Qt::ControlModifier);
  QTRY_COMPARE(derived.count(), 1);
  QTRY_COMPARE(card->request().parentRunId, std::optional<std::string>("ux-history"));
  QCOMPARE(card->request().parameters.scoreThreshold, history.request.parameters.scoreThreshold);
  QCOMPARE(card->request().requestedRange, history.request.requestedRange);
  QCOMPARE(card->request().rangeOrigin, std::string("custom"));
  QVERIFY(!score->property("parameterDraft").toBool());
  QCOMPARE(window.session().activeFile()->detectionRuns.size(), std::size_t{1});
  auto* cancel = panel->findChild<QAction*>("taskCancelAction");
  QVERIFY(cancel && !cancel->isEnabled());
  list->setFocus(); QTest::keyClick(list, Qt::Key_X, Qt::ControlModifier | Qt::ShiftModifier);
  QCOMPARE(window.session().activeFile()->detectionRuns.front().lifecycle, DetectionLifecycle::Completed);
}

inline void UxTests::uxD03_activeTaskCancellationAndFrozenDisplay() {
  const auto previousFixture = qApp->property("enableDetectionTestFixture");
  qApp->setProperty("enableDetectionTestFixture", true);
  const auto restore = qScopeGuard([&] { qApp->setProperty("enableDetectionTestFixture", previousFixture); });
  QTemporaryDir directory;
  MainWindow window;
  QString issue;
  QVERIFY(window.createProject(directory.path(), "active-task", &issue));
  const auto path = directory.filePath("IQ_A_FS1Msps_FC100MHz.dat");
  QFile iq(path);
  QVERIFY(iq.open(QIODevice::WriteOnly));
  QCOMPARE(iq.write(QByteArray(4'000'000, '\0')), qint64{4'000'000});
  iq.close();
  QVERIFY2(window.addIqFile(path, &issue), qPrintable(issue));
  QTRY_COMPARE_WITH_TIMEOUT(window.session().activeFile()->metadata.availability.status, LoadStatus::Ready, 10000);
  const auto firstId = window.session().activeFile()->metadata.id;
  issue = addSource(window, directory.path(), "IQ_B_FS1Msps_FC200MHz.dat");
  QVERIFY2(issue.isEmpty(), qPrintable(issue));
  const auto secondId = window.session().activeFile()->metadata.id;
  auto completedB = historyFor(window);
  completedB.request.runId = "terminal-b";
  completedB.request.contextRange = fullRange(window.session().activeFile()->metadata);
  // historyFor may carry its original manifest identity. This renamed history
  // must be real durable evidence before exercising the ordinary save path.
  completedB.manifestRelativePath.clear();
  QVERIFY2(writeDetectionRun(QString::fromStdString(window.session().project().assetDirectory), completedB, issue), qPrintable(issue));
  window.session().activeFile()->detectionRuns.push_back(completedB);
  QVERIFY(window.session().activateFile(firstId));
  expose(window);
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  QVERIFY(uxDetectionPage(window));
  DetectionRun configuration;
  configuration.request.runId = "ux-fixture-template";
  configuration.request.sourceIdentity = annotationSourceIdentity(window.session().activeFile()->metadata, window.session().projectGeneration());
  configuration.request.profile = *findDetectionProfile("pipeline-fixture-v1");
  configuration.request.profile.fftSize = 2048;
  configuration.request.profile.hopSeconds = .0001;
  configuration.request.profile.modelFrames = 10;
  configuration.request.profile.modelHopFrames = 1;
  configuration.request.requestedRange = fullRange(window.session().activeFile()->metadata);
  configuration.request.contextRange = configuration.request.requestedRange;
  configuration.request.rangeOrigin = "custom";
  card->configure(&configuration);
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  QVERIFY(run && run->isVisible() && run->isEnabled());
  QTest::mouseClick(run, Qt::LeftButton);
  QTRY_VERIFY_WITH_TIMEOUT(!window.session().activeFile()->detectionRuns.empty(), 10000);
  const auto runId = window.session().activeFile()->detectionRuns.front().request.runId;
  QVERIFY(!runId.empty() && runId != "terminal-b");
  const auto find = [&]() -> std::optional<DetectionRun> {
    for (const auto& source : window.session().project().files)
      for (const auto& value : source.detectionRuns)
        if (value.request.runId == runId) return value;
    return {};
  };
  QTRY_VERIFY_WITH_TIMEOUT(find() && find()->completedWindows > 0 && find()->lifecycle == DetectionLifecycle::Running, 15000);
  // The actual modal dialogs are driven below. A broken save/open path must
  // unwind its dialog rather than hanging this regression's nested event loop.
  const auto boundedDialogCall = [&](const auto& operation) {
    QElapsedTimer deadline;deadline.start();
    QTimer watchdog;watchdog.setInterval(10);bool timedOut=false;
    QObject::connect(&watchdog,&QTimer::timeout,&window,[&] {
      if(deadline.elapsed()<10000)return;
      timedOut=true;
      if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();
    });
    watchdog.start();const bool result=operation();watchdog.stop();
    return result && !timedOut;
  };
  // A copied Save As must not switch the live worker away from the asset
  // directory to which its frozen job continues publishing chunks.
  const auto originalAssets = window.session().project().assetDirectory;
  const auto copiedProject = directory.filePath("running-copy.ssproj");
  bool assetCopyWarning = false;
  QTimer::singleShot(0, &window, [&] {
    if (auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
      assetCopyWarning = warning->windowTitle() == "暂不能复制工程资产";
      warning->accept();
    }
  });
  QVERIFY(!boundedDialogCall([&]{return window.saveProject(copiedProject);}));
  QVERIFY(assetCopyWarning && !QFileInfo::exists(copiedProject));
  QCOMPARE(window.session().project().assetDirectory, originalAssets);
  const auto frozen = encodeDetectionRequest(find()->request);
  const auto firstChunks = find()->chunks.size();
  QVERIFY(firstChunks > 0);
  const auto firstChunk = find()->chunks.front();
  const QDir assets(QString::fromStdString(window.session().project().assetDirectory));
  QVERIFY(assets.exists());
  QFile durablePrefix(assets.filePath(QString::fromStdString(firstChunk.relativePath)));
  QVERIFY(durablePrefix.open(QIODevice::ReadOnly));
  const auto prefixBytes = durablePrefix.readAll();
  durablePrefix.close();
  QVERIFY(!prefixBytes.isEmpty());
  const auto originalProject=directory.filePath("active-task.ssproj");
  QVERIFY(boundedDialogCall([&]{return window.saveProject(originalProject);}));
  QCOMPARE(window.session().project().assetDirectory,originalAssets);
  QVERIFY(find() && find()->lifecycle==DetectionLifecycle::Running);

  const auto referenceProject=directory.filePath("running-reference.ssproj");
  auto* saveAs=window.findChild<QAction*>("saveProjectAsAction");QVERIFY(saveAs && saveAs->isEnabled());
  const bool nativeDialogsDisabled=QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
  QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs,true);
  const auto restoreDialogs=qScopeGuard([&]{QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs,nativeDialogsDisabled);});
  bool fileDialogSeen=false,referenceChoiceSeen=false,unexpectedDialog=false;
  QTimer saveAsDriver;saveAsDriver.setInterval(10);
  QObject::connect(&saveAsDriver,&QTimer::timeout,&window,[&] {
    if(auto* dialog=qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
      if(fileDialogSeen)return;
      fileDialogSeen=dialog->windowTitle()=="工程另存为" && dialog->acceptMode()==QFileDialog::AcceptSave;
      if(!fileDialogSeen){unexpectedDialog=true;dialog->reject();return;}
      dialog->setDirectory(directory.path());dialog->selectFile(QFileInfo(referenceProject).fileName());
      QMetaObject::invokeMethod(dialog,"accept",Qt::QueuedConnection);
    } else if(auto* question=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
      if(question->windowTitle()=="伴随资产" && question->standardButtons().testFlag(QMessageBox::No)) {
        referenceChoiceSeen=true;QTest::mouseClick(question->button(QMessageBox::No),Qt::LeftButton);
      } else {unexpectedDialog=true;question->reject();}
    }
  });
  saveAsDriver.start();
  const bool saveAsReturned=boundedDialogCall([&]{saveAs->trigger();return true;});
  saveAsDriver.stop();
  QVERIFY(saveAsReturned && fileDialogSeen && referenceChoiceSeen && !unexpectedDialog);
  QVERIFY(QFileInfo::exists(referenceProject));
  QCOMPARE(window.session().project().assetDirectory,originalAssets);
  QVERIFY(find() && find()->lifecycle==DetectionLifecycle::Running);
  const auto readContainer=[&](const QString& projectPath) {
    QFile file(projectPath);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
  };
  const auto referenceBeforeSave=QJsonDocument::fromJson(readContainer(referenceProject)).object();
  QCOMPARE(referenceBeforeSave["assetMode"].toString(),QString("reference"));
  QCOMPARE(QDir(referenceBeforeSave["assetDirectory"].toString()).absolutePath(),QDir(QString::fromStdString(originalAssets)).absolutePath());
  // This is the current Reference path: ordinary Save must use Preserve,
  // neither converting it to Copy nor moving the active worker's asset root.
  QVERIFY(boundedDialogCall([&]{return window.saveProject(referenceProject);}));
  QCOMPARE(window.session().project().assetDirectory,originalAssets);
  const auto savedWhileRunning=readContainer(referenceProject);QVERIFY(!savedWhileRunning.isEmpty());
  const auto referenceAfterSave=QJsonDocument::fromJson(savedWhileRunning).object();
  QCOMPARE(referenceAfterSave["assetMode"].toString(),QString("reference"));
  QCOMPARE(referenceAfterSave["assetDirectory"],referenceBeforeSave["assetDirectory"]);
  QVERIFY(!QFileInfo::exists(referenceProject+".assets"));
  QJsonObject savedLiveIndex;
  for(const auto entry:referenceAfterSave["assets"].toArray()) {
    const auto object=entry.toObject();
    if(object["runId"].toString()==QString::fromStdString(runId))savedLiveIndex=object;
  }
  QCOMPARE(savedLiveIndex["integrity"].toString(),QString("detection-live-v1"));
  QCOMPARE(savedLiveIndex["requestSha256"].toString(),QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(frozen).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex()));
  const auto savedMinimumChunks=savedLiveIndex["minimumChunks"].toArray();
  QVERIFY(savedMinimumChunks.size()>=static_cast<qsizetype>(firstChunks));
  const auto savedCompleted=find()->completedWindows;const auto savedChunks=find()->chunks.size();
  QTRY_VERIFY_WITH_TIMEOUT(find() && find()->lifecycle==DetectionLifecycle::Running && find()->completedWindows>savedCompleted && find()->chunks.size()>savedChunks,15000);
  QCOMPARE(window.session().project().assetDirectory,originalAssets);
  QCOMPARE(encodeDetectionRequest(find()->request),frozen);
  window.activateWindow();
  auto* palette = window.findChild<QComboBox*>("colormap");
  palette->setFocus(); QTest::keyClick(palette, Qt::Key_Down);
  for (const auto* name : {"stftFft", "psdFft"}) {
    if (QString(name) == "psdFft") QTest::mouseClick(window.findChild<QPushButton*>("psdMode"), Qt::LeftButton);
    auto* combo = window.findChild<QComboBox*>(name);
    QVERIFY(combo && combo->isVisible());
    combo->setFocus();
    const int target = combo->findText("4096");
    QVERIFY(target >= 0);
    QTest::keyClick(combo, Qt::Key_Home);
    for (int i = 0; i < target; ++i) QTest::keyClick(combo, Qt::Key_Down);
    QCOMPARE(combo->currentIndex(), target);
  }
  QTest::mouseClick(chartSettingsEntry(window,"stftSettings"), Qt::LeftButton);
  auto* dynamic = window.findChild<QComboBox*>("dynamic");
  QVERIFY(dynamic && dynamic->isVisible());
  dynamic->setFocus(); dynamic->lineEdit()->selectAll();
  QTest::keyClicks(dynamic->lineEdit(), "72"); QTest::keyClick(dynamic->lineEdit(), Qt::Key_Return);
  QTest::keyClick(dynamic->lineEdit(), Qt::Key_Escape);
  QCOMPARE(encodeDetectionRequest(find()->request), frozen);
  QVERIFY(!detectionIsTerminal(find()->lifecycle));
  QVERIFY(window.session().activateFile(secondId));
  window.refresh();
  QVERIFY(uxDetectionPage(window));
  auto* vagueCancel = window.findChild<QPushButton*>("cancelDetectionCardRun");
  QVERIFY(vagueCancel && !vagueCancel->isEnabled());
  panel->refresh(); focusHistory(window, "terminal-b");
  auto* list = window.findChild<QListView*>("detectionRunList");
  list->setFocus();
  QTest::keyClick(list, Qt::Key_X, Qt::ControlModifier | Qt::ShiftModifier);
  QVERIFY(!detectionIsTerminal(find()->lifecycle));
  focusHistory(window, QString::fromStdString(runId));
  list->setFocus(); QTRY_VERIFY(list->hasFocus());
  QVERIFY(panel->findChild<QAction*>("taskCancelAction")->isEnabled());
  QTest::keyClick(list, Qt::Key_X, Qt::ControlModifier | Qt::ShiftModifier);
  QTRY_VERIFY_WITH_TIMEOUT(find() && detectionIsTerminal(find()->lifecycle), 15000);
  QCOMPARE(find()->lifecycle, DetectionLifecycle::Cancelled);
  QCOMPARE(window.session().activeFile()->metadata.id, secondId);
  QCOMPARE(encodeDetectionRequest(find()->request), frozen);
  QVERIFY(find()->completedWindows > 0 && find()->completedWindows < find()->eligibleWindows);
  QVERIFY(find()->chunks.size() >= firstChunks);
  QCOMPARE(window.session().activeFile()->detectionRuns.front().lifecycle, DetectionLifecycle::Completed);
  DetectionRun reopened;
  QVERIFY2(loadDetectionRun(assets.absolutePath(), QString::fromStdString(runId), reopened, issue), qPrintable(issue));
  QCOMPARE(reopened.lifecycle, DetectionLifecycle::Cancelled);
  QCOMPARE(encodeDetectionRequest(reopened.request), frozen);
  QCOMPARE(reopened.chunks.size(), find()->chunks.size());
  QCOMPARE(reopened.chunks.front().sha256, firstChunk.sha256);
  QVERIFY(durablePrefix.open(QIODevice::ReadOnly));
  QCOMPARE(durablePrefix.readAll(), prefixBytes);
  QVERIFY(std::any_of(reopened.coverage.begin(), reopened.coverage.end(), [](const auto& part) { return part.kind == DetectionCoverageKind::Unread; }));
  durablePrefix.close();
  // The container still has its Running-time request and minimum prefix. Its
  // bytes must not be refreshed to make the later Cancelled manifest load.
  QCOMPARE(readContainer(referenceProject),savedWhileRunning);
  const auto latestChunks=encodeDetectionRun(reopened)["chunks"].toArray();
  QVERIFY(latestChunks.size()>savedMinimumChunks.size());
  for(qsizetype i=0;i<savedMinimumChunks.size();++i)QCOMPARE(latestChunks[i],savedMinimumChunks[i]);
  MainWindow restoredWindow;
  QVERIFY(boundedDialogCall([&]{return restoredWindow.openProject(referenceProject);}));
  QCOMPARE(restoredWindow.session().project().assetDirectory,originalAssets);
  const auto findRestored=[&]() -> std::optional<DetectionRun> {
    for(const auto& source:restoredWindow.session().project().files)
      for(const auto& value:source.detectionRuns)if(value.request.runId==runId)return value;
    return {};
  };
  QVERIFY(findRestored());const auto restoredRun=*findRestored();
  QCOMPARE(restoredRun.lifecycle,DetectionLifecycle::Cancelled);
  QCOMPARE(restoredRun.completedWindows,reopened.completedWindows);
  QCOMPARE(restoredRun.chunks.size(),reopened.chunks.size());
  QCOMPARE(encodeDetectionRequest(restoredRun.request),frozen);
  QCOMPARE(encodeDetectionRun(restoredRun)["chunks"].toArray(),latestChunks);
  QVERIFY(std::any_of(restoredRun.coverage.begin(),restoredRun.coverage.end(),[](const auto& part){return part.kind==DetectionCoverageKind::Unread;}));
  // After reopening, an idle ordinary Save must still preserve Reference.
  QVERIFY(boundedDialogCall([&]{return restoredWindow.saveProject(referenceProject);}));
  QCOMPARE(restoredWindow.session().project().assetDirectory,originalAssets);
  const auto idleContainer=QJsonDocument::fromJson(readContainer(referenceProject)).object();
  QCOMPARE(idleContainer["assetMode"].toString(),QString("reference"));
  QCOMPARE(idleContainer["assetDirectory"],referenceBeforeSave["assetDirectory"]);
  QVERIFY(!QFileInfo::exists(referenceProject+".assets"));
}
