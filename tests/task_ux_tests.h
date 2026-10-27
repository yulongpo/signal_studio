#pragma once

#include <algorithm>
#include <atomic>
#include <QElapsedTimer>
#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QtEndian>
#include <QWheelEvent>
#include <QFileDialog>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QPersistentModelIndex>
#include <QScopeGuard>
#include <QTextBrowser>
#include <QTimer>
#include <QtEndian>
#include <cmath>
#include "infrastructure/detection_store.h"

inline void UxTests::uxR01_lateRunFilterPopupAndRefresh() {
  // UX-R5 removes the historical run-filter popup. Preserve this regression's
  // late-publication/refresh purpose on the authoritative current-result page.
  QTemporaryDir directory;
  MainWindow window;
  QString issue;
  QVERIFY(window.createProject(directory.path(), "current-publication", &issue));
  // A tone gives the synthetic pipeline worker nonempty results. Silence
  // would make the result-preservation assertions pass with zero old rows.
  QFile iq(directory.filePath("IQ_A_FS1Msps_FC100MHz.dat"));
  QByteArray samples(20000 * 4, '\0');
  for (int sample = 0; sample < 20000; ++sample) {
    const double angle = 2 * 3.14159265358979323846 * .125 * sample;
    qToLittleEndian<qint16>(static_cast<qint16>(12000 * std::cos(angle)), samples.data() + sample * 4);
    qToLittleEndian<qint16>(static_cast<qint16>(12000 * std::sin(angle)), samples.data() + sample * 4 + 2);
  }
  QVERIFY(iq.open(QIODevice::WriteOnly));
  QCOMPARE(iq.write(samples), qint64(samples.size()));
  iq.close();
  QVERIFY2(window.addIqFile(iq.fileName(), &issue), qPrintable(issue));
  QTRY_COMPARE_WITH_TIMEOUT(window.session().activeFile()->metadata.availability.status, LoadStatus::Ready, 10000);
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
  QVERIFY(rows > 0);
  const auto firstId = table->model()->index(0,1).data(Qt::UserRole);
  for (int refresh = 0; refresh < 5; ++refresh) window.refresh();
  QCOMPARE(table->model()->rowCount(), rows);
  QCOMPARE(table->model()->index(0,1).data(Qt::UserRole), firstId);
  QCOMPARE(window.session().activeFile()->detectionRuns.size(), std::size_t(1));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* score = card->findChild<AdaptiveValueEdit*>("detectionScoreThreshold");
  QVERIFY(uxDetectionReveal(window, score));
  window.session().selectMarks({firstId.toString().toStdString()});
  window.refresh();
  const auto selected = window.session().activeFile()->selectedMarkIds;
  const auto oldResult = encodeAnnotation(*findAnnotation(*window.session().activeFile(), firstId.toString().toStdString()));
  score->setFocus(); score->selectAll(); QTest::keyClicks(score, "0.55");
  QVERIFY(score->property("parameterDraft").toBool());
  window.refresh();
  QCOMPARE(table->model()->rowCount(), rows);
  QVERIFY(!currentDetectionSlot(*window.session().activeFile(), 0)->stale);
  QTest::keyClick(score, Qt::Key_Escape);
  QVERIFY(!score->property("parameterDraft").toBool());
  QCOMPARE(table->model()->rowCount(), rows);
  score->selectAll(); QTest::keyClicks(score, "0.55");
  QTest::keyClick(score, Qt::Key_Return);
  QVERIFY(currentDetectionSlot(*window.session().activeFile(), 0)->stale);
  QCOMPARE(table->model()->rowCount(), rows);
  QCOMPARE(table->model()->index(0,1).data(Qt::UserRole), firstId);
  QCOMPARE(window.session().activeFile()->selectedMarkIds, selected);
  auto* model = card->findChild<QComboBox*>("detectionModel");
  model->setCurrentIndex(model->findData("scn-max-psd-v1"));
  QCOMPARE(card->request().profile.id, std::string("scn-max-psd-v1"));
  QCOMPARE(table->model()->rowCount(), rows);
  panel->markConfigurationStale();
  QCOMPARE(table->model()->rowCount(), rows);
  const auto status = window.findChild<QLabel*>("currentDetectionStatus")->text();
  QVERIFY(status.contains("待更新") && status.contains("当前显示上次检测结果"));
  QCOMPARE(currentDetectionSlot(*window.session().activeFile(), 0)->committedRunId, request.runId);
  QCOMPARE(encodeAnnotation(*findAnnotation(*window.session().activeFile(), firstId.toString().toStdString())), oldResult);
  QVERIFY(annotationOverlayVisible(*window.session().activeFile(), *findAnnotation(*window.session().activeFile(), firstId.toString().toStdString())));
  for (const auto* tab : {"manualAnnotationsTab", "taskTab", "logTab", "detectionResultsTab"})
    QTest::mouseClick(window.findChild<QAbstractButton*>(tab), Qt::LeftButton);
  QCOMPARE(table->model()->rowCount(), rows);
  const auto projectPath = directory.filePath("current-publication.ssproj");
  QVERIFY(window.saveProject(projectPath));
  QVERIFY(window.openProject(projectPath));
  QTest::mouseClick(window.findChild<QAbstractButton*>("detectionResultsTab"), Qt::LeftButton);
  window.refresh();
  QCOMPARE(table->model()->rowCount(), rows);
  QCOMPARE(table->model()->index(0,1).data(Qt::UserRole), firstId);
  QVERIFY(currentDetectionSlot(*window.session().activeFile(), 0)->stale);
  QCOMPARE(encodeAnnotation(*findAnnotation(*window.session().activeFile(), firstId.toString().toStdString())), oldResult);
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

inline void UxTests::uxD09_intermediatePreviewAndRedetectClear() {
  const auto previousFixture = qApp->property("enableDetectionTestFixture");
  qApp->setProperty("enableDetectionTestFixture", true);
  const auto restore = qScopeGuard([&] {
    qApp->setProperty("enableDetectionTestFixture", previousFixture);
  });
  QTemporaryDir directory;
  MainWindow window;
  QString issue;
  QVERIFY(window.createProject(directory.path(), "preview-clear", &issue));
  const auto path = directory.filePath("IQ_A_FS1Msps_FC100MHz.dat");
  QFile iq(path);
  QVERIFY(iq.open(QIODevice::WriteOnly));
  QByteArray samples(20000 * 4, '\0');
  for (int sample = 0; sample < 20000; ++sample) {
    const double angle = 2 * 3.14159265358979323846 * .125 * sample;
    qToLittleEndian<qint16>(static_cast<qint16>(12000 * std::cos(angle)),
                            samples.data() + sample * 4);
    qToLittleEndian<qint16>(static_cast<qint16>(12000 * std::sin(angle)),
                            samples.data() + sample * 4 + 2);
  }
  QCOMPARE(iq.write(samples), qint64(samples.size()));
  iq.close();
  QVERIFY2(window.addIqFile(path, &issue), qPrintable(issue));
  QTRY_COMPARE_WITH_TIMEOUT(
      window.session().activeFile()->metadata.availability.status, LoadStatus::Ready,
      10000);
  expose(window);
  QTest::mouseClick(window.findChild<QAbstractButton*>("detectionResultsTab"),
                    Qt::LeftButton);
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  auto* status = window.findChild<QLabel*>("currentDetectionStatus");
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  QVERIFY(panel && plot && status && card);
  QVERIFY(uxDetectionPage(window));
  DetectionRequest request;
  request.runId = "preview-clear-initial";
  request.sourceIdentity = annotationSourceIdentity(
      window.session().activeFile()->metadata, window.session().projectGeneration());
  request.profile = *findDetectionProfile("pipeline-fixture-v1");
  request.requestedRange = fullRange(window.session().activeFile()->metadata);
  request.contextRange = request.requestedRange;
  request.rangeOrigin = "available-file";
  QVERIFY2(panel->startRequest(request, issue), qPrintable(issue));
  const auto committed = [&](const std::string& runId) {
    const auto* current = window.session().activeFile();
    const auto* slot = current ? currentDetectionSlot(
        *current, current->metadata.sampleFormat.selectedChannel) : nullptr;
    return slot && !slot->stale && slot->committedRunId == runId;
  };
  QTRY_VERIFY_WITH_TIMEOUT(committed(request.runId), 20000);
  auto* file = window.session().activeFile();
  std::vector<std::string> formalIds;
  for (const auto& mark : file->marks)
    if (mark.sourceKind == AnnotationSource::Algorithm &&
        annotationInContext(*file, mark))
      formalIds.push_back(mark.id);
  QVERIFY(!formalIds.empty());
  const auto manualId = window.session().addMark(
      {{100, 400}, {99.9e6, 100.05e6}});
  QVERIFY(findAnnotation(*file, manualId) &&
          findAnnotation(*file, manualId)->sourceKind == AnnotationSource::Manual);
  const auto manualBefore = encodeAnnotation(*findAnnotation(*file, manualId));
  auto* timer = panel->findChild<QTimer*>("detectionTaskPoll");
  QVERIFY(timer);
  timer->stop();
  const auto generations = plot->powerGenerationCount();
  DetectionPreviewSnapshot preview;
  preview.runId = "preview-run";
  preview.fileId = file->metadata.id;
  preview.events.push_back({});
  preview.events.back().trackId = "preview-track";
  preview.events.back().sequence = 1;
  preview.events.back().observedSupport = {1000, 5000};
  preview.events.back().frequency = {99.9e6, 100.1e6};
  preview.dropped = 2;
  file->selectedMarkIds = {formalIds.front(), "preview-track"};
  file->activeMarkId = "preview-track";
  file->hoveredMarkId = "preview-track";
  panel->presentLivePreview(preview, false);
  QCOMPARE(plot->property("detectionPreviewCount").toInt(), 1);
  QVERIFY(status->text().contains("预览"));
  plot->selectDetectionPreview("preview-track");
  QCOMPARE(plot->selectedDetectionPreview(), QString("preview-track"));
  panel->presentLivePreview(preview, true);
  QCOMPARE(plot->property("detectionPreviewCount").toInt(), 0);
  QCOMPARE(plot->selectedDetectionPreview(), QString());
  QVERIFY(!status->text().contains("预览"));
  QCOMPARE(file->activeMarkId, formalIds.front());
  QCOMPARE(file->hoveredMarkId, std::string{});
  QVERIFY(std::find(file->selectedMarkIds.begin(), file->selectedMarkIds.end(),
                    "preview-track") == file->selectedMarkIds.end());
  QVERIFY(std::find(file->selectedMarkIds.begin(), file->selectedMarkIds.end(),
                    formalIds.front()) != file->selectedMarkIds.end());
  QVERIFY(findAnnotation(*file, formalIds.front()));
  QVERIFY(findAnnotation(*file, manualId));
  QCOMPARE(plot->powerGenerationCount(), generations);
  panel->presentLivePreview(preview, false);
  QCOMPARE(plot->property("detectionPreviewCount").toInt(), 1);
  QVERIFY(uxDetectionFold(window, "detectionResourceParametersToggle"));
  auto* box = window.findChild<QCheckBox*>("showIntermediateDetectionResults");
  QVERIFY(uxDetectionReveal(window, box));
  QCOMPARE(box->text(), QString("显示中间结果"));
  QVERIFY(box->isChecked());
  const QPointF center(box->rect().center());
  QWheelEvent wheel(center, box->mapToGlobal(center.toPoint()), QPoint(),
                    QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                    Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(box, &wheel);
  QVERIFY(box->isChecked());
  box->setChecked(false);
  QCOMPARE(plot->property("detectionPreviewCount").toInt(), 0);
  QVERIFY(!status->text().contains("预览"));
  box->setChecked(true);
  QCOMPARE(plot->property("detectionPreviewCount").toInt(), 1);
  QVERIFY(status->text().contains("预览"));
  auto* model = card->findChild<QComboBox*>("detectionModel");
  QVERIFY(model);
  model->setCurrentIndex(model->findData("scn-max-psd-v1"));
  file = window.session().activeFile();
  QVERIFY(findAnnotation(*file, formalIds.front()));
  QVERIFY(findAnnotation(*file, manualId));
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, manualId)), manualBefore);
  model->setCurrentIndex(model->findData("pipeline-fixture-v1"));
  QVERIFY(findAnnotation(*window.session().activeFile(), formalIds.front()));
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  QVERIFY2(run && run->isVisible() && run->isEnabled(),
           run ? qPrintable(run->text()) : "missing run button");
  QVERIFY(run->text().contains("重新检测"));
  QTest::mouseClick(run, Qt::LeftButton);
  file = window.session().activeFile();
  const auto* slot = currentDetectionSlot(
      *file, file->metadata.sampleFormat.selectedChannel);
  QVERIFY2(slot && slot->committedRunId.empty() && !slot->pendingRunId.empty(),
           qPrintable(status->text()));
  for (const auto& id : formalIds) QVERIFY(!findAnnotation(*file, id));
  QVERIFY(findAnnotation(*file, manualId));
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, manualId)), manualBefore);
  QVERIFY(status->text().contains("已清除当前通道上次正式检测结果"));
  QVERIFY(status->text().contains("不会恢复"));
  QCOMPARE(plot->property("detectionPreviewCount").toInt(), 0);
  QCOMPARE(plot->powerGenerationCount(), generations);
  const auto pendingId = slot->pendingRunId;
  panel->tasks().cancel(pendingId);
  timer->start();
  bool cancelledWithoutRestore = false;
  QElapsedTimer wait;
  wait.start();
  while (wait.elapsed() < 20000) {
    QTest::qWait(50);
    file = window.session().activeFile();
    const auto* now = currentDetectionSlot(
        *file, file->metadata.sampleFormat.selectedChannel);
    if (now && now->pendingRunId.empty() && now->committedRunId.empty()) {
      cancelledWithoutRestore = true;
      break;
    }
    if (now && now->committedRunId == pendingId) break;
  }
  QVERIFY2(cancelledWithoutRestore, qPrintable(status->text()));
  file = window.session().activeFile();
  for (const auto& id : formalIds) QVERIFY(!findAnnotation(*file, id));
  std::size_t algorithm = 0;
  for (const auto& mark : file->marks)
    if (mark.sourceKind == AnnotationSource::Algorithm) ++algorithm;
  QCOMPARE(algorithm, std::size_t{0});
  QVERIFY(findAnnotation(*file, manualId));
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, manualId)), manualBefore);
  QVERIFY(status->text().contains("已清除当前通道上次正式检测结果"));
  QVERIFY(status->text().contains("不会恢复"));
  QCOMPARE(plot->property("detectionPreviewCount").toInt(), 0);
}

inline int uxR6VisiblePieces(const FileState& file, bool outside) {
  int count = 0;
  const auto& view = file.view;
  for (const auto& mark : file.marks) {
    if (!annotationVisible(file, mark)) continue;
    const auto take = [&](const ViewRange& range) {
      const bool out = range.time.end <= view.time.begin ||
                       range.time.begin >= view.time.end ||
                       range.frequency.upperHz <= view.frequency.lowerHz ||
                       range.frequency.lowerHz >= view.frequency.upperHz;
      if (out == outside) ++count;
    };
    if (mark.segments.empty()) {
      if (mark.observation) take(*mark.observation);
    } else {
      for (const auto& segment : mark.segments) take(segment.range);
    }
  }
  return count;
}

inline void UxTests::uxR6_commitOverlaySkipsHeatmap() {
  const auto previousFixture = qApp->property("enableDetectionTestFixture");
  qApp->setProperty("enableDetectionTestFixture", true);
  const auto restore = qScopeGuard([&] {
    qApp->setProperty("enableDetectionTestFixture", previousFixture);
  });
  QTemporaryDir directory;
  MainWindow window;
  QString issue;
  QVERIFY(window.createProject(directory.path(), "overlay-commit", &issue));
  QFile iq(directory.filePath("IQ_A_FS1Msps_FC100MHz.dat"));
  QByteArray samples(20000 * 4, '\0');
  for (int sample = 0; sample < 20000; ++sample) {
    const double angle = 2 * 3.14159265358979323846 * .125 * sample;
    qToLittleEndian<qint16>(static_cast<qint16>(12000 * std::cos(angle)),
                            samples.data() + sample * 4);
    qToLittleEndian<qint16>(static_cast<qint16>(12000 * std::sin(angle)),
                            samples.data() + sample * 4 + 2);
  }
  QVERIFY(iq.open(QIODevice::WriteOnly));
  QCOMPARE(iq.write(samples), qint64(samples.size()));
  iq.close();
  QVERIFY2(window.addIqFile(iq.fileName(), &issue), qPrintable(issue));
  QTRY_COMPARE_WITH_TIMEOUT(
      window.session().activeFile()->metadata.availability.status, LoadStatus::Ready,
      10000);
  expose(window);
  QTest::mouseClick(window.findChild<QAbstractButton*>("detectionResultsTab"),
                    Qt::LeftButton);
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  QVERIFY(panel && plot);
  QString stableKey;
  quint64 stablePowers = 0;
  QTRY_VERIFY_WITH_TIMEOUT([&] {
    const auto key = plot->cachedPowerKey();
    const auto powers = plot->powerGenerationCount();
    const bool unchanged = !key.isEmpty() && key == stableKey && powers == stablePowers;
    stableKey = key;
    stablePowers = powers;
    if (!unchanged) return false;
    QTest::qWait(250);
    return plot->cachedPowerKey() == key && plot->powerGenerationCount() == powers;
  }(), 15000);
  QVERIFY(!stableKey.isEmpty());

  const auto requestFor = [&](const char* id) {
    DetectionRequest request;
    request.runId = id;
    request.sourceIdentity = annotationSourceIdentity(
        window.session().activeFile()->metadata,
        window.session().projectGeneration());
    request.profile = *findDetectionProfile("pipeline-fixture-v1");
    request.requestedRange = fullRange(window.session().activeFile()->metadata);
    request.contextRange = request.requestedRange;
    request.rangeOrigin = "overlay-commit";
    return request;
  };
  const auto committed = [&](const std::string& runId) {
    const auto* current = window.session().activeFile();
    const auto* slot = current ? currentDetectionSlot(
        *current, current->metadata.sampleFormat.selectedChannel) : nullptr;
    return slot && !slot->stale && slot->committedRunId == runId;
  };
  QVERIFY2(panel->startRequest(requestFor("overlay-seed"), issue), qPrintable(issue));
  QTRY_VERIFY_WITH_TIMEOUT(committed("overlay-seed"), 20000);
  QTRY_COMPARE_WITH_TIMEOUT(plot->cachedPowerKey(), stableKey, 5000);
  QCOMPARE(plot->powerGenerationCount(), stablePowers);

  struct HeatSample {
    quint64 powers = 0;
    quint64 uploads = 0;
    qint64 heat = 0;
    qint64 overlay = 0;
    qint64 overlayUploads = 0;
    QString key;
  };
  const auto sampleOf = [](PlotWidget* chart) {
    HeatSample sample;
    sample.powers = chart->powerGenerationCount();
    sample.uploads = chart->textureUploadCount();
    sample.key = chart->cachedPowerKey();
    const auto stats = chart->renderStatistics();
    const auto wall = stats.value("cpuWallTimings").toObject();
    sample.heat = wall.value("heatmapUploadPreparation").toObject()
                      .value("samples").toInteger();
    sample.overlay = wall.value("overlayPainter").toObject()
                         .value("samples").toInteger();
    sample.overlayUploads = stats.value("overlayUploads").toInteger();
    return sample;
  };
  struct Measured {
    bool seen = false;
    HeatSample before, after;
    int rects = 0;
    bool previewEmpty = false;
  };
  Measured clearMeasured, publishMeasured;
  connect(panel, &DetectionPanel::commitSettled, &window,
          [&](bool publication, bool installed) {
            if (!installed) return;
            auto& target = publication ? publishMeasured : clearMeasured;
            if (target.seen) return;
            target.before = sampleOf(plot);
            if (!plot->gpuReady()) plot->repaint();
            target.after = sampleOf(plot);
            target.rects = plot->visibleAnnotationRectCount();
            target.previewEmpty = panel->livePreview().events.empty();
            target.seen = true;
          });
  QVERIFY2(panel->startRequest(requestFor("overlay-measured"), issue),
           qPrintable(issue));
  QTRY_VERIFY_WITH_TIMEOUT(clearMeasured.seen, 20000);
  if (plot->gpuReady()) {
    QTRY_COMPARE_WITH_TIMEOUT(plot->powerGenerationCount(),
                              clearMeasured.before.powers, 3000);
  }
  const auto expectUnchanged = [](const Measured& measured, bool publish) {
    QCOMPARE(measured.after.powers, measured.before.powers);
    QCOMPARE(measured.after.key, measured.before.key);
    QCOMPARE(measured.after.uploads, measured.before.uploads);
    if (measured.before.key.isEmpty()) QFAIL("热图功率键尚未建立");
    if (publish) {
      QVERIFY(measured.after.powers == measured.before.powers);
    }
  };
  expectUnchanged(clearMeasured, false);
  if (plot->gpuReady()) {
    QCOMPARE(clearMeasured.after.heat, clearMeasured.before.heat);
  }
  QTRY_VERIFY_WITH_TIMEOUT(committed("overlay-measured"), 20000);
  QTRY_VERIFY_WITH_TIMEOUT(publishMeasured.seen, 5000);
  if (plot->gpuReady()) {
    QTRY_VERIFY_WITH_TIMEOUT(
        plot->renderStatistics().value("overlayUploads").toInteger() ==
                publishMeasured.before.overlayUploads + 1 ||
            plot->renderStatistics()
                    .value("cpuWallTimings").toObject()
                    .value("overlayPainter").toObject()
                    .value("samples").toInteger() ==
                publishMeasured.before.overlay + 1,
        3000);
    const auto rendered = sampleOf(plot);
    QCOMPARE(rendered.powers, publishMeasured.before.powers);
    QCOMPARE(rendered.key, publishMeasured.before.key);
    QCOMPARE(rendered.uploads, publishMeasured.before.uploads);
    QCOMPARE(rendered.heat, publishMeasured.before.heat);
    QVERIFY(rendered.overlayUploads == publishMeasured.before.overlayUploads + 1 ||
            rendered.overlay == publishMeasured.before.overlay + 1);
  } else {
    expectUnchanged(publishMeasured, true);
    QCOMPARE(publishMeasured.after.key, publishMeasured.before.key);
  }
  const auto* file = window.session().activeFile();
  const int inside = uxR6VisiblePieces(*file, false);
  const int outside = uxR6VisiblePieces(*file, true);
  QCOMPARE(plot->visibleAnnotationRectCount(), inside);
  QVERIFY2(outside > 0, "视窗外应有正式段，用于确认它们不会被绘制");
  QVERIFY(publishMeasured.previewEmpty);
  QCOMPARE(panel->livePreview().events.size(), std::size_t{0});

  auto gate = std::make_shared<std::atomic<bool>>(false);
  panel->setPublicationGate(gate);
  QStringList holdMessages;
  connect(panel, &DetectionPanel::message, &window, [&](const QString& text) {
    holdMessages << text;
  });
  connect(panel, &DetectionPanel::commitSettled, &window,
          [&](bool publication, bool installed) {
            holdMessages << QString("settled %1 %2").arg(publication).arg(installed);
          });
  QVERIFY2(panel->startRequest(requestFor("overlay-hold"), issue), qPrintable(issue));
  if (!QTest::qWaitFor([&] { return panel->publicationCommitInProgress(); }, 20000)) {
    const auto* slot = currentDetectionSlot(
        *window.session().activeFile(),
        window.session().activeFile()->metadata.sampleFormat.selectedChannel);
    qWarning().noquote() << holdMessages.join(" | ");
    qWarning().noquote() << "hold slot"
                         << (slot ? QString::fromStdString(slot->pendingRunId) : QString("none"))
                         << (slot ? QString::fromStdString(slot->committedRunId) : QString())
                         << "epoch" << window.session().contentEpoch();
    for (const auto& run : panel->tasks().snapshots())
      qWarning().noquote() << "hold-run" << QString::fromStdString(run.request.runId)
                           << int(run.lifecycle) << QString::fromStdString(run.error);
    QFAIL("publication commit did not stay in progress");
  }
  auto* timer = panel->findChild<QTimer*>("detectionTaskPoll");
  QVERIFY(timer);
  timer->stop();
  DetectionPreviewSnapshot injected;
  injected.runId = "overlay-hold";
  injected.fileId = window.session().activeFile()->metadata.id;
  injected.events.push_back({});
  injected.events.back().trackId = "hold-track";
  injected.events.back().sequence = 7;
  injected.events.back().observedSupport = {100, 400};
  injected.events.back().frequency = {99.9e6, 100.05e6};
  panel->presentLivePreview(injected, true);
  QCOMPARE(panel->livePreview().events.size(), std::size_t{1});
  gate->store(true);
  timer->start();
  QTRY_VERIFY_WITH_TIMEOUT(committed("overlay-hold"), 20000);
  QCOMPARE(panel->livePreview().events.size(), std::size_t{0});
}

inline void UxTests::uxR6_commitFailureKeepsPreviousResult() {
  const auto previousFixture = qApp->property("enableDetectionTestFixture");
  qApp->setProperty("enableDetectionTestFixture", true);
  const auto restore = qScopeGuard([&] {
    qApp->setProperty("enableDetectionTestFixture", previousFixture);
  });
  QTemporaryDir directory;
  MainWindow window;
  QString issue;
  QVERIFY(window.createProject(directory.path(), "commit-failure", &issue));
  QFile iq(directory.filePath("IQ_A_FS1Msps_FC100MHz.dat"));
  QByteArray samples(20000 * 4, '\0');
  for (int sample = 0; sample < 20000; ++sample) {
    const double angle = 2 * 3.14159265358979323846 * .125 * sample;
    qToLittleEndian<qint16>(static_cast<qint16>(8000 * std::cos(angle)),
                            samples.data() + sample * 4);
    qToLittleEndian<qint16>(static_cast<qint16>(8000 * std::sin(angle)),
                            samples.data() + sample * 4 + 2);
  }
  QVERIFY(iq.open(QIODevice::WriteOnly));
  QCOMPARE(iq.write(samples), qint64(samples.size()));
  iq.close();
  QVERIFY2(window.addIqFile(iq.fileName(), &issue), qPrintable(issue));
  QTRY_COMPARE_WITH_TIMEOUT(
      window.session().activeFile()->metadata.availability.status, LoadStatus::Ready,
      10000);
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(panel);
  const auto requestFor = [&](const char* id) {
    DetectionRequest request;
    request.runId = id;
    request.sourceIdentity = annotationSourceIdentity(
        window.session().activeFile()->metadata,
        window.session().projectGeneration());
    request.profile = *findDetectionProfile("pipeline-fixture-v1");
    request.requestedRange = fullRange(window.session().activeFile()->metadata);
    request.contextRange = request.requestedRange;
    request.rangeOrigin = "commit-failure";
    return request;
  };
  const auto slotOf = [&] {
    const auto* current = window.session().activeFile();
    return current ? currentDetectionSlot(
               *current, current->metadata.sampleFormat.selectedChannel)
                   : nullptr;
  };
  QVERIFY2(panel->startRequest(requestFor("failure-seed"), issue), qPrintable(issue));
  if (!QTest::qWaitFor([&] {
        return slotOf() && slotOf()->committedRunId == "failure-seed" &&
               !slotOf()->stale;
      }, 20000)) {
    const auto* slot = slotOf();
    qWarning().noquote() << "slot committed"
                         << (slot ? QString::fromStdString(slot->committedRunId) : QString("none"))
                         << "pending"
                         << (slot ? QString::fromStdString(slot->pendingRunId) : QString("none"))
                         << "epoch" << window.session().contentEpoch();
    for (const auto& run : panel->tasks().snapshots())
      qWarning().noquote() << "run" << QString::fromStdString(run.request.runId)
                           << "life" << int(run.lifecycle)
                           << QString::fromStdString(run.error);
    QFAIL("failure-seed did not commit");
  }
  std::vector<QByteArray> formal;
  for (const auto& mark : window.session().activeFile()->marks)
    if (mark.sourceKind == AnnotationSource::Algorithm &&
        mark.originEvidence && mark.originEvidence->runId == "failure-seed")
      formal.push_back(QJsonDocument(encodeAnnotation(mark)).toJson(QJsonDocument::Compact));
  QVERIFY(!formal.empty());
  const auto committedBefore = slotOf()->committedRunId;

  bool saveFailed = false;
  connect(panel, &DetectionPanel::commitSettled, &window,
          [&](bool publication, bool installed) {
            if (!publication && !installed) saveFailed = true;
          });
  panel->setStartSaveFault(true);
  QVERIFY2(panel->startRequest(requestFor("failure-save"), issue), qPrintable(issue));
  QTRY_VERIFY_WITH_TIMEOUT(saveFailed, 20000);
  panel->setStartSaveFault(false);
  QCOMPARE(slotOf()->committedRunId, committedBefore);
  std::vector<QByteArray> formalAfterSave;
  for (const auto& mark : window.session().activeFile()->marks)
    if (mark.sourceKind == AnnotationSource::Algorithm &&
        mark.originEvidence && mark.originEvidence->runId == "failure-seed")
      formalAfterSave.push_back(
          QJsonDocument(encodeAnnotation(mark)).toJson(QJsonDocument::Compact));
  QCOMPARE(formalAfterSave, formal);
  const auto snaps = panel->tasks().snapshots();
  QVERIFY(std::none_of(snaps.begin(), snaps.end(), [](const DetectionRun& run) {
    return run.request.runId == "failure-save";
  }));

  std::string committedAtPublication;
  bool publishFailed = false;
  connect(panel, &DetectionPanel::commitSettled, &window,
          [&](bool publication, bool installed) {
            if (!publication && installed && committedAtPublication.empty())
              committedAtPublication = slotOf() ? slotOf()->committedRunId : std::string{};
            if (publication && !installed) publishFailed = true;
          });
  DetectionTransactionOptions fault;
  fault.failAt = [](DetectionTransactionPoint point) {
    return point == DetectionTransactionPoint::Commit;
  };
  panel->setPublicationFault(fault);
  QVERIFY2(panel->startRequest(requestFor("failure-publish"), issue),
           qPrintable(issue));
  QTRY_VERIFY_WITH_TIMEOUT(publishFailed, 20000);
  QCOMPARE(slotOf()->committedRunId, committedAtPublication);
  QVERIFY(slotOf()->committedRunId != "failure-publish");
  for (const auto& mark : window.session().activeFile()->marks)
    if (mark.originEvidence)
      QVERIFY(mark.originEvidence->runId != "failure-publish");
}
