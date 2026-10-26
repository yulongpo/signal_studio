#pragma once

#include <QDoubleSpinBox>

namespace {
std::array<std::string, 3> uxR5Seed(MainWindow& window) {
  window.session().addDemoFile();
  auto& file = *window.session().activeFile();
  const auto manual = window.session().addMark({{1000, 4000}, {99.9e6, 100.1e6}});
  const auto prototype = *findAnnotation(file, manual);
  std::array<std::string, 3> ids{"r5-label", manual, "r5-detection"};
  for (const int kind : {0, 2}) {
    AnnotationSourceSet set;
    set.id = "r5-source-" + std::to_string(kind);
    set.fileId = file.metadata.id;
    set.sampleChannel = file.metadata.sampleFormat.selectedChannel;
    set.sourceKind = static_cast<AnnotationSource>(kind);
    set.sourceIdentity = annotationSourceIdentity(file.metadata);
    set.formatId = kind == 0 ? "generator-txt-v1" : "detection-v1";
    set.originalText = "original immutable source text";
    auto annotation = prototype;
    annotation.id = ids[kind];
    annotation.name = kind == 0 ? "标签甲" : "检测乙";
    annotation.sourceKind = set.sourceKind;
    annotation.sourceSetId = set.id;
    auto origin = std::make_shared<OriginEvidence>(*annotation.originEvidence);
    origin->formatId = set.formatId;
    annotation.originEvidence = origin;
    annotation.revisionId.clear();
    annotation.revisions.clear();
    annotation.reviewHistory.clear();
    if (!window.session().annotations().publishSourceSet(file, set, {annotation})) return {};
  }
  window.session().selectMarks({});
  file.view = {{0, 10000}, {99.5e6, 100.5e6}};
  return ids;
}
}

inline void UxTests::uxR5_threeTabsReadOnlyAndConversion() {
  MainWindow window;
  const auto ids = uxR5Seed(window);
  QVERIFY(!ids[0].empty());
  expose(window);
  auto* panel = window.findChild<AnnotationPanel*>("annotationPanel");
  QVERIFY(panel);
  QVERIFY(!window.findChild<QWidget*>("annotationSourceFilter"));
  QVERIFY(!window.findChild<QAbstractButton*>("resultTab"));
  QVERIFY(!window.findChild<QTableView*>("annotationTable"));
  for (const auto* name : {"importedLabelsTab", "manualAnnotationsTab", "detectionResultsTab", "taskTab", "logTab"})
    QVERIFY(window.findChild<QAbstractButton*>(name));
  auto* file = window.session().activeFile();
  const auto rawBefore = encodeAnnotation(*findAnnotation(*file, ids[0]));
  auto* table = window.findChild<QTableView*>("importedLabelTable");
  QTest::mouseClick(window.findChild<QAbstractButton*>("importedLabelsTab"), Qt::LeftButton);
  QCOMPARE(table->model()->rowCount(), 1);
  QCOMPARE(table->model()->index(0, 1).data(Qt::UserRole).toString(), QString::fromStdString(ids[0]));
  const auto viewBefore = file->view;
  QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(table->model()->index(0, 1)).center());
  QCOMPARE(file->activeMarkId, ids[0]);
  QCOMPARE(file->view, viewBefore);
  QVERIFY(!(table->model()->flags(table->model()->index(0, 1)) & Qt::ItemIsEditable));
  QTest::keyClick(table, Qt::Key_F2);
  QTest::keyClick(table, Qt::Key_Delete);
  QVERIFY(!window.findChild<QLineEdit*>("annotationTableEditor"));
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, ids[0])), rawBefore);
  window.findChild<QPushButton*>("contextAnnotation")->click();
  auto* readOnly = window.findChild<QLabel*>("annotationReadOnlyRange");
  QVERIFY(readOnly && readOnly->isVisible());
  QVERIFY(readOnly->text().contains("转换为人工标记"));
  QVERIFY(!window.findChild<QWidget*>("annotationGeometryEditor")->isVisible());
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  plot->repaint();
  QCOMPARE(plot->property("annotationHandleCount").toInt(), 0);
  const QPoint from = plot->toPixel(2500, 100e6).toPoint();
  QTest::mousePress(plot, Qt::LeftButton, Qt::NoModifier, from);
  QMouseEvent move(QEvent::MouseMove, from+QPoint(25, 15), plot->mapToGlobal(from+QPoint(25,15)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(plot, &move);
  QTest::mouseRelease(plot, Qt::LeftButton, Qt::NoModifier, from+QPoint(25,15));
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, ids[0])), rawBefore);
  window.session().selectMarks({ids[0]}, ids[0]);
  window.refresh();
  window.findChild<QPushButton*>("convertAnnotationsToManual")->click();
  QCOMPARE(panel->sourceTab(), AnnotationSource::Manual);
  const auto created = file->activeMarkId;
  QVERIFY(created != ids[0] && created != ids[1]);
  QVERIFY(window.session().annotations().canEdit(*file, created));
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, ids[0])), rawBefore);
  QVERIFY(findAnnotation(*file, created)->conversionSnapshot);
  window.findChild<QPushButton*>("undoAnnotations")->click();
  const auto* undone = findAnnotation(*file, created);
  QVERIFY(!undone || !annotationInContext(*file, *undone));
  window.findChild<QPushButton*>("redoAnnotations")->click();
  QVERIFY(annotationInContext(*file, *findAnnotation(*file, created)));
}

inline void UxTests::uxR5_layersAndIndependentFilters() {
  MainWindow window;
  const auto ids = uxR5Seed(window);
  expose(window);
  auto* panel = window.findChild<AnnotationPanel*>("annotationPanel");
  auto* file = window.session().activeFile();
  for (const auto* name : {"annotationLayer0", "annotationLayer1", "annotationLayer2", "annotationLayerBar"})
    QVERIFY(!window.findChild<QWidget*>(name));
  const std::array<const char*, 3> buttons{"importedLabelsTab", "manualAnnotationsTab", "detectionResultsTab"};
  const auto view = file->view;
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  QTest::mouseClick(window.findChild<QAbstractButton*>(buttons[0]), Qt::LeftButton);
  QTest::qWait(30);  // Opening the bottom panel changes plot dimensions once.
  QTRY_VERIFY_WITH_TIMEOUT(plot->isDisplaySettled(), 5000);
  const auto calculations = plot->powerGenerationCount();
  for (int source : {0, 1, 2, 0}) {
    QTest::mouseClick(window.findChild<QAbstractButton*>(buttons[source]), Qt::LeftButton);
    QCOMPARE(static_cast<int>(panel->sourceTab()), source);
    for (int kind = 0; kind < 3; ++kind) {
      QCOMPARE(file->annotationLayers[kind], kind == source);
      QCOMPARE(annotationOverlayVisible(*file, *findAnnotation(*file, ids[kind])), kind == source);
    }
    QCOMPARE(file->view, view);
  }
  QCOMPARE(plot->powerGenerationCount(), calculations);
  QTest::mouseClick(window.findChild<QAbstractButton*>("importedLabelsTab"), Qt::LeftButton);
  auto* table = window.findChild<QTableView*>("importedLabelTable");
  QCOMPARE(table->model()->rowCount(), 1);
  QVERIFY(annotationOverlayVisible(*file, *findAnnotation(*file, ids[0])));
  QVERIFY(annotationEligibleForTab(*file, *findAnnotation(*file, ids[0]), AnnotationSource::ImportedLabel));
  table->model()->setData(table->model()->index(0, 0), Qt::Checked, Qt::CheckStateRole);
  QCOMPARE(file->selectedMarkIds, std::vector<std::string>{ids[0]});
  auto* filter = window.findChild<QLineEdit*>("annotationQuickFilter");
  filter->setText("标签甲");
  QCOMPARE(table->model()->rowCount(), 1);
  QTest::mouseClick(window.findChild<QAbstractButton*>("manualAnnotationsTab"), Qt::LeftButton);
  QVERIFY(file->selectedMarkIds.empty());
  QVERIFY(filter->text().isEmpty());
  filter->setText("无匹配");
  QCOMPARE(window.findChild<QTableView*>("manualAnnotationTable")->model()->rowCount(), 0);
  QTest::mouseClick(window.findChild<QAbstractButton*>("importedLabelsTab"), Qt::LeftButton);
  QCOMPARE(panel->sourceTab(), AnnotationSource::ImportedLabel);
  QCOMPARE(filter->text(), QString("标签甲"));
  QCOMPARE(table->model()->rowCount(), 1);

  // Controlled authoritative-slot projection. Durable save/reopen and atomic
  // success/zero replacement are covered by the lifecycle and task tests.
  const auto labelBefore = encodeAnnotation(*findAnnotation(*file, ids[0]));
  const auto manualBefore = encodeAnnotation(*findAnnotation(*file, ids[1]));
  auto* result = findAnnotation(*file, ids[2]);
  auto* source = findSourceSet(*file, result->sourceSetId);
  source->runId = "r6-visible-committed";
  auto origin = std::make_shared<OriginEvidence>(*result->originEvidence);
  origin->runId = source->runId;
  origin->scoreMean = .9;
  result->originEvidence = std::move(origin);
  const auto resultBefore = encodeAnnotation(*result);
  DetectionRun committed;
  committed.request.runId = source->runId;
  committed.request.sourceIdentity = source->sourceIdentity;
  committed.request.profile = *findDetectionProfile("pipeline-fixture-v1");
  committed.request.requestedRange = fullRange(file->metadata);
  committed.request.contextRange = committed.request.requestedRange;
  committed.lifecycle = DetectionLifecycle::Completed;
  file->detectionRuns.push_back(committed);
  CurrentDetectionSlot slot;
  slot.sampleChannel = file->metadata.sampleFormat.selectedChannel;
  slot.committedRunId = committed.request.runId;
  slot.generation = 1;
  slot.stale = true;
  file->currentDetectionSlots = std::vector<CurrentDetectionSlot>{slot};
  QTest::mouseClick(window.findChild<QAbstractButton*>("detectionResultsTab"), Qt::LeftButton);
  window.refresh();
  auto* results = window.findChild<QTableView*>("detectionResultTable");
  QCOMPARE(results->model()->rowCount(), 1);
  QCOMPARE(results->model()->index(0, 1).data(Qt::UserRole).toString(), QString::fromStdString(ids[2]));
  QVERIFY(window.findChild<QLabel*>("currentDetectionStatus")->text().contains("当前显示上次检测结果"));
  window.session().selectMarks({ids[2]}, ids[2]); window.refresh();
  QVERIFY(annotationSelectable(*file, *result) && annotationOverlayVisible(*file, *result));
  QVERIFY(!window.session().annotations().canEdit(*file, ids[2]));
  QVERIFY(!window.session().annotations().capabilities(*file, ids[2]).createChannelDirectly);
  filter->setText("无匹配");
  QCOMPARE(results->model()->rowCount(), 0);
  QVERIFY(annotationSelectable(*file, *result) && annotationOverlayVisible(*file, *result));
  filter->clear();
  auto* mean = window.findChild<QDoubleSpinBox*>("annotationMinimumMeanScore");
  mean->setValue(1);
  QCOMPARE(results->model()->rowCount(), 0);
  QVERIFY(annotationSelectable(*file, *result) && annotationOverlayVisible(*file, *result));
  mean->setValue(0);
  QCOMPARE(results->model()->rowCount(), 1);
  window.session().selectMarks({ids[2]}, ids[2]); window.refresh();
  DetectionRun pending = committed;
  pending.request.runId = "r6-visible-pending";
  pending.lifecycle = DetectionLifecycle::Running;
  file->detectionRuns.push_back(pending);
  file->currentDetectionSlots->front().pendingRunId = pending.request.runId;
  window.refresh();
  QCOMPARE(results->model()->rowCount(), 1);
  const auto running = window.findChild<QLabel*>("currentDetectionStatus")->text();
  QVERIFY(running.contains("检测运行中") && running.contains("当前显示上次检测结果"));
  QCOMPARE(file->selectedMarkIds, std::vector<std::string>{ids[2]});
  for (int kind : {0, 1, 2}) {
    QTest::mouseClick(window.findChild<QAbstractButton*>(buttons[kind]), Qt::LeftButton);
    for (int sourceKind = 0; sourceKind < 3; ++sourceKind)
      QCOMPARE(annotationOverlayVisible(*file, *findAnnotation(*file, ids[sourceKind])), sourceKind == kind);
  }
  QCOMPARE(results->model()->rowCount(), 1);
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, ids[0])), labelBefore);
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, ids[1])), manualBefore);
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, ids[2])), resultBefore);
}

inline void UxTests::uxR5_readOnlyMenusAndCommandGuards() {
  MainWindow window;
  const auto ids = uxR5Seed(window);
  expose(window);
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  auto* tree = window.findChild<QTreeWidget*>("projectTree");
  auto* globalRename = window.findChild<QAction*>("renameMarkAction");
  auto* globalDelete = window.findChild<QAction*>("deleteMarksAction");
  QVERIFY(plot && tree && globalRename && globalDelete);
  const auto actionEnabled = [](QMenu* menu, const char* name) {
    auto* action = menu->findChild<QAction*>(name);
    return action && action->isEnabled();
  };
  for (int source : {0, 2, 1}) {
    window.session().selectMarks({ids[source]}, ids[source]);
    window.refresh();
    const bool editable = source == 1;
    auto* file = window.session().activeFile();
    const auto before = encodeAnnotation(*findAnnotation(*file, ids[source]));
    QCOMPARE(globalRename->isEnabled(), editable);
    QCOMPARE(globalDelete->isEnabled(), editable);
    QCOMPARE(window.findChild<QPushButton*>("renameMark")->isEnabled(), editable);
    QCOMPARE(window.findChild<QPushButton*>("deleteMarks")->isEnabled(), editable);
    QCOMPARE(window.findChild<QPushButton*>("treeDeleteMarks")->isEnabled(), editable);
    auto* plotMenu = plot->createContextMenu(plot->plotRect().center().toPoint());
    QCOMPARE(actionEnabled(plotMenu, "contextRenameAction"), editable);
    QCOMPARE(actionEnabled(plotMenu, "contextDeleteAction"), editable);
    delete plotMenu;

    const auto treeItem = [&]() -> QTreeWidgetItem* {
      for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toString() == "mark" &&
            (*it)->data(0, Qt::UserRole+1).toString() == QString::fromStdString(ids[source])) return *it;
      return nullptr;
    };
    QTRY_VERIFY_WITH_TIMEOUT(treeItem(), 3000);
    auto* item = treeItem();
    for (auto* parent = item->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
    bool treeMenuSeen = false, treeRenameEnabled = false, treeDeleteEnabled = false;
    QTimer inspectTreeMenu;
    QObject::connect(&inspectTreeMenu, &QTimer::timeout, &window, [&] {
      auto* menu = window.findChild<QMenu*>("annotationTreeContextMenu");
      if (!menu || !menu->isVisible()) return;
      treeMenuSeen = true;
      treeRenameEnabled = actionEnabled(menu, "annotationTreeRenameAction");
      treeDeleteEnabled = actionEnabled(menu, "annotationTreeDeleteAction");
      menu->close();
    });
    inspectTreeMenu.start(5);
    tree->customContextMenuRequested(tree->visualItemRect(item).center());
    inspectTreeMenu.stop();
    QVERIFY(treeMenuSeen);
    QCOMPARE(treeRenameEnabled, editable);
    QCOMPARE(treeDeleteEnabled, editable);

    // Bypass disabled QAction presentation and exercise the connected command.
    // A readonly source must return before constructing any editable dialog.
    bool renameDialogSeen = false;
    QTimer inspectRename;
    QObject::connect(&inspectRename, &QTimer::timeout, &window, [&] {
      if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
        renameDialogSeen = true;
        dialog->reject();
      }
    });
    inspectRename.start(5);
    plot->markRenameRequested();
    inspectRename.stop();
    QCOMPARE(renameDialogSeen, editable);
    QCOMPARE(encodeAnnotation(*findAnnotation(*file, ids[source])), before);
    if (!editable) {
      plot->markDeleteRequested();
      QCOMPARE(encodeAnnotation(*findAnnotation(*file, ids[source])), before);
    }
  }
  window.session().selectMarks({ids[1], ids[0]}, ids[1]);
  window.refresh();
  QVERIFY(!globalDelete->isEnabled());
  QVERIFY(!window.findChild<QPushButton*>("deleteMarks")->isEnabled());
  auto* mixedMenu = plot->createContextMenu(plot->plotRect().center().toPoint());
  QVERIFY(!actionEnabled(mixedMenu, "contextDeleteAction"));
  delete mixedMenu;
}

inline void UxTests::uxR5_crossViewAndUnknownObservation() {
  MainWindow window;
  const auto ids = uxR5Seed(window);
  expose(window);
  auto* file = window.session().activeFile();
  auto* panel = window.findChild<AnnotationPanel*>("annotationPanel");
  auto* algorithm = findAnnotation(*file, ids[2]);
  window.session().selectMarks({ids[1]}, ids[1]);
  window.refresh();
  auto* tree = window.findChild<QTreeWidget*>("projectTree");
  QVERIFY(tree);
  QTreeWidgetItem* importedItem = nullptr;
  const auto findImported = [&] {
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
      if ((*it)->data(0, Qt::UserRole).toString() == "mark" &&
          (*it)->data(0, Qt::UserRole+1).toString() == QString::fromStdString(ids[0])) return *it;
    return static_cast<QTreeWidgetItem*>(nullptr);
  };
  // Tree projection is intentionally queued to avoid rebuilding during input.
  QTRY_VERIFY_WITH_TIMEOUT((importedItem = findImported()) != nullptr, 3000);
  for (auto* parent = importedItem->parent(); parent; parent=parent->parent()) parent->setExpanded(true);
  tree->scrollToItem(importedItem);
  const auto beforeTreeSelection = file->view;
  QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualItemRect(importedItem).center());
  QCOMPARE(file->activeMarkId, ids[0]);
  QCOMPARE(panel->sourceTab(), AnnotationSource::ImportedLabel);
  QCOMPARE(file->view, beforeTreeSelection);
  algorithm->observation = algorithm->segments.front().range;
  algorithm->segments.clear();
  algorithm->activeSegmentId.clear();
  auto origin = std::make_shared<OriginEvidence>(*algorithm->originEvidence);
  origin->originalSegments.clear();
  origin->originalObservation = algorithm->observation;
  algorithm->originEvidence = origin;
  refreshAnnotationProjection(*algorithm);
  const auto range = file->view;
  window.session().selectMarks({ids[2]}, ids[2]);
  window.refresh();
  QCOMPARE(panel->sourceTab(), AnnotationSource::Algorithm);
  QCOMPARE(file->view, range);
  const auto observedTime = algorithm->observation->time;
  window.findChild<QPushButton*>("convertAnnotationsToManual")->click();
  QCOMPARE(panel->sourceTab(), AnnotationSource::Manual);
  const auto* converted = findAnnotation(*file, file->activeMarkId);
  QVERIFY(converted && converted->segments.empty() && converted->observation);
  QCOMPARE(converted->observation->time, observedTime);
  QCOMPARE(file->view, range);
}
