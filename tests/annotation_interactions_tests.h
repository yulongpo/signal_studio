#pragma once

#include <QLayout>
#include <QTreeWidgetItemIterator>
#include <array>

// Native Qt interaction coverage. Annotation fixtures are synthetic and do not
// make claims about model quality, GPU inference, or real RF accuracy.
namespace {
std::string annotationUiId(int index) {
  return QString("ui-%1").arg(index, 3, 10, QChar('0')).toStdString();
}

AnnotationSourceSet annotationUiSource(const FileState& file, std::string id,
                                       AnnotationSource kind) {
  AnnotationSourceSet source;
  source.id = std::move(id);
  source.fileId = file.metadata.id;
  source.sampleChannel = file.metadata.sampleFormat.selectedChannel;
  source.sourceKind = kind;
  source.formatId = kind == AnnotationSource::ImportedLabel
                        ? "torchsig-rfbench-sigmf-v1"
                    : kind == AnnotationSource::Manual ? "manual-v1"
                                                       : "detection-v1";
  source.parserId = "native-qt-test";
  source.parserVersion = "1";
  source.sourceHash = "fixture:" + source.id;
  source.originalText = R"({"syntheticFixture":true,"modelInference":false})";
  source.sourceIdentity = annotationSourceIdentity(file.metadata);
  return source;
}

Annotation annotationUiValue(const FileState& file,
                             const AnnotationSourceSet& source, std::string id,
                             std::string name, std::vector<ViewRange> ranges,
                             std::optional<ViewRange> observation = {}) {
  Annotation value;
  value.id = std::move(id);
  value.name = std::move(name);
  value.fileId = file.metadata.id;
  value.sourceSetId = source.id;
  value.sourceKind = source.sourceKind;
  value.originRecordId = value.id;
  value.observation = observation;
  value.observationTimeQuality = TimeQuality::WindowObservation;
  for (const auto& range : ranges) {
    Segment segment;
    segment.id = value.id + ":s" + std::to_string(value.segments.size());
    segment.range = range;
    segment.timeQuality = TimeQuality::Manual;
    segment.frequencyQuality = FrequencyQuality::Manual;
    value.segments.push_back(std::move(segment));
  }
  auto evidence = std::make_shared<OriginEvidence>();
  evidence->formatId = source.formatId;
  evidence->originRef = value.id;
  evidence->originalFieldsJson =
      R"({"syntheticFixture":true,"modelInference":false})";
  evidence->originalSegments = value.segments;
  evidence->originalObservation = observation;
  if (source.sourceKind == AnnotationSource::Algorithm)
    evidence->scoreMean = .8;
  value.originEvidence = std::move(evidence);
  return value;
}

AnnotationResult annotationUiPublish(MainWindow& window, int count) {
  auto& session = window.session();
  session.addDemoFile("annotation-ui.iq", 1e6, 100e6, .1);
  auto& file = *session.activeFile();
  file.metadata.sampleFormat.channels = 2;
  for (int kind = 0; kind < 3; ++kind) {
    const auto source =
        annotationUiSource(file, "ui-source:" + std::to_string(kind),
                           static_cast<AnnotationSource>(kind));
    std::vector<Annotation> values;
    for (int index = 0; index < count; ++index) {
      const int group = index < 4 ? 0 : index == 4 ? 1 : 2;
      if (group != kind) continue;
      const ViewRange range =
          index == count - 1 ? ViewRange{{70000, 90000}, {100.2e6, 100.3e6}}
                             : ViewRange{{10000, 20000}, {99.8e6, 99.9e6}};
      values.push_back(annotationUiValue(
          file, source, annotationUiId(index),
          QString("Row %1").arg(index, 3, 10, QChar('0')).toStdString(),
          {range}));
    }
    if (!values.empty()) {
      const auto result = session.annotations().publishSourceSet(
          file, source, std::move(values));
      if (!result.ok) return result;
    }
  }
  session.selectMarks({});
  session.setView(fullRange(file.metadata));
  return {true};
}

QTreeWidgetItem* annotationUiTreeItem(QTreeWidget* tree,
                                      const std::string& id) {
  if (!tree || !tree->topLevelItemCount()) return nullptr;
  for (auto* node : descendantsOfType(tree->topLevelItem(0), "mark"))
    if (node->data(0, Qt::UserRole + 1).toString().toStdString() == id)
      return node;
  return nullptr;
}

QModelIndex annotationUiTableIndex(QTableView* table, const std::string& id,
                                   int column = 1) {
  for (int row = 0; row < table->model()->rowCount(); ++row) {
    const auto index = table->model()->index(row, column);
    if (index.data(Qt::UserRole).toString().toStdString() == id) return index;
  }
  return {};
}

void annotationUiClickRow(QTableView* table, const std::string& id,
                          Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  const auto index = annotationUiTableIndex(table, id);
  table->scrollTo(index);
  QTest::mouseClick(table->viewport(), Qt::LeftButton, modifiers,
                    table->visualRect(index).center());
}
QString annotationUiTreeKey(const QTreeWidgetItem* item) {
  QString result;
  for (auto* part = item; part; part = part->parent())
    result.prepend("/" + part->data(0, Qt::UserRole).toString() + ":" +
                   part->data(0, Qt::UserRole + 1).toString());
  return result;
}

QTreeWidgetItem* annotationUiTreeItemByKey(QTreeWidget* tree,
                                           const QString& key) {
  if (key.isEmpty()) return nullptr;
  for (QTreeWidgetItemIterator it(tree); *it; ++it)
    if (annotationUiTreeKey(*it) == key) return *it;
  return nullptr;
}

bool annotationUiRevealTreeItem(MainWindow& window, QTreeWidget* tree,
                                const QString& key, QPoint* global = nullptr) {
  auto* scroll = window.findChild<QScrollArea*>("resourceScroll");
  if (!scroll || key.isEmpty()) return false;
  // The fixed-height tree and its containing scroll area each defer geometry.
  // A pending refresh may rebuild every item while these events are processed.
  // Carry only the stable role/id path across event boundaries.
  tree->doItemsLayout();
  QCoreApplication::processEvents();
  if (scroll->widget()->layout()) scroll->widget()->layout()->activate();
  QCoreApplication::processEvents();
  {
    const auto* item = annotationUiTreeItemByKey(tree, key);
    if (!item) return false;
    const auto point = tree->viewport()->mapTo(
        scroll->widget(), tree->visualItemRect(item).center());
    scroll->ensureVisible(point.x(), point.y(), 0, 40);
  }
  QCoreApplication::processEvents();
  const auto* item = annotationUiTreeItemByKey(tree, key);
  if (!item) return false;
  const auto local = tree->visualItemRect(item).center();
  const auto globalPoint = tree->viewport()->mapToGlobal(local);
  if (global) *global = globalPoint;
  return tree->itemAt(local) == item &&
         tree->viewport()->rect().contains(local) &&
         scroll->viewport()->rect().contains(
             scroll->viewport()->mapFromGlobal(globalPoint)) &&
         window.rect().contains(window.mapFromGlobal(globalPoint));
}
bool annotationUiClickTreeItem(
    MainWindow& window, QTreeWidget* tree, const QString& key,
    Qt::MouseButton button = Qt::LeftButton,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QPoint global;
  if (!annotationUiRevealTreeItem(window, tree, key, &global)) return false;
  QTest::mouseClick(window.windowHandle(), button, modifiers,
                    window.windowHandle()->mapFromGlobal(global));
  return true;
}

bool annotationUiMoveMouse(MainWindow& window, QWidget* widget,
                           const QPoint& point) {
  const auto global = widget->mapToGlobal(point);
  if (!widget->rect().contains(point) ||
      !window.rect().contains(window.mapFromGlobal(global)))
    return false;
  QTest::mouseMove(window.windowHandle(),
                   window.windowHandle()->mapFromGlobal(global));
  return true;
}
}  // namespace

void UiTests::annotationSelectionAcrossPagesAndViews() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 268);
  QVERIFY2(published.ok, published.error.c_str());
  showWindow(window);
  window.refresh();
  window.findChild<QPushButton*>("resultTab")->click();
  auto* file = window.session().activeFile();
  const auto originalView = file->view;
  auto* table = window.findChild<QTableView*>("annotationTable");
  auto* tree = window.findChild<QTreeWidget*>("projectTree");
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  QVERIFY(table && tree && plot);
  QCOMPARE(table->model()->columnCount(), 11);
  QCOMPARE(table->model()->rowCount(), 200);
  annotationUiClickRow(table, annotationUiId(0));
  QCOMPARE(file->selectedMarkIds, std::vector<std::string>{annotationUiId(0)});
  window.findChild<QPushButton*>("annotationsNextPage")->click();
  QCOMPARE(table->model()->rowCount(), 68);
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{1});
  annotationUiClickRow(table, annotationUiId(220), Qt::ShiftModifier);
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{221});
  QCOMPARE(file->selectionAnchorId, annotationUiId(0));
  QVERIFY(std::find(file->selectedMarkIds.begin(), file->selectedMarkIds.end(),
                    annotationUiId(199)) != file->selectedMarkIds.end());
  QVERIFY(std::find(file->selectedMarkIds.begin(), file->selectedMarkIds.end(),
                    annotationUiId(200)) != file->selectedMarkIds.end());
  window.findChild<QPushButton*>("annotationsPreviousPage")->click();
  annotationUiClickRow(table, annotationUiId(0));
  window.findChild<QPushButton*>("annotationsNextPage")->click();
  annotationUiClickRow(table, annotationUiId(220), Qt::ControlModifier);
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{2});
  QCOMPARE(file->activeMarkId, annotationUiId(220));
  QTRY_VERIFY(annotationUiTreeItem(tree, annotationUiId(220)));
  QVERIFY(annotationUiTreeItem(tree, annotationUiId(220))->isSelected());
  auto groups = descendantsOfType(tree->topLevelItem(0), "annotationSource");
  QCOMPARE(groups.size(), 3);
  groups[0]->setExpanded(false);
  window.findChild<QPushButton*>("annotationsPreviousPage")->click();
  QVERIFY(!annotationUiTableIndex(table, annotationUiId(267)).isValid());
  QTest::mouseClick(plot, Qt::LeftButton, Qt::NoModifier,
                    plot->toPixel(80000, 100.25e6).toPoint());
  QCOMPARE(file->selectedMarkIds,
           std::vector<std::string>{annotationUiId(267)});
  QVERIFY(annotationUiTableIndex(table, annotationUiId(267)).isValid());
  QTRY_VERIFY(annotationUiTreeItem(tree, annotationUiId(267)));
  QVERIFY(annotationUiTreeItem(tree, annotationUiId(267))->isSelected());
  groups = descendantsOfType(tree->topLevelItem(0), "annotationSource");
  QVERIFY(!groups[0]->isExpanded());
  QCOMPARE(file->view, originalView);
  // Explicitly clicking the same current ID must reveal it again after manual
  // page navigation. An ordinary refresh must leave the chosen page alone.
  window.findChild<QPushButton*>("annotationsPreviousPage")->click();
  window.refresh();
  QVERIFY(!annotationUiTableIndex(table, annotationUiId(267)).isValid());
  QTest::mouseClick(plot, Qt::LeftButton, Qt::NoModifier,
                    plot->toPixel(80000, 100.25e6).toPoint());
  QVERIFY(annotationUiTableIndex(table, annotationUiId(267)).isValid());
  QCOMPARE(file->view, originalView);
  const auto counts = QString("匹配 268 / 总数 268 / 已选 1");
  QVERIFY(
      window.findChild<QLabel*>("annotationCounts")->text().startsWith(counts));
  QCOMPARE(window.findChild<QLabel*>("treeAnnotationCounts")->text(), counts);
  QVERIFY(
      window.findChild<QLabel*>("selectedCount")->text().startsWith(counts));
  window.findChild<QPushButton*>("treeSelectAll")->click();
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{268});
  window.findChild<QPushButton*>("annotationsPreviousPage")->click();
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{268});
  window.findChild<QComboBox*>("annotationSourceFilter")->setCurrentIndex(3);
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{263});
  window.findChild<QCheckBox*>("annotationLayer2")->setChecked(false);
  QVERIFY(file->selectedMarkIds.empty() && file->activeMarkId.empty() &&
          file->activeSegmentId.empty() && file->selectionAnchorId.empty());
  QCOMPARE(table->model()->rowCount(), 0);
  window.findChild<QCheckBox*>("annotationLayer2")->setChecked(true);
  window.findChild<QComboBox*>("annotationSourceFilter")->setCurrentIndex(0);
  QVERIFY(file->selectedMarkIds.empty());
  QCOMPARE(table->model()->rowCount(), 200);
}

void UiTests::annotationHoverProjectionAndNoCalculation() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 8);
  QVERIFY2(published.ok, published.error.c_str());
  window.session().selectMarks({annotationUiId(4)}, annotationUiId(4));
  showWindow(window);
  window.refresh();
  window.findChild<QPushButton*>("resultTab")->click();
  auto* table = window.findChild<QTableView*>("annotationTable");
  auto* tree = window.findChild<QTreeWidget*>("projectTree");
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  QVERIFY(table && tree && plot);
  for (auto* chart : window.findChildren<PlotWidget*>())
    QTRY_VERIFY(chart->isDisplaySettled());
  const auto* file = window.session().activeFile();
  const auto ids = file->selectedMarkIds;
  const auto active = file->activeMarkId;
  const auto segment = file->activeSegmentId;
  const auto view = file->view;
  const auto powers = plot->powerGenerationCount();
  const auto uploads = plot->textureUploadCount();
  const auto display = plot->displayGeneration();
  QSignalSpy reset(table->model(), &QAbstractItemModel::modelReset);
  const auto first = annotationUiTableIndex(table, annotationUiId(1));
  QVERIFY(first.isValid());
  table->scrollTo(first, QAbstractItemView::EnsureVisible);
  QCoreApplication::processEvents();
  const auto firstPoint = table->visualRect(first).center();
  QVERIFY(table->viewport()->rect().contains(firstPoint));
  const auto firstGlobal = table->viewport()->mapToGlobal(firstPoint);
  QVERIFY(window.rect().contains(window.mapFromGlobal(firstGlobal)));
  QTest::mouseMove(window.windowHandle(),
                   window.windowHandle()->mapFromGlobal(firstGlobal));
  QTRY_COMPARE(file->hoveredMarkId, annotationUiId(1));
  QCOMPARE(plot->property("hoveredAnnotationId").toString().toStdString(),
           annotationUiId(1));
  QTRY_VERIFY(annotationUiTreeItem(tree, annotationUiId(1)));
  QVERIFY(annotationUiTreeItem(tree, annotationUiId(1))
              ->data(0, Qt::UserRole + 3)
              .toInt() &
          4);
  auto* target = annotationUiTreeItem(tree, annotationUiId(6));
  QVERIFY(target);
  const auto targetKey = annotationUiTreeKey(target);
  QPoint targetGlobal;
  QVERIFY(annotationUiRevealTreeItem(window, tree, targetKey, &targetGlobal));
  QTest::mouseMove(window.windowHandle(),
                   window.windowHandle()->mapFromGlobal(targetGlobal));
  QTRY_COMPARE(file->hoveredMarkId, annotationUiId(6));
  QVERIFY(annotationUiTableIndex(table, annotationUiId(6))
              .data(Qt::UserRole + 1)
              .toInt() &
          4);
  QVERIFY(annotationUiMoveMouse(window, plot,
                                plot->toPixel(80000, 100.25e6).toPoint()));
  QTRY_COMPARE(file->hoveredMarkId, annotationUiId(7));
  QCOMPARE(file->selectedMarkIds, ids);
  QCOMPARE(file->activeMarkId, active);
  QCOMPARE(file->activeSegmentId, segment);
  QCOMPARE(file->view, view);
  QCOMPARE(plot->powerGenerationCount(), powers);
  QCOMPARE(plot->textureUploadCount(), uploads);
  QCOMPARE(plot->displayGeneration(), display);
  QCOMPARE(reset.count(), 0);
  QEvent leave(QEvent::Leave);
  QCoreApplication::sendEvent(plot, &leave);
  QVERIFY(file->hoveredMarkId.empty() && file->hoveredSegmentId.empty());
}

void UiTests::annotationCurrentSegmentGeometryAndHandles() {
  MainWindow window;
  window.session().addDemoFile("segments.iq", 1e6, 100e6, .1);
  auto* file = window.session().activeFile();
  const auto first =
      window.session().addMark({{5000, 15000}, {99.7e6, 99.8e6}});
  QVERIFY(window.session()
              .annotations()
              .addSegment(*file, first, {{30000, 45000}, {100.1e6, 100.2e6}})
              .ok);
  const auto other =
      window.session().addMark({{55000, 65000}, {99.9e6, 100e6}});
  const auto* annotation = findAnnotation(*file, first);
  const auto firstSegment = annotation->segments[0].id;
  const auto secondSegment = annotation->segments[1].id;
  QVERIFY(window.session()
              .annotations()
              .select(*file, {first, other}, first, secondSegment)
              .ok);
  window.session().setView(fullRange(file->metadata));
  showWindow(window);
  window.refresh();
  window.findChild<QPushButton*>("resultTab")->click();
  auto* table = window.findChild<QTableView*>("annotationTable");
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  auto* selector = window.findChild<QComboBox*>("annotationSegment");
  QVERIFY(table && plot && selector);
  const auto row = annotationUiTableIndex(table, first).row();
  QCOMPARE(table->model()->index(row, 3).data().toString(),
           QString("第 2/2 段"));
  QCOMPARE(table->model()->index(row, 4).data().toString(), QString("30"));
  QCOMPARE(table->model()->index(row, 5).data().toString(), QString("45"));
  QCOMPARE(table->model()->index(row, 6).data().toString().toDouble(), 100.15);
  QCOMPARE(table->model()->index(row, 7).data().toString().toDouble(), 100.0);
  QTRY_COMPARE(plot->property("annotationHandleCount").toInt(), 8);
  selector->setCurrentIndex(
      selector->findData(QString::fromStdString(firstSegment)));
  QCOMPARE(file->activeSegmentId, firstSegment);
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{2});
  QCOMPARE(table->model()->index(row, 4).data().toString(), QString("5"));
  QVERIFY(
      window.findChild<QLabel*>("annotationIdentity")->text().contains("1/2"));
  QTRY_COMPARE(plot->property("activeAnnotationSegmentId").toString(),
               QString::fromStdString(firstSegment));
  QTRY_COMPARE(plot->property("annotationHandleCount").toInt(), 8);
  const auto source =
      annotationUiSource(*file, "unknown-time", AnnotationSource::Algorithm);
  const auto unknown =
      annotationUiValue(*file, source, "unknown", "Unknown time", {},
                        ViewRange{{50000, 80000}, {100.3e6, 100.4e6}});
  QVERIFY(window.session()
              .annotations()
              .publishSourceSet(*file, source, {unknown})
              .ok);
  window.session().selectMarks({"unknown"}, "unknown");
  window.refresh();
  const auto unknownRow = annotationUiTableIndex(table, "unknown").row();
  QCOMPARE(table->model()->index(unknownRow, 3).data().toString(),
           QString("时间未定位"));
  QCOMPARE(table->model()->index(unknownRow, 4).data().toString(),
           QString("—"));
  QCOMPARE(table->model()->index(unknownRow, 5).data().toString(),
           QString("—"));
  QCOMPARE(table->model()->index(unknownRow, 6).data().toString().toDouble(),
           100.35);
  QVERIFY(file->activeSegmentId.empty() &&
          findAnnotation(*file, "unknown")->segments.empty());
  QTRY_COMPARE(plot->property("annotationHandleCount").toInt(), 0);
}

void UiTests::annotationSelectionContextAndPublication() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 8);
  QVERIFY2(published.ok, published.error.c_str());
  auto* file = window.session().activeFile();
  file->metadata.sampleFormat.channels = 2;
  window.session().selectMarks({annotationUiId(4)}, annotationUiId(4));
  showWindow(window);
  window.refresh();
  auto* start = window.findChild<QLineEdit*>("annotationStartMilliseconds");
  QVERIFY(start && start->isEnabled());
  start->setFocus();
  start->selectAll();
  QTest::keyClicks(start, "12.345");
  QVERIFY(start->property("parameterDraft").toBool());
  const auto originalRange = findAnnotation(*file, annotationUiId(4))->range;
  const auto source =
      annotationUiSource(*file, "background", AnnotationSource::Algorithm);
  auto added =
      annotationUiValue(*file, source, "background", "Background result",
                        {{{25000, 35000}, {99.8e6, 99.9e6}}});
  QVERIFY(window.session()
              .annotations()
              .publishSourceSet(*file, source, {added})
              .ok);
  window.refresh();
  QCOMPARE(start->text(), QString("12.345"));
  QVERIFY(start->property("parameterDraft").toBool());
  QCOMPARE(findAnnotation(*file, annotationUiId(4))->range, originalRange);
  QCOMPARE(file->selectedMarkIds, std::vector<std::string>{annotationUiId(4)});
  QCOMPARE(annotationCounts(*file).matching, std::size_t{9});
  window.session().annotations().hover(*file, annotationUiId(7));
  file->metadata.sampleFormat.selectedChannel = 1;
  window.refresh();
  QVERIFY(file->selectedMarkIds.empty() && file->selectionAnchorId.empty() &&
          file->hoveredMarkId.empty());
  QVERIFY(!start->property("parameterDraft").toBool());
  QCOMPARE(
      window.findChild<QTableView*>("annotationTable")->model()->rowCount(), 0);
  const auto channelMark =
      window.session().addMark({{10000, 20000}, {99.8e6, 99.9e6}});
  QVERIFY(!channelMark.empty());
  window.refresh();
  QCOMPARE(annotationCounts(*file).matching, std::size_t{1});
  QCOMPARE(file->activeMarkId, channelMark);
  file->metadata.sampleFormat.selectedChannel = 0;
  window.refresh();
  QVERIFY(file->selectedMarkIds.empty());
  QCOMPARE(annotationCounts(*file).matching, std::size_t{9});
  window.session().selectMarks({annotationUiId(4)}, annotationUiId(4));
  const auto firstFile = file->metadata.id;
  window.session().addDemoFile("other.iq", 1e6, 100e6, .1);
  window.refresh();
  QVERIFY(window.session().activeFile()->selectedMarkIds.empty());
  QVERIFY(window.session().activateFile(firstFile));
  window.refresh();
  QCOMPARE(window.session().activeFile()->selectedMarkIds,
           std::vector<std::string>{annotationUiId(4)});
  window.session().newProject();
  window.refresh();
  QVERIFY(!window.session().activeFile());
  QCOMPARE(
      window.findChild<QTableView*>("annotationTable")->model()->rowCount(), 0);
}

void UiTests::annotationTreeDemandLoadingAndViewOrder() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 268);
  QVERIFY2(published.ok, published.error.c_str());
  showWindow(window);
  window.refresh();
  auto* tree = window.findChild<QTreeWidget*>("projectTree");
  auto* table = window.findChild<QTableView*>("annotationTable");
  QVERIFY(tree && table);
  QTRY_COMPARE(descendantsOfType(tree->topLevelItem(0), "mark").size(), 45);
  auto groups = descendantsOfType(tree->topLevelItem(0), "annotationSource");
  groups[0]->setExpanded(false);
  auto more = descendantsOfType(tree->topLevelItem(0), "annotationMore");
  QCOMPARE(more.size(), 1);
  const auto moreKey = annotationUiTreeKey(more[0]);
  QVERIFY(annotationUiClickTreeItem(window, tree, moreKey));
  QTRY_COMPARE(descendantsOfType(tree->topLevelItem(0), "mark").size(), 85);
  groups = descendantsOfType(tree->topLevelItem(0), "annotationSource");
  QVERIFY(!groups[0]->isExpanded());
  for (int expected : {125, 165, 205, 245, 268}) {
    more = descendantsOfType(tree->topLevelItem(0), "annotationMore");
    QCOMPARE(more.size(), 1);
    QCOMPARE(annotationUiTreeKey(more[0]), moreKey);
    QVERIFY(annotationUiClickTreeItem(window, tree, moreKey));
    QTRY_COMPARE(descendantsOfType(tree->topLevelItem(0), "mark").size(),
                 expected);
  }
  QVERIFY(descendantsOfType(tree->topLevelItem(0), "annotationMore").isEmpty());
  window.findChild<QPushButton*>("resultTab")->click();
  const auto headerPoint =
      QPoint(table->horizontalHeader()->sectionViewportPosition(1) + 20,
             table->horizontalHeader()->height() / 2);
  QTest::mouseClick(table->horizontalHeader()->viewport(), Qt::LeftButton,
                    Qt::NoModifier, headerPoint);
  QTRY_VERIFY(annotationUiTreeItem(tree, annotationUiId(267)));
  const auto firstKey =
      annotationUiTreeKey(annotationUiTreeItem(tree, annotationUiId(267)));
  const auto thirdKey =
      annotationUiTreeKey(annotationUiTreeItem(tree, annotationUiId(265)));
  QVERIFY(!firstKey.isEmpty() && !thirdKey.isEmpty());
  QVERIFY(annotationUiClickTreeItem(window, tree, firstKey));
  QVERIFY(annotationUiClickTreeItem(window, tree, thirdKey, Qt::LeftButton,
                                    Qt::ShiftModifier));
  QCOMPARE(window.session().activeFile()->selectedMarkIds,
           (std::vector<std::string>{annotationUiId(267), annotationUiId(266),
                                     annotationUiId(265)}));
  QCOMPARE(window.session().activeFile()->selectionAnchorId,
           annotationUiId(267));
}

void UiTests::annotationModifiedPlotClicksPreserveAnchorAndCurrent() {
  MainWindow window;
  window.session().addDemoFile("modified-selection.iq", 1e6, 100e6, .1);
  auto* file = window.session().activeFile();
  const std::vector<ViewRange> geometry{{{10000, 25000}, {99.7e6, 99.8e6}},
                                        {{35000, 50000}, {99.7e6, 99.8e6}},
                                        {{60000, 75000}, {99.7e6, 99.8e6}}};
  std::vector<std::string> ids;
  for (const auto& range : geometry)
    ids.push_back(window.session().addMark(range));
  window.session().setView(fullRange(file->metadata));
  showWindow(window);
  window.refresh();
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  QVERIFY(plot);
  const auto originalView = file->view;
  const auto first = plot->toPixel(17500, 99.75e6).toPoint();
  const auto middle = plot->toPixel(42500, 99.75e6).toPoint();
  const auto last = plot->toPixel(67500, 99.75e6).toPoint();
  QTest::mouseClick(plot, Qt::LeftButton, Qt::NoModifier, last);
  QTest::mouseClick(plot, Qt::LeftButton, Qt::ShiftModifier, first);
  QCOMPARE(file->selectedMarkIds, ids);
  QCOMPARE(file->activeMarkId, ids[0]);
  QCOMPARE(file->selectionAnchorId, ids[2]);

  // A selected but noncurrent body's pointer-down must not replace the anchor
  // before the modifier click is interpreted on release.
  QTest::mousePress(plot, Qt::LeftButton, Qt::ShiftModifier, middle);
  QCOMPARE(file->activeMarkId, ids[0]);
  QCOMPARE(file->selectionAnchorId, ids[2]);
  QCOMPARE(file->selectedMarkIds, ids);
  QTest::mouseRelease(plot, Qt::LeftButton, Qt::ShiftModifier, middle);
  QCOMPARE(file->selectedMarkIds, (std::vector<std::string>{ids[1], ids[2]}));
  QCOMPARE(file->activeMarkId, ids[1]);
  QCOMPARE(file->selectionAnchorId, ids[2]);

  QTest::mouseClick(plot, Qt::LeftButton, Qt::NoModifier, last);
  QTest::mouseClick(plot, Qt::LeftButton, Qt::ShiftModifier, first);
  QCOMPARE(file->activeMarkId, ids[0]);
  QCOMPARE(file->selectionAnchorId, ids[2]);
  QTest::mousePress(plot, Qt::LeftButton, Qt::ControlModifier, middle);
  QCOMPARE(file->activeMarkId, ids[0]);
  QCOMPARE(file->selectionAnchorId, ids[2]);
  QTest::mouseRelease(plot, Qt::LeftButton, Qt::ControlModifier, middle);
  QCOMPARE(file->selectedMarkIds, (std::vector<std::string>{ids[0], ids[2]}));
  QCOMPARE(file->activeMarkId, ids[0]);
  QCOMPARE(file->selectionAnchorId, ids[1]);
  QCOMPARE(file->view, originalView);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    QCOMPARE(findAnnotation(*file, ids[i])->range, geometry[i]);
    QCOMPARE(findAnnotation(*file, ids[i])->revisions.size(), std::size_t{1});
  }
}

void UiTests::annotationCheckboxShiftExtendsSelection() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 268);
  QVERIFY2(published.ok, published.error.c_str());
  auto* file = window.session().activeFile();
  window.session().selectMarks({annotationUiId(267), annotationUiId(0)},
                               annotationUiId(0));
  showWindow(window);
  window.refresh();
  window.findChild<QPushButton*>("resultTab")->click();
  auto* table = window.findChild<QTableView*>("annotationTable");
  QVERIFY(table);
  window.findChild<QPushButton*>("annotationsNextPage")->click();
  const auto cell = annotationUiTableIndex(table, annotationUiId(220), 0);
  QVERIFY(cell.isValid());
  table->scrollTo(cell);
  QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::ShiftModifier,
                    table->visualRect(cell).center());
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{222});
  QCOMPARE(file->activeMarkId, annotationUiId(220));
  QCOMPARE(file->selectionAnchorId, annotationUiId(0));
  for (int i = 0; i <= 220; ++i)
    QVERIFY(std::find(file->selectedMarkIds.begin(),
                      file->selectedMarkIds.end(),
                      annotationUiId(i)) != file->selectedMarkIds.end());
  QVERIFY(std::find(file->selectedMarkIds.begin(), file->selectedMarkIds.end(),
                    annotationUiId(267)) != file->selectedMarkIds.end());
  const auto next = annotationUiTableIndex(table, annotationUiId(230), 0);
  table->scrollTo(next);
  QTest::mouseClick(table->viewport(), Qt::LeftButton,
                    Qt::ControlModifier | Qt::ShiftModifier,
                    table->visualRect(next).center());
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{232});
  QCOMPARE(file->selectionAnchorId, annotationUiId(0));
  QCOMPARE(file->activeMarkId, annotationUiId(230));
}

void UiTests::annotationHoverTargetsOnlyOneSegment() {
  MainWindow window;
  window.session().addDemoFile("segment-hover.iq", 1e6, 100e6, .1);
  auto* file = window.session().activeFile();
  const auto id = window.session().addMark({{5000, 15000}, {99.7e6, 99.8e6}});
  QVERIFY(window.session()
              .annotations()
              .addSegment(*file, id, {{30000, 45000}, {100.1e6, 100.2e6}})
              .ok);
  const auto* annotation = findAnnotation(*file, id);
  const auto firstSegment = annotation->segments[0].id;
  const auto secondSegment = annotation->segments[1].id;
  QVERIFY(
      window.session().annotations().select(*file, {id}, id, firstSegment).ok);
  window.session().setView(fullRange(file->metadata));
  showWindow(window);
  window.refresh();
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  QVERIFY(plot);
  QTRY_VERIFY(plot->isDisplaySettled());
  // Keep both the baseline and subsequent hover cursors outside segment one.
  QTest::mouseMove(plot,
                   plot->plotRect().bottomLeft().toPoint() + QPoint(2, -2));
  QTRY_VERIFY(file->hoveredMarkId.empty());
  const auto before = plot->grab().toImage();
  const auto ratio = before.devicePixelRatio();
  const auto firstBox =
      QRectF(plot->toPixel(5000, 99.8e6), plot->toPixel(15000, 99.7e6))
          .normalized()
          .adjusted(-6, -6, 6, 6);
  const QRect probe(qFloor(firstBox.x() * ratio), qFloor(firstBox.y() * ratio),
                    qCeil(firstBox.width() * ratio),
                    qCeil(firstBox.height() * ratio));
  const auto powers = plot->powerGenerationCount();
  QTest::mouseMove(plot, plot->toPixel(37500, 100.15e6).toPoint());
  QTRY_COMPARE(file->hoveredMarkId, id);
  QTRY_COMPARE(file->hoveredSegmentId, secondSegment);
  const auto after = plot->grab().toImage();
  // This checks the rendered inactive hover target, including the pixels where
  // an unwanted segment-one halo or tooltip would have appeared.
  QCOMPARE(after.copy(probe), before.copy(probe));
  QCOMPARE(file->activeSegmentId, firstSegment);
  QCOMPARE(file->selectedMarkIds, std::vector<std::string>{id});
  QCOMPARE(plot->powerGenerationCount(), powers);
}

void UiTests::annotationTreeStructuralNavigationAndLocate() {
  MainWindow window;
  showWindow(window);
  // The bundled IQ demo exercises the existing CPU DDC/workspace route. The
  // extra annotation objects below are synthetic UI fixtures only.
  window.openNarrowbandDemoProject();
  auto* file = window.session().activeFile();
  QVERIFY(file && window.session().activeChannel());
  const auto source = annotationUiSource(*file, "structural-navigation",
                                         AnnotationSource::Algorithm);
  const auto band = fullRange(file->metadata).frequency;
  const auto span = band.upperHz - band.lowerHz;
  std::vector<Annotation> values;
  for (int i = 0; i < 80; ++i)
    values.push_back(annotationUiValue(
        *file, source, "navigation:" + std::to_string(i),
        "Navigation fixture " + std::to_string(i),
        {{{10000, 20000},
          {band.lowerHz + span * .45, band.lowerHz + span * .55}}}));
  QVERIFY(window.session()
              .annotations()
              .publishSourceSet(*file, source, std::move(values))
              .ok);
  window.refresh();
  auto* tree = window.findChild<QTreeWidget*>("projectTree");
  auto* narrowband = window.findChild<QWidget*>("narrowbandWorkspace");
  QVERIFY(tree && narrowband);
  QTRY_VERIFY(!descendantsOfType(tree->topLevelItem(0), "channel").isEmpty());
  const auto ids = file->selectedMarkIds;
  const auto active = file->activeMarkId;
  const auto segment = file->activeSegmentId;
  const auto anchor = file->selectionAnchorId;
  const auto originalView = file->view;

  // Every navigation key starts from an actual file/channel click. Keyboard
  // focus on these structural nodes must not become a mark selection command.
  for (const auto& kind : {QString("channel"), QString("file")}) {
    for (const auto key : {Qt::Key_Up, Qt::Key_Down, Qt::Key_Home, Qt::Key_End,
                           Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_Space}) {
      auto nodes = descendantsOfType(tree->topLevelItem(0), kind);
      QVERIFY(!nodes.isEmpty());
      const auto nodeKey = annotationUiTreeKey(nodes.front());
      QVERIFY(annotationUiClickTreeItem(window, tree, nodeKey));
      QTRY_VERIFY(tree->currentItem());
      QTRY_COMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), kind);
      tree->setFocus();
      QTest::keyClick(tree, key);
      QCOMPARE(file->selectedMarkIds, ids);
      QCOMPARE(file->activeMarkId, active);
      QCOMPARE(file->activeSegmentId, segment);
      QCOMPARE(file->selectionAnchorId, anchor);
      QCOMPARE(file->view, originalView);
      if (key == Qt::Key_Up)
        QVERIFY(annotationUiTreeKey(tree->currentItem()) != nodeKey);
    }
  }

  auto channels = descendantsOfType(tree->topLevelItem(0), "channel");
  QVERIFY(!channels.isEmpty());
  const auto channelKey = annotationUiTreeKey(channels.front());
  const auto selectedKey =
      annotationUiTreeKey(annotationUiTreeItem(tree, active));
  QVERIFY(!selectedKey.isEmpty());
  // Default Qt navigation can move from a structural node onto a mark without
  // changing the business current object. The next command must use that focus.
  const auto focusMarkFromChannel = [&]() -> std::string {
    if (!annotationUiClickTreeItem(window, tree, channelKey)) return {};
    tree->setFocus();
    for (int step = 0; step < 12; ++step) {
      const auto* focused = tree->currentItem();
      if (focused && focused->data(0, Qt::UserRole).toString() == "mark")
        return focused->data(0, Qt::UserRole + 1).toString().toStdString();
      QTest::keyClick(tree, Qt::Key_Up);
    }
    return {};
  };
  for (const auto key : {Qt::Key_Space, Qt::Key_Up}) {
    const auto focusedId = focusMarkFromChannel();
    QVERIFY(!focusedId.empty() && focusedId != active);
    QCOMPARE(file->selectedMarkIds, ids);
    QCOMPARE(file->activeMarkId, active);
    QCOMPARE(file->activeSegmentId, segment);
    QCOMPARE(file->selectionAnchorId, anchor);
    const auto* preceding = tree->itemAbove(tree->currentItem());
    QVERIFY(preceding && preceding->data(0, Qt::UserRole).toString() == "mark");
    const auto precedingId =
        preceding->data(0, Qt::UserRole + 1).toString().toStdString();
    QTest::keyClick(tree, key);
    if (key == Qt::Key_Space) {
      auto expected = ids;
      expected.push_back(focusedId);
      QCOMPARE(file->selectedMarkIds, expected);
      QCOMPARE(file->activeMarkId, focusedId);
      QTest::keyClick(tree, Qt::Key_Up);
    }
    QCOMPARE(file->selectedMarkIds, std::vector<std::string>{precedingId});
    QCOMPARE(file->activeMarkId, precedingId);
    QCOMPARE(file->view, originalView);
    QVERIFY(annotationUiClickTreeItem(window, tree, selectedKey));
    QCOMPARE(file->selectedMarkIds, ids);
    QCOMPARE(file->activeMarkId, active);
    QCOMPARE(file->activeSegmentId, segment);
    QCOMPARE(file->selectionAnchorId, anchor);
  }

  // Empty business selection still permits native keyboard focus on a mark.
  // Hover must preserve that focus so Space can select the focused object.
  QVERIFY(annotationUiClickTreeItem(window, tree, selectedKey, Qt::LeftButton,
                                    Qt::ControlModifier));
  QVERIFY(file->selectedMarkIds.empty() && file->activeMarkId.empty() &&
          file->activeSegmentId.empty());
  const auto emptySelectionAnchor = file->selectionAnchorId;
  const auto emptySelectionFocus = focusMarkFromChannel();
  QVERIFY(!emptySelectionFocus.empty());
  const auto focusedKey = annotationUiTreeKey(tree->currentItem());
  QPoint focusedGlobal;
  QVERIFY(annotationUiRevealTreeItem(window, tree, focusedKey, &focusedGlobal));
  QTest::mouseMove(window.windowHandle(),
                   window.windowHandle()->mapFromGlobal(focusedGlobal));
  QTRY_COMPARE(file->hoveredMarkId, emptySelectionFocus);
  QCOMPARE(annotationUiTreeKey(tree->currentItem()), focusedKey);
  QVERIFY(file->selectedMarkIds.empty() && file->activeMarkId.empty() &&
          file->activeSegmentId.empty());
  QCOMPARE(file->selectionAnchorId, emptySelectionAnchor);
  QTest::keyClick(tree, Qt::Key_Space);
  QCOMPARE(file->selectedMarkIds,
           std::vector<std::string>{emptySelectionFocus});
  QCOMPARE(file->activeMarkId, emptySelectionFocus);
  QCOMPARE(file->view, originalView);
  QVERIFY(annotationUiClickTreeItem(window, tree, selectedKey));
  QCOMPARE(file->selectedMarkIds, ids);
  QCOMPARE(file->activeMarkId, active);
  QCOMPARE(file->activeSegmentId, segment);
  QCOMPARE(file->selectionAnchorId, anchor);

  QVERIFY(annotationUiClickTreeItem(window, tree, channelKey));
  QTRY_VERIFY(window.session().project().narrowbandWorkspaceOpen);
  QVERIFY(narrowband->isVisible());
  QPoint markGlobal;
  QVERIFY(annotationUiRevealTreeItem(window, tree, selectedKey, &markGlobal));
  QTest::mouseMove(window.windowHandle(),
                   window.windowHandle()->mapFromGlobal(markGlobal));
  QTRY_COMPARE(file->hoveredMarkId, active);
  QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(),
           QString("channel"));
  QCOMPARE(file->selectedMarkIds, ids);
  QCOMPARE(file->activeMarkId, active);

  window.session().setView(fullRange(file->metadata));
  const auto beforeLocate = file->view;
  const auto windowPoint = window.windowHandle()->mapFromGlobal(markGlobal);
  QTest::mouseDClick(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                     windowPoint);
  QTest::mouseRelease(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      windowPoint);
  QTRY_VERIFY(!window.session().project().narrowbandWorkspaceOpen);
  QVERIFY(!narrowband->isVisible());
  QVERIFY(window.findChild<PlotWidget*>("mainPlot")->isVisible());
  QCOMPARE(file->activeMarkId, active);
  QCOMPARE(file->activeSegmentId, segment);
  QVERIFY(file->view != beforeLocate);

  auto files = descendantsOfType(tree->topLevelItem(0), "file");
  QVERIFY(!files.isEmpty());
  QVERIFY(annotationUiClickTreeItem(window, tree,
                                    annotationUiTreeKey(files.front())));
  tree->setFocus();
  QTest::keyClick(tree, Qt::Key_A, Qt::ControlModifier);
  QCOMPARE(file->selectedMarkIds.size(), annotationCounts(*file).matching);
}

void UiTests::annotationTreeSourceColorsSurviveInteractionStates() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 8);
  QVERIFY2(published.ok, published.error.c_str());
  showWindow(window);
  window.refresh();
  auto* tree = window.findChild<QTreeWidget*>("projectTree");
  auto* file = window.session().activeFile();
  QVERIFY(tree && file);
  const std::array ids{annotationUiId(0), annotationUiId(4), annotationUiId(7)};
  const std::array colors{QColor("#65d2bd"), QColor("#edc56d"),
                          QColor("#51c4e9")};
  std::array<QString, 3> keys;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    QTRY_VERIFY(annotationUiTreeItem(tree, ids[i]));
    keys[i] = annotationUiTreeKey(annotationUiTreeItem(tree, ids[i]));
  }
  const auto sourcePixels = [&](std::size_t source) {
    const auto* item = annotationUiTreeItemByKey(tree, keys[source]);
    if (!item) return 0;
    const auto row = tree->visualItemRect(item)
                         .adjusted(20, 4, -4, -4)
                         .intersected(tree->viewport()->rect());
    if (row.isEmpty()) return 0;
    // Inspect Qt's painted text, excluding branch/selection borders, at its
    // actual device pixel ratio. Model color data missed the QSS override.
    const auto image = tree->viewport()->grab(row).toImage();
    const auto expected = colors[source];
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
      for (int x = 0; x < image.width(); ++x) {
        const auto pixel = image.pixelColor(x, y);
        if (std::abs(pixel.red() - expected.red()) <= 8 &&
            std::abs(pixel.green() - expected.green()) <= 8 &&
            std::abs(pixel.blue() - expected.blue()) <= 8)
          ++count;
      }
    return count;
  };
  for (std::size_t i = 0; i < ids.size(); ++i) {
    QVERIFY(annotationUiRevealTreeItem(window, tree, keys[i]));
    QVERIFY2(sourcePixels(i) >= 15,
             qPrintable(
                 QString("Unselected source %1 has no painted color").arg(i)));
  }
  for (std::size_t i = 0; i < ids.size(); ++i) {
    QVERIFY(
        annotationUiClickTreeItem(window, tree, keys[i], Qt::LeftButton,
                                  i ? Qt::ControlModifier : Qt::NoModifier));
    QCOMPARE(file->activeMarkId, ids[i]);
    QVERIFY2(
        sourcePixels(i) >= 15,
        qPrintable(QString("Current source %1 lost its painted color").arg(i)));
  }
  const auto selected = file->selectedMarkIds;
  const auto active = file->activeMarkId;
  QCOMPARE(selected.size(), std::size_t{3});
  for (std::size_t i = 0; i < ids.size(); ++i) {
    QPoint global;
    QVERIFY(annotationUiRevealTreeItem(window, tree, keys[i], &global));
    QVERIFY2(sourcePixels(i) >= 15,
             qPrintable(
                 QString("Selected source %1 lost its painted color").arg(i)));
    QTest::mouseMove(window.windowHandle(),
                     window.windowHandle()->mapFromGlobal(global));
    QTRY_COMPARE(file->hoveredMarkId, ids[i]);
    QVERIFY2(
        sourcePixels(i) >= 15,
        qPrintable(QString("Hovered source %1 lost its painted color").arg(i)));
    QCOMPARE(file->selectedMarkIds, selected);
    QCOMPARE(file->activeMarkId, active);
  }
}
