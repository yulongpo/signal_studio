#pragma once

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
  QTest::mouseClick(window.findChild<QAbstractButton*>("importedLabelsTab"), Qt::LeftButton);
  auto* layer = window.findChild<QCheckBox*>("annotationLayer0");
  QVERIFY(layer);
  layer->setChecked(false);
  auto* table = window.findChild<QTableView*>("importedLabelTable");
  QCOMPARE(table->model()->rowCount(), 1);
  QVERIFY(!annotationOverlayVisible(*file, *findAnnotation(*file, ids[0])));
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
}

inline void UxTests::uxR5_crossViewAndUnknownObservation() {
  MainWindow window;
  const auto ids = uxR5Seed(window);
  expose(window);
  auto* file = window.session().activeFile();
  auto* panel = window.findChild<AnnotationPanel*>("annotationPanel");
  auto* algorithm = findAnnotation(*file, ids[2]);
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
