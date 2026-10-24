#pragma once

#include <QDialog>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QPersistentModelIndex>
#include <QScopeGuard>
#include <QTextBrowser>
#include <QTimer>
#include "infrastructure/detection_store.h"

inline void UxTests::uxR01_lateRunFilterPopupAndRefresh() {
  QTemporaryDir directory;
  MainWindow window;
  QString issue;
  QVERIFY(window.createProject(directory.path(), "run-filter", &issue));
  issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
  QVERIFY2(issue.isEmpty(), qPrintable(issue));
  expose(window);
  QTest::mouseClick(window.findChild<QAbstractButton*>("resultTab"), Qt::LeftButton);
  auto* filter = window.findChild<QComboBox*>("annotationRunFilter");
  QVERIFY(filter && filter->isVisible());
  QCOMPARE(filter->count(), 2);
  const auto open = [&] {
    QTest::mouseClick(filter, Qt::LeftButton, Qt::NoModifier, QPoint(filter->width() - 8, filter->height() / 2));
  };
  open();
  QTRY_VERIFY(filter->view()->isVisible());
  QTest::keyClick(filter->view(), Qt::Key_Escape);
  QTRY_VERIFY(!filter->view()->isVisible());
  auto* source = window.session().activeFile();
  DetectionRequest request;
  // Wider than the first-show editor, while fitting the offscreen plugin's
  // small virtual popup screen. Native capture also checks full UUID IDs.
  request.runId = "ux-filter-run-0123456789";
  request.sourceIdentity = annotationSourceIdentity(source->metadata, window.session().projectGeneration());
  request.profile = *findDetectionProfile("pipeline-fixture-v1");
  request.requestedRange = fullRange(source->metadata);
  request.contextRange = request.requestedRange;
  request.rangeOrigin = "available-file";
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY2(panel->startRequest(request, issue), qPrintable(issue));
  QTRY_VERIFY_WITH_TIMEOUT(filter->findData(QString::fromStdString(request.runId)) >= 2, 15000);
  open();
  auto* view = filter->view();
  QTRY_VERIFY(view->isVisible());
  const QPersistentModelIndex row(filter->model()->index(filter->findData(QString::fromStdString(request.runId)), 0));
  view->scrollTo(row);
  QTRY_VERIFY2(view->viewport()->rect().contains(view->visualRect(row)),
      qPrintable(QString("row %1x%2; viewport %3x%4; view min %5; popup %6x%7")
          .arg(view->visualRect(row).width()).arg(view->visualRect(row).height())
          .arg(view->viewport()->width()).arg(view->viewport()->height())
          .arg(view->minimumWidth()).arg(view->window()->width()).arg(view->window()->height())));
  const auto tooltip = row.data(Qt::ToolTipRole).toString();
  QCOMPARE(tooltip, QString::fromStdString(request.runId));
  QTest::keyClick(view, Qt::Key_End);
  QCOMPARE(view->currentIndex(), QModelIndex(row));
  for (int refresh = 0; refresh < 5; ++refresh) window.refresh();
  QVERIFY(row.isValid() && view->isVisible());
  QCOMPARE(view->currentIndex(), QModelIndex(row));
  QCOMPARE(row.data(Qt::UserRole).toString(), QString::fromStdString(request.runId));
  QVERIFY(view->viewport()->rect().contains(view->visualRect(row)));
  QTest::qWait(QApplication::doubleClickInterval() + 50);
  QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->visualRect(row).center());
  QTRY_VERIFY(!view->isVisible());
  QCOMPARE(source->annotationFilter.runId, request.runId);
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
  QTRY_VERIFY_WITH_TIMEOUT(find() && find()->completedWindows > 0 && !detectionIsTerminal(find()->lifecycle), 15000);
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
  QVERIFY(!window.saveProject(copiedProject));
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
}
