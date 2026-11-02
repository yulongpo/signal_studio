#pragma once

#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLayout>
#include <QTreeWidgetItemIterator>
#include <array>

// Native Qt interaction coverage. Annotation fixtures are synthetic and do not
// make claims about model quality, GPU inference, or real RF accuracy.
namespace {
std::string annotationUiId(int index) {
  return QString("ui-%1").arg(index, 3, 10, QChar('0')).toStdString();
}

std::string annotationUiPageId(AnnotationSource source, int index) {
  if (source == AnnotationSource::ImportedLabel) return annotationUiId(index);
  return "page-source:" + std::to_string(static_cast<int>(source)) + ":" +
         annotationUiId(index);
}

QTableView* annotationUiTable(MainWindow& window, AnnotationSource source) {
  const std::array names{"importedLabelTable", "manualAnnotationTable",
                          "detectionResultTable"};
  return window.findChild<QTableView*>(names[static_cast<int>(source)]);
}

QTableView* annotationUiActiveTable(MainWindow& window) {
  const auto* panel = window.findChild<AnnotationPanel*>("annotationPanel");
  return panel ? annotationUiTable(window, panel->sourceTab()) : nullptr;
}

QTableView* annotationUiOpenSource(MainWindow& window, AnnotationSource source) {
  const std::array names{"importedLabelsTab", "manualAnnotationsTab",
                          "detectionResultsTab"};
  auto* panel = window.findChild<AnnotationPanel*>("annotationPanel");
  auto* tab = window.findChild<QPushButton*>(names[static_cast<int>(source)]);
  if (!panel || !tab) return nullptr;
  tab->click();
  panel->setSourceTab(source);
  window.refresh();
  return annotationUiActiveTable(window);
}

AnnotationResult annotationUiCheckManualCopy(const FileState& file,
                                             const std::string& sourceId,
                                             const std::string& copyId) {
  const auto fail = [](const char* message) {
    return AnnotationResult{false, false, {}, message};
  };
  const auto* source = findAnnotation(file, sourceId);
  const auto* copy = findAnnotation(file, copyId);
  if (!source || !copy || source->sourceKind == AnnotationSource::Manual ||
      copy->sourceKind != AnnotationSource::Manual || copy->id == source->id ||
      copy->sourceSetId == source->sourceSetId ||
      copy->revisionId == source->revisionId || !copy->conversionSnapshot)
    return fail("Conversion must create an independent Manual identity and snapshot");
  const auto* sourceSet = findSourceSet(file, source->sourceSetId);
  const auto* copySet = findSourceSet(file, copy->sourceSetId);
  const auto& snapshot = *copy->conversionSnapshot;
  if (!sourceSet || !copySet || copySet->sourceKind != AnnotationSource::Manual ||
      snapshot.sourceAnnotationId != source->id ||
      snapshot.sourceRevisionId != source->revisionId ||
      snapshot.sourceOriginRecordId != source->originRecordId ||
      snapshot.sourceKind != source->sourceKind ||
      encodeSourceSet(snapshot.sourceSet) != encodeSourceSet(*sourceSet) ||
      snapshot.sourceName != source->name || snapshot.sourceNote != source->note ||
      snapshot.sourceClassLabel != source->classLabel ||
      snapshot.sourceSegments != source->segments ||
      snapshot.sourceObservation != source->observation ||
      snapshot.sourceObservationFrequencyFrame != source->observationFrequencyFrame ||
      snapshot.sourceObservationTimeQuality != source->observationTimeQuality ||
      !source->originEvidence ||
      snapshot.sourceOriginEvidence != *source->originEvidence ||
      copy->derivedFrom != std::vector<std::string>{sourceId})
    return fail("Manual conversion must retain self-contained immutable source evidence");
  if (copy->name != source->name || copy->classLabel != source->classLabel ||
      copy->observation != source->observation ||
      copy->segments.size() != source->segments.size())
    return fail("Conversion changed source geometry, class or unknown-time observation");
  for (std::size_t i = 0; i < copy->segments.size(); ++i)
    if (copy->segments[i].range != source->segments[i].range ||
        copy->segments[i].frequencyFrame != source->segments[i].frequencyFrame ||
        copy->segments[i].timeQuality != source->segments[i].timeQuality ||
        copy->segments[i].id == source->segments[i].id)
      return fail("Conversion must preserve physical segments with independent IDs");
  return {true};
}

AnnotationResult annotationUiConvertToManual(MainWindow& window,
                                             const std::string& sourceId,
                                             std::string& copyId) {
  auto& file = *window.session().activeFile();
  const auto* source = findAnnotation(file, sourceId);
  if (!source) return {false, false, {}, "Missing conversion source"};
  const auto original = encodeAnnotation(*source);
  const auto setId = source->sourceSetId;
  const auto originalSet = encodeSourceSet(*findSourceSet(file, setId));
  std::vector<std::string> created;
  const auto result = window.session().annotations().convertToManual(
      file, {sourceId}, {}, created);
  if (!result) return result;
  if (created.size() != 1 ||
      encodeAnnotation(*findAnnotation(file, sourceId)) != original ||
      encodeSourceSet(*findSourceSet(file, setId)) != originalSet)
    return {false, false, {}, "Conversion must leave the source and source set unchanged"};
  copyId = created.front();
  const auto checked = annotationUiCheckManualCopy(file, sourceId, copyId);
  if (!checked) return checked;
  window.findChild<AnnotationPanel*>("annotationPanel")
      ->setSourceTab(AnnotationSource::Manual);
  window.session().selectMarks({copyId}, copyId);
  window.refresh();
  return result;
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
  source.originalText =
      QJsonDocument(
          QJsonObject{{"syntheticFixture", true},
                      {"modelInference", false},
                      {"fixtureSourceId", QString::fromStdString(source.id)}})
          .toJson(QJsonDocument::Compact)
          .toStdString();
  source.sourceHash =
      QCryptographicHash::hash(QByteArray::fromStdString(source.originalText),
                               QCryptographicHash::Sha256)
          .toHex()
          .toStdString();
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

// Each source has 268 distinct IDs, including identical display names. Paging
// and filtering must use the active source's ID order, never a mixed row index.
AnnotationResult annotationUiPublishPages(MainWindow& window) {
  auto& session = window.session();
  session.addDemoFile("annotation-pages.iq", 1e6, 100e6, .1);
  auto& file = *session.activeFile();
  file.metadata.sampleFormat.channels = 2;
  for (int kind = 0; kind < 3; ++kind) {
    const auto sourceKind = static_cast<AnnotationSource>(kind);
    const auto source = annotationUiSource(
        file, "page-source:" + std::to_string(kind), sourceKind);
    std::vector<Annotation> values;
    for (int index = 0; index < 268; ++index) {
      const ViewRange range = index == 267 && kind == 0
          ? ViewRange{{70000, 90000}, {100.2e6, 100.3e6}}
          : ViewRange{{10000, 20000}, {99.8e6, 99.9e6}};
      values.push_back(annotationUiValue(
          file, source, annotationUiPageId(sourceKind, index),
          QString("Row %1").arg(index, 3, 10, QChar('0')).toStdString(), {range}));
    }
    const auto result = session.annotations().publishSourceSet(
        file, source, std::move(values));
    if (!result) return result;
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
  if (!table || !table->model()) return {};
  for (int row = 0; row < table->model()->rowCount(); ++row) {
    const auto index = table->model()->index(row, column);
    if (index.data(Qt::UserRole).toString().toStdString() == id) return index;
  }
  return {};
}

void annotationUiClickRow(QTableView* table, const std::string& id,
                          Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  const auto index = annotationUiTableIndex(table, id);
  QVERIFY2(index.isValid(), "The business ID must be present on the active source page");
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
  const auto published = annotationUiPublishPages(window);
  QVERIFY2(published.ok, published.error.c_str());
  showWindow(window);
  window.refresh();
  auto* table = annotationUiOpenSource(window, AnnotationSource::ImportedLabel);
  auto* file = window.session().activeFile();
  const auto originalView = file->view;
  auto* panel = window.findChild<AnnotationPanel*>("annotationPanel");
  auto* tree = window.findChild<QTreeWidget*>("projectTree");
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  QVERIFY(table && panel && tree && plot);
  QCOMPARE(table->model()->columnCount(), 11);
  QCOMPARE(table->model()->rowCount(), 200);
  QVERIFY(window.findChild<QWidget*>("displayPage")->isVisible());
  annotationUiClickRow(table, annotationUiId(0));
  QTRY_VERIFY(window.findChild<QWidget*>("annotationPage")->isVisible());
  QCOMPARE(file->selectedMarkIds, std::vector<std::string>{annotationUiId(0)});
  window.findChild<QPushButton*>("annotationsNextPage")->click();
  QCOMPARE(table->model()->rowCount(), 68);
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{1});
  annotationUiClickRow(table, annotationUiId(220), Qt::ShiftModifier);
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{221});
  QCOMPARE(file->selectionAnchorId, annotationUiId(0));
  for (int i = 0; i <= 220; ++i)
    QVERIFY(std::find(file->selectedMarkIds.begin(), file->selectedMarkIds.end(),
                      annotationUiId(i)) != file->selectedMarkIds.end());
  window.findChild<QPushButton*>("annotationsPreviousPage")->click();
  annotationUiClickRow(table, annotationUiId(0));
  window.findChild<QPushButton*>("annotationsNextPage")->click();
  QVERIFY(openInspectorPage(window, "Display"));
  annotationUiClickRow(table, annotationUiId(220), Qt::ControlModifier);
  QVERIFY(window.findChild<QWidget*>("displayPage")->isVisible());
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{2});
  QCOMPARE(file->activeMarkId, annotationUiId(220));
  QTRY_VERIFY(annotationUiTreeItem(tree, annotationUiId(220)));
  QVERIFY(annotationUiTreeItem(tree, annotationUiId(220))->isSelected());
  auto groups = descendantsOfType(tree->topLevelItem(0), "annotationSource");
  QCOMPARE(groups.size(), 3);
  groups[1]->setExpanded(false);
  window.findChild<QPushButton*>("annotationsPreviousPage")->click();
  QVERIFY(!annotationUiTableIndex(table, annotationUiId(220)).isValid());
  const auto treeKey = annotationUiTreeKey(annotationUiTreeItem(tree, annotationUiId(220)));
  QVERIFY(!treeKey.isEmpty() && annotationUiClickTreeItem(window, tree, treeKey));
  QCOMPARE(file->activeMarkId, annotationUiId(220));
  QVERIFY(annotationUiTableIndex(annotationUiActiveTable(window), annotationUiId(220)).isValid());
  QCOMPARE(file->view, originalView);
  window.findChild<QPushButton*>("annotationsPreviousPage")->click();
  QVERIFY(!annotationUiTableIndex(table, annotationUiId(267)).isValid());
  QTest::mouseClick(plot, Qt::LeftButton, Qt::NoModifier,
                    plot->toPixel(80000, 100.25e6).toPoint());
  QVERIFY(window.findChild<QWidget*>("displayPage")->isVisible());
  QCOMPARE(file->selectedMarkIds, std::vector<std::string>{annotationUiId(267)});
  table = annotationUiActiveTable(window);
  QCOMPARE(panel->sourceTab(), AnnotationSource::ImportedLabel);
  QVERIFY(annotationUiTableIndex(table, annotationUiId(267)).isValid());
  QTRY_VERIFY(annotationUiTreeItem(tree, annotationUiId(267)));
  QVERIFY(annotationUiTreeItem(tree, annotationUiId(267))->isSelected());
  groups = descendantsOfType(tree->topLevelItem(0), "annotationSource");
  QVERIFY(!groups[1]->isExpanded());
  QCOMPARE(file->view, originalView);
  // Repeating a selection reveals its page; an ordinary refresh preserves
  // deliberate page navigation even when the current ID is off-page.
  window.findChild<QPushButton*>("annotationsPreviousPage")->click();
  window.refresh();
  QVERIFY(!annotationUiTableIndex(table, annotationUiId(267)).isValid());
  QTest::mouseClick(plot, Qt::LeftButton, Qt::NoModifier,
                    plot->toPixel(80000, 100.25e6).toPoint());
  QVERIFY(annotationUiTableIndex(table, annotationUiId(267)).isValid());
  QCOMPARE(file->view, originalView);
  QVERIFY(window.findChild<QLabel*>("annotationCounts")->text().startsWith(
      "匹配 268 / 总数 268 / 已选 1"));
  QCOMPARE(window.findChild<QLabel*>("treeAnnotationCounts")->text(), QString("已选 1"));
  window.findChild<QPushButton*>("treeSelectAll")->click();
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{268});
  for (const auto& id : file->selectedMarkIds)
    QCOMPARE(findAnnotation(*file, id)->sourceKind, AnnotationSource::ImportedLabel);
  const auto active = file->activeMarkId;
  annotationUiOpenSource(window, AnnotationSource::Manual);
  QVERIFY(file->selectedMarkIds.empty());
  QVERIFY(!annotationOverlayVisible(*file, *findAnnotation(*file, active)));
  table = annotationUiOpenSource(window, AnnotationSource::ImportedLabel);
  QCOMPARE(table->property("matchingAnnotationCount").toULongLong(), qulonglong{268});
  QVERIFY(table->model()->rowCount() > 0);
  QVERIFY(annotationOverlayVisible(*file, *findAnnotation(*file, active)));
  // Explicitly select the previous ID after the tab switch cleared selection.
  window.session().selectMarks({active}, active);
  window.refresh();
  QVERIFY(annotationUiTableIndex(table, active).isValid());
  annotationUiClickRow(table, active);
  QCOMPARE(file->activeMarkId, active);

  auto* query = window.findChild<QLineEdit*>("annotationQuickFilter");
  QVERIFY(query);
  query->setText("Row 2");
  QCOMPARE(table->model()->rowCount(), 68);
  QVERIFY(window.findChild<QLabel*>("annotationCounts")->text().startsWith(
      "匹配 68 / 总数 268 / 已选 1"));
  auto* selectAll = table->findChild<QCheckBox*>("selectAllVisibleAnnotations");
  QVERIFY(selectAll);
  selectAll->click();
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{68});
  for (int i = 200; i < 268; ++i)
    QVERIFY(std::find(file->selectedMarkIds.begin(), file->selectedMarkIds.end(),
                      annotationUiId(i)) != file->selectedMarkIds.end());

  table = annotationUiOpenSource(window, AnnotationSource::Manual);
  QVERIFY(table && file->selectedMarkIds.empty());
  QCOMPARE(query->text(), QString{});
  QCOMPARE(table->model()->rowCount(), 200);
  window.findChild<QPushButton*>("annotationsNextPage")->click();
  QCOMPARE(table->model()->rowCount(), 68);
  query->setText("Row 00");
  QCOMPARE(table->model()->rowCount(), 10);
  QVERIFY(window.findChild<QLabel*>("annotationCounts")->text().startsWith(
      "匹配 10 / 总数 268 / 已选 0"));
  annotationUiClickRow(table, annotationUiPageId(AnnotationSource::Manual, 0));
  QCOMPARE(file->selectedMarkIds, (std::vector<std::string>{
      annotationUiPageId(AnnotationSource::Manual, 0)}));

  table = annotationUiOpenSource(window, AnnotationSource::Algorithm);
  QVERIFY(table && file->selectedMarkIds.empty());
  QCOMPARE(query->text(), QString{});
  QCOMPARE(table->model()->rowCount(), 200);
  window.findChild<QPushButton*>("annotationsNextPage")->click();
  QCOMPARE(table->model()->rowCount(), 68);
  QCOMPARE(table->property("annotationPage").toULongLong(), qulonglong{2});
  table = annotationUiOpenSource(window, AnnotationSource::ImportedLabel);
  QCOMPARE(query->text(), QString("Row 2"));
  QCOMPARE(table->model()->rowCount(), 68);
  QCOMPARE(table->property("annotationPage").toULongLong(), qulonglong{1});
  table = annotationUiOpenSource(window, AnnotationSource::Manual);
  QCOMPARE(query->text(), QString("Row 00"));
  QCOMPARE(table->model()->rowCount(), 10);
  table = annotationUiOpenSource(window, AnnotationSource::Algorithm);
  QCOMPARE(query->text(), QString{});
  QCOMPARE(table->property("annotationPage").toULongLong(), qulonglong{2});
  QVERIFY(annotationUiTableIndex(table,
      annotationUiPageId(AnnotationSource::Algorithm, 220)).isValid());
  // A different tab's hidden rectangle cannot be hit. Tree navigation may
  // cross the source boundary and must reveal the exact ID and its overlay.
  QTest::mouseClick(plot, Qt::LeftButton, Qt::NoModifier,
                    plot->toPixel(80000, 100.25e6).toPoint());
  QCOMPARE(panel->sourceTab(), AnnotationSource::Algorithm);
  QVERIFY(file->activeMarkId.empty());
  QTRY_VERIFY(annotationUiTreeItem(tree, annotationUiId(267)));
  const auto importedKey = annotationUiTreeKey(annotationUiTreeItem(tree, annotationUiId(267)));
  QVERIFY(annotationUiClickTreeItem(window, tree, importedKey));
  QCOMPARE(panel->sourceTab(), AnnotationSource::ImportedLabel);
  QCOMPARE(file->activeMarkId, annotationUiId(267));
  QVERIFY(annotationUiTableIndex(annotationUiActiveTable(window), annotationUiId(267)).isValid());
  QCOMPARE(file->view, originalView);
}

void UiTests::annotationHoverProjectionAndNoCalculation() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 8);
  QVERIFY2(published.ok, published.error.c_str());
  window.session().selectMarks({annotationUiId(0)}, annotationUiId(0));
  showWindow(window);
  window.refresh();
  auto* table = annotationUiOpenSource(window, AnnotationSource::ImportedLabel);
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
  auto* target = annotationUiTreeItem(tree, annotationUiId(2));
  QVERIFY(target);
  const auto targetKey = annotationUiTreeKey(target);
  QPoint targetGlobal;
  QVERIFY(annotationUiRevealTreeItem(window, tree, targetKey, &targetGlobal));
  QTest::mouseMove(window.windowHandle(),
                   window.windowHandle()->mapFromGlobal(targetGlobal));
  QTRY_COMPARE(file->hoveredMarkId, annotationUiId(2));
  QVERIFY(annotationUiTableIndex(table, annotationUiId(2))
              .data(Qt::UserRole + 1)
              .toInt() &
          4);
  QVERIFY(annotationUiMoveMouse(window, plot,
                                plot->toPixel(15000, 99.85e6).toPoint()));
  QTRY_COMPARE(file->hoveredMarkId, annotationUiId(3));
  QCOMPARE(window.findChild<AnnotationPanel*>("annotationPanel")->sourceTab(),
           AnnotationSource::ImportedLabel);
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
  auto* table = annotationUiOpenSource(window, AnnotationSource::Manual);
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
  // Domain fixture selection on the first refresh does not open Inspector.
  QVERIFY(window.findChild<QWidget*>("displayPage")->isVisible());
  QVERIFY(openInspectorPage(window, "Annotation"));
  QVERIFY(revealParameter(selector));
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
  table = annotationUiActiveTable(window);
  QCOMPARE(table, annotationUiTable(window, AnnotationSource::Algorithm));
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
  const auto originalUnknown = encodeAnnotation(*findAnnotation(*file, "unknown"));
  QTRY_COMPARE(plot->property("annotationHandleCount").toInt(), 0);
  QVERIFY(!window.session().annotations().commitObservationFrequency(
      *file, "unknown", {100.31e6, 100.39e6}).ok);
  std::string manualUnknown;
  const auto converted = annotationUiConvertToManual(window, "unknown", manualUnknown);
  QVERIFY2(converted.ok, converted.error.c_str());
  table = annotationUiActiveTable(window);
  QVERIFY(annotationUiTableIndex(table, manualUnknown).isValid());
  QVERIFY(findAnnotation(*file, manualUnknown)->segments.empty());
  QVERIFY(file->activeSegmentId.empty());
  QTRY_COMPARE(plot->property("annotationHandleCount").toInt(), 2);
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, "unknown")), originalUnknown);
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
  QVERIFY(window.findChild<QWidget*>("displayPage")->isVisible());
  QVERIFY(openInspectorPage(window, "Annotation"));
  auto* start = window.findChild<QLineEdit*>("annotationStartMilliseconds");
  QVERIFY(start && start->isEnabled());
  QVERIFY(revealParameter(start));
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
  QVERIFY(window.findChild<QWidget*>("annotationPage")->isVisible());
  QCOMPARE(start->text(), QString("12.345"));
  QVERIFY(start->property("parameterDraft").toBool());
  QCOMPARE(findAnnotation(*file, annotationUiId(4))->range, originalRange);
  QCOMPARE(file->selectedMarkIds, std::vector<std::string>{annotationUiId(4)});
  QCOMPARE(annotationUiActiveTable(window)->model()->rowCount(), 1);
  QCOMPARE(annotationUiActiveTable(window)->property("matchingAnnotationCount")
               .toULongLong(), qulonglong{1});
  window.session().annotations().hover(*file, annotationUiId(7));
  file->metadata.sampleFormat.selectedChannel = 1;
  window.refresh();
  QVERIFY(file->selectedMarkIds.empty() && file->selectionAnchorId.empty() &&
          file->hoveredMarkId.empty());
  QVERIFY(!start->property("parameterDraft").toBool());
  QCOMPARE(
      annotationUiActiveTable(window)->model()->rowCount(), 0);
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
  QVERIFY(window.findChild<QWidget*>("annotationPage")->isVisible());
  QVERIFY(window.session().activeFile()->selectedMarkIds.empty());
  QVERIFY(window.session().activateFile(firstFile));
  window.refresh();
  QVERIFY(window.findChild<QWidget*>("annotationPage")->isVisible());
  QCOMPARE(window.session().activeFile()->selectedMarkIds,
           std::vector<std::string>{annotationUiId(4)});
  window.session().newProject();
  window.refresh();
  QVERIFY(!window.session().activeFile());
  QCOMPARE(
      annotationUiActiveTable(window)->model()->rowCount(), 0);
}

void UiTests::annotationTreeDemandLoadingAndViewOrder() {
  MainWindow window;
  const auto published = annotationUiPublishPages(window);
  QVERIFY2(published.ok, published.error.c_str());
  showWindow(window);
  window.refresh();
  auto* tree = window.findChild<QTreeWidget*>("projectTree");
  auto* table = annotationUiOpenSource(window, AnnotationSource::ImportedLabel);
  QVERIFY(tree && table);
  QTRY_COMPARE(descendantsOfType(tree->topLevelItem(0), "mark").size(), 120);
  auto groups = descendantsOfType(tree->topLevelItem(0), "annotationSource");
  QCOMPARE(groups.size(), 3);
  for (auto* group : groups)
    QCOMPARE(descendantsOfType(group, "mark").size(), 40);
  groups[1]->setExpanded(false);
  auto more = descendantsOfType(tree->topLevelItem(0), "annotationMore");
  QCOMPARE(more.size(), 3);
  QCOMPARE(more[0]->parent()->data(0, Qt::UserRole + 1).toInt(), 0);
  const auto moreKey = annotationUiTreeKey(more[0]);
  QVERIFY(annotationUiClickTreeItem(window, tree, moreKey));
  QTRY_COMPARE(descendantsOfType(tree->topLevelItem(0), "mark").size(), 160);
  groups = descendantsOfType(tree->topLevelItem(0), "annotationSource");
  QCOMPARE(descendantsOfType(groups[0], "mark").size(), 80);
  QVERIFY(!groups[1]->isExpanded());
  for (int expected : {120, 160, 200, 240, 268}) {
    more = descendantsOfType(tree->topLevelItem(0), "annotationMore");
    QCOMPARE(more.size(), 3);
    QCOMPARE(annotationUiTreeKey(more[0]), moreKey);
    QVERIFY(annotationUiClickTreeItem(window, tree, moreKey));
    QTRY_COMPARE(descendantsOfType(tree->topLevelItem(0), "mark").size(),
                 expected + 80);
    groups = descendantsOfType(tree->topLevelItem(0), "annotationSource");
    QCOMPARE(descendantsOfType(groups[0], "mark").size(), expected);
    QCOMPARE(descendantsOfType(groups[1], "mark").size(), 40);
    QCOMPARE(descendantsOfType(groups[2], "mark").size(), 40);
  }
  QCOMPARE(descendantsOfType(tree->topLevelItem(0), "annotationMore").size(), 2);
  QVERIFY(descendantsOfType(groups[0], "annotationMore").isEmpty());
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
  QCOMPARE(window.findChild<AnnotationPanel*>("annotationPanel")->sourceTab(),
           AnnotationSource::ImportedLabel);
  QVERIFY(annotationUiTableIndex(annotationUiActiveTable(window), annotationUiId(265)).isValid());
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
  const auto published = annotationUiPublishPages(window);
  QVERIFY2(published.ok, published.error.c_str());
  auto* file = window.session().activeFile();
  window.session().selectMarks({annotationUiId(267), annotationUiId(0)},
                               annotationUiId(0));
  showWindow(window);
  window.refresh();
  auto* table = annotationUiOpenSource(window, AnnotationSource::ImportedLabel);
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
  window.session().selectMarks({"navigation:0"}, "navigation:0");
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
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{80});
  for (const auto& id : file->selectedMarkIds)
    QCOMPARE(findAnnotation(*file, id)->sourceKind, AnnotationSource::Algorithm);
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
  const auto manualPeer = window.session().addMark(
      {{30000, 40000}, {99.9e6, 100e6}});
  QVERIFY(!manualPeer.empty());
  window.session().selectMarks({});
  const std::array ids{annotationUiId(0), annotationUiId(4), annotationUiId(7)};
  const std::array peers{annotationUiId(1), manualPeer, annotationUiId(6)};
  const std::array colors{QColor("#65d2bd"), QColor("#edc56d"),
                          QColor("#51c4e9")};
  std::array<QString, 3> keys;
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
    QVERIFY(annotationUiOpenSource(window, static_cast<AnnotationSource>(i)));
    QVERIFY(file->selectedMarkIds.empty());
    QTRY_VERIFY(annotationUiTreeItem(tree, ids[i]));
    keys[i] = annotationUiTreeKey(annotationUiTreeItem(tree, ids[i]));
    QVERIFY(annotationUiRevealTreeItem(window, tree, keys[i]));
    QVERIFY2(sourcePixels(i) >= 15,
             qPrintable(
                 QString("Unselected source %1 has no painted color").arg(i)));
    QVERIFY(
        annotationUiClickTreeItem(window, tree, keys[i], Qt::LeftButton,
                                  Qt::NoModifier));
    QCOMPARE(file->activeMarkId, ids[i]);
    QVERIFY2(
        sourcePixels(i) >= 15,
        qPrintable(QString("Current source %1 lost its painted color").arg(i)));
    QCOMPARE(file->selectedMarkIds, std::vector<std::string>{ids[i]});
    QTRY_VERIFY(annotationUiTreeItem(tree, peers[i]));
    QVERIFY(annotationUiClickTreeItem(window, tree,
        annotationUiTreeKey(annotationUiTreeItem(tree, peers[i])),
        Qt::LeftButton, Qt::ControlModifier));
    const auto selected = file->selectedMarkIds;
    const auto active = file->activeMarkId;
    QCOMPARE(selected.size(), std::size_t{2});
    QCOMPARE(active, peers[i]);
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

// UX-R5 migrations of the four pre-R5 editing cases keep their existing slots.
// Small synthetic Qt scenes exercise the editing contract. They do not run a
// detector or claim real RF accuracy, throughput, or native GPU rendering.
void UiTests::annotationDirectTableDraftsAndPublication() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 8);
  QVERIFY2(published.ok, published.error.c_str());
  auto* file = window.session().activeFile();
  const auto sourceId = annotationUiId(0);
  const auto sourceBefore = encodeAnnotation(*findAnnotation(*file, sourceId));
  const auto sourceSetId = findAnnotation(*file, sourceId)->sourceSetId;
  const auto sourceSetBefore = encodeSourceSet(*findSourceSet(*file, sourceSetId));
  QVERIFY(!window.session().annotations().reviewBatch(
      *file, {sourceId}, ReviewStatus::Accepted).ok);
  window.session().selectMarks({sourceId}, sourceId);
  showWindow(window);
  window.refresh();
  auto* table = annotationUiOpenSource(window, AnnotationSource::ImportedLabel);
  QVERIFY(table && table->isVisible());
  auto index = annotationUiTableIndex(table, sourceId, 1);
  QVERIFY(index.isValid() && !(index.flags() & Qt::ItemIsEditable));
  table->setCurrentIndex(index);
  table->setFocus();
  QTest::keyClick(table, Qt::Key_F2);
  QVERIFY(!window.findChild<QLineEdit*>("annotationTableEditor"));
  auto* panel = window.findChild<AnnotationPanel*>("annotationPanel");
  QVERIFY(panel);
  auto* convert = window.findChild<QPushButton*>("convertAnnotationsToManual");
  QVERIFY(convert && convert->isEnabled());
  QTest::mouseClick(convert, Qt::LeftButton);
  const auto manualId = file->activeMarkId;
  const auto copied = annotationUiCheckManualCopy(*file, sourceId, manualId);
  QVERIFY2(copied.ok, copied.error.c_str());
  const auto sourceSnapshot = *findAnnotation(*file, manualId)->conversionSnapshot;
  QCOMPARE(panel->sourceTab(), AnnotationSource::Manual);
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, sourceId)), sourceBefore);
  QVERIFY(window.session().annotations().reviewBatch(
      *file, {manualId}, ReviewStatus::Accepted).ok);
  window.refresh();
  table = annotationUiActiveTable(window);
  index = annotationUiTableIndex(table, manualId, 1);
  QVERIFY(index.isValid() && (index.flags() & Qt::ItemIsEditable));
  table->scrollTo(index);
  QVERIFY(
      table->viewport()->rect().contains(table->visualRect(index).center()));
  const auto origin = findAnnotation(*file, manualId)->originEvidence;
  QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                     table->visualRect(index).center());
  QPointer<QLineEdit> editor =
      window.findChild<QLineEdit*>("annotationTableEditor");
  QVERIFY(editor && editor->isVisible());
  QTest::keyClicks(editor, "Direct manual name");
  QSignalSpy reset(table->model(), &QAbstractItemModel::modelReset);
  // A synthetic Algorithm publication can reallocate marks, but must not insert
  // rows into the Manual tab or commit its live editor. Formal worker result
  // replacement is exercised by widebandFixtureTaskNativeRoundtrip.
  const auto source = annotationUiSource(*file, "editing-background",
                                         AnnotationSource::Algorithm);
  const auto result = window.session().annotations().publishSourceSet(
      *file, source,
      {annotationUiValue(*file, source, "editing-new", "New result",
                         {{{50000, 60000}, {100.1e6, 100.2e6}}})});
  QVERIFY2(result.ok, result.error.c_str());
  window.refresh();
  QCOMPARE(reset.count(), 0);
  QVERIFY(editor && editor->hasFocus());
  QCOMPARE(editor->text(), QString("Direct manual name"));
  QCOMPARE(findAnnotation(*file, manualId)->name, std::string("Row 000"));
  QTest::keyClick(editor, Qt::Key_Return);
  QCOMPARE(findAnnotation(*file, manualId)->name,
           std::string("Direct manual name"));
  QCOMPARE(findAnnotation(*file, manualId)->reviewStatus,
           ReviewStatus::Accepted);
  QCOMPARE(findAnnotation(*file, manualId)->originEvidence, origin);
  QTRY_COMPARE(table->model()->rowCount(), 2);
  QVERIFY(findAnnotation(*file, "editing-new"));

  // A native Manual and an explicitly converted Algorithm copy both use F2.
  for (const auto& originalId : {annotationUiId(4), annotationUiId(5)}) {
    auto id = originalId;
    const auto originalBefore = encodeAnnotation(*findAnnotation(*file, originalId));
    if (findAnnotation(*file, id)->sourceKind != AnnotationSource::Manual) {
      const auto algorithmBefore = encodeAnnotation(*findAnnotation(*file, id));
      table = annotationUiOpenSource(window, AnnotationSource::Algorithm);
      annotationUiClickRow(table, id);
      index = annotationUiTableIndex(table, id, 1);
      QVERIFY(index.isValid() && !(index.flags() & Qt::ItemIsEditable));
      table->setCurrentIndex(index);
      table->setFocus();
      QTest::keyClick(table, Qt::Key_F2);
      QVERIFY(!window.findChild<QLineEdit*>("annotationTableEditor"));
      const auto result = annotationUiConvertToManual(window, originalId, id);
      QVERIFY2(result.ok, result.error.c_str());
      QCOMPARE(encodeAnnotation(*findAnnotation(*file, originalId)), algorithmBefore);
    }
    window.session().selectMarks({id}, id);
    window.refresh();
    table = annotationUiActiveTable(window);
    index = annotationUiTableIndex(table, id, 1);
    QVERIFY(index.isValid());
    table->setCurrentIndex(index);
    table->setFocus();
    QTest::keyClick(table, Qt::Key_F2);
    editor = window.findChild<QLineEdit*>("annotationTableEditor");
    QVERIFY(editor && editor->isVisible());
    QTest::keyClicks(editor, "F2 direct edit");
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(findAnnotation(*file, id)->name, std::string("F2 direct edit"));
    if (id != originalId)
      QCOMPARE(encodeAnnotation(*findAnnotation(*file, originalId)), originalBefore);
    QCoreApplication::processEvents();
  }

  // An editor Delete deletes text. Escape leaves the current revision intact.
  window.session().selectMarks({manualId}, manualId);
  window.refresh();
  index = annotationUiTableIndex(table, manualId, 1);
  table->setCurrentIndex(index);
  table->setFocus();
  QTest::keyClick(table, Qt::Key_F2);
  editor = window.findChild<QLineEdit*>("annotationTableEditor");
  QVERIFY(editor && editor->isVisible());
  const auto revision = findAnnotation(*file, manualId)->revisionId;
  QTest::keyClick(editor, Qt::Key_Delete);
  QVERIFY(!findAnnotation(*file, manualId)->deleted);
  QVERIFY(editor->text().isEmpty());
  QTest::keyClick(editor, Qt::Key_Escape);
  QCOMPARE(findAnnotation(*file, manualId)->revisionId, revision);
  QCoreApplication::processEvents();

  // Committing before a click must not replace its actual target mid-event.
  index = annotationUiTableIndex(table, manualId, 1);
  table->setCurrentIndex(index);
  table->setFocus();
  QTest::keyClick(table, Qt::Key_F2);
  editor = window.findChild<QLineEdit*>("annotationTableEditor");
  QVERIFY(editor);
  QTest::keyClicks(editor, "Commit before next click");
  auto* tree = window.findChild<QTreeWidget*>("projectTree");
  QVERIFY(tree && tree->topLevelItemCount());
  const auto actions = descendantsOfType(tree->topLevelItem(0), "treeActions");
  QVERIFY(!actions.isEmpty());
  const auto actionKey = annotationUiTreeKey(actions.front());
  QVERIFY(annotationUiRevealTreeItem(window, tree, actionKey));
  QPointer<QPushButton> selectAll =
      window.findChild<QPushButton*>("treeSelectAll");
  auto* resource = window.findChild<QScrollArea*>("resourceScroll");
  QVERIFY(selectAll && resource);
  const auto target = selectAll->mapToGlobal(selectAll->rect().center());
  QVERIFY(resource->viewport()->rect().contains(
      resource->viewport()->mapFromGlobal(target)));
  const auto click = window.windowHandle()->mapFromGlobal(target);
  QTest::mousePress(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                    click);
  QCoreApplication::processEvents();
  QCoreApplication::processEvents();
  QVERIFY(selectAll && selectAll->isDown());
  QCOMPARE(findAnnotation(*file, manualId)->name,
           std::string("Commit before next click"));
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{1});
  QTest::mouseRelease(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      click);
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{3});
  for (const auto& selectedId : file->selectedMarkIds)
    QCOMPARE(findAnnotation(*file, selectedId)->sourceKind, AnnotationSource::Manual);
  QCoreApplication::processEvents();

  // Actual Cancel clicks must bypass the generic outside-click commit policy.
  window.session().selectMarks({manualId}, manualId);
  window.refresh();
  QVERIFY(openInspectorPage(window, "Annotation"));
  auto* low = window.findChild<QLineEdit*>("annotationLowerMHz");
  auto* cancel = window.findChild<QPushButton*>("cancelAnnotationGeometry");
  auto* properties = window.findChild<QScrollArea*>("propScroll");
  QVERIFY(low && cancel && properties);
  properties->ensureWidgetVisible(cancel);
  QCoreApplication::processEvents();
  QVERIFY(properties->viewport()->rect().contains(
      properties->viewport()->mapFromGlobal(
          cancel->mapToGlobal(cancel->rect().center()))));
  const auto beforeCancel = findAnnotation(*file, manualId)->revisionId;
  QVERIFY(revealParameter(low));
  low->setFocus();
  low->selectAll();
  QTest::keyClicks(low, "99.81");
  QVERIFY(revealParameter(cancel));
  QTest::mouseClick(cancel, Qt::LeftButton);
  QCOMPARE(findAnnotation(*file, manualId)->revisionId, beforeCancel);
  QVERIFY(!low->property("parameterDraft").toBool());
  index = annotationUiTableIndex(table, manualId, 1);
  table->setCurrentIndex(index);
  table->setFocus();
  QTest::keyClick(table, Qt::Key_F2);
  editor = window.findChild<QLineEdit*>("annotationTableEditor");
  QVERIFY(editor && editor->isVisible());
  QTest::keyClicks(editor, "Must cancel with real button");
  QVERIFY(revealParameter(cancel));
  QTest::mouseClick(cancel, Qt::LeftButton);
  QCOMPARE(findAnnotation(*file, manualId)->revisionId, beforeCancel);
  QCOMPARE(findAnnotation(*file, manualId)->name,
           std::string("Commit before next click"));
  QVERIFY(!window.findChild<QLineEdit*>("annotationTableEditor"));
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, sourceId)), sourceBefore);
  QCOMPARE(encodeSourceSet(*findSourceSet(*file, sourceSetId)), sourceSetBefore);
  QVERIFY(*findAnnotation(*file, manualId)->conversionSnapshot == sourceSnapshot);
}

void UiTests::annotationAtomicRangeAndUnknownFrequency() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 8);
  QVERIFY2(published.ok, published.error.c_str());
  auto* file = window.session().activeFile();
  const auto sourceId = annotationUiId(5);
  const auto sourceBefore = encodeAnnotation(*findAnnotation(*file, sourceId));
  Channel deniedChannel;
  QVERIFY(!window.session().annotations().bindChannelSource(
      *file, deniedChannel, sourceId,
      findAnnotation(*file, sourceId)->activeSegmentId).ok);
  QVERIFY(!window.session().annotations().deleteBatch(*file, {sourceId}).ok);
  QVERIFY(!window.session().annotations().addSegment(
      *file, sourceId, {{40000, 50000}, {100.1e6 + .125, 100.2e6 + .875}}).ok);
  std::string id;
  const auto converted = annotationUiConvertToManual(window, sourceId, id);
  QVERIFY2(converted.ok, converted.error.c_str());
  const auto sourceSnapshot = *findAnnotation(*file, id)->conversionSnapshot;
  QVERIFY(window.session()
              .annotations()
              .addSegment(*file, id,
                          {{40000, 50000}, {100.1e6 + .125, 100.2e6 + .875}})
              .ok);
  QVERIFY(window.session()
              .annotations()
              .reviewBatch(*file, {id}, ReviewStatus::Accepted)
              .ok);
  auto* annotation = findAnnotation(*file, id);
  const auto first = annotation->segments.front();
  const auto currentSegment = annotation->activeSegmentId;
  const auto before = activeSegment(*annotation)->range;
  window.session().selectMarks({id}, id);
  showWindow(window);
  window.refresh();
  QVERIFY(openInspectorPage(window, "Annotation"));
  auto* panel = window.findChild<AnnotationPanel*>("annotationPanel");
  QVERIFY(panel);
  auto* editRange = window.findChild<QPushButton*>("editAnnotationRange");
  QVERIFY(editRange && revealParameter(editRange));
  const auto history = window.session().annotations().historySize(*file);
  bool dialogChecked = false;
  QTimer::singleShot(0, &window, [&] {
    auto* dialog = window.findChild<QDialog*>("annotationRangeDialog");
    if (!dialog) return;
    auto* start =
        dialog->findChild<QLineEdit*>("annotationRangeStartMilliseconds");
    auto* end = dialog->findChild<QLineEdit*>("annotationRangeEndMilliseconds");
    auto* apply = dialog->findChild<QPushButton*>("applyAnnotationRange");
    // Start alone would be outside the old end. Tab only edits the modal
    // draft, so the complete 55–65 ms range is validated once on Apply.
    start->setFocus();
    start->selectAll();
    QTest::keyClicks(start, "55");
    QTest::keyClick(start, Qt::Key_Tab);
    dialogChecked = activeSegment(*findAnnotation(*file, id))->range == before;
    end->selectAll();
    QTest::keyClicks(end, "65");
    apply->click();
  });
  QTest::mouseClick(editRange, Qt::LeftButton);
  QVERIFY(dialogChecked);
  annotation = findAnnotation(*file, id);
  QCOMPARE(annotation->segments.front(), first);
  QCOMPARE(activeSegment(*annotation)->range.time, (TimeRange{55000, 65000}));
  QCOMPARE(activeSegment(*annotation)->range.frequency, before.frequency);
  QCOMPARE(annotation->activeSegmentId, currentSegment);
  QCOMPARE(window.session().annotations().historySize(*file), history + 1);
  QVERIFY(window.session().annotations().undo(*file).ok);
  QCOMPARE(activeSegment(*findAnnotation(*file, id))->range, before);

  const auto source = annotationUiSource(*file, "editing-observation",
                                         AnnotationSource::Algorithm);
  const auto unknown = annotationUiValue(
      *file, source, "editing-unknown", "Unknown time", {},
      ViewRange{{20000, 80000}, {99.9e6 + .25, 100.1e6 + .75}});
  QVERIFY(window.session()
              .annotations()
              .publishSourceSet(*file, source, {unknown})
              .ok);
  const auto publishedOrigin =
      findAnnotation(*file, unknown.id)->originEvidence;
  const auto originalEvidence = *publishedOrigin;
  QVERIFY(originalEvidence == *unknown.originEvidence);
  const auto unknownBefore = encodeAnnotation(*findAnnotation(*file, unknown.id));
  window.session().selectMarks({unknown.id}, unknown.id);
  window.refresh();
  QVERIFY(openInspectorPage(window, "Annotation"));
  auto* low = window.findChild<AdaptiveValueEdit*>("annotationLowerMHz");
  QVERIFY(low && !low->isEnabled());
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  QVERIFY(plot);
  QTRY_COMPARE(plot->property("annotationHandleCount").toInt(), 0);
  QVERIFY(!window.session().annotations().commitObservationFrequency(
      *file, unknown.id, {99.95e6, 100.1e6 + .75}).ok);
  std::string manualUnknown;
  const auto unknownConverted = annotationUiConvertToManual(window, unknown.id, manualUnknown);
  QVERIFY2(unknownConverted.ok, unknownConverted.error.c_str());
  const auto manualOrigin = findAnnotation(*file, manualUnknown)->originEvidence;
  const auto unknownSnapshot = *findAnnotation(*file, manualUnknown)->conversionSnapshot;
  QVERIFY(low->isEnabled());
  QVERIFY(revealParameter(low));
  QVERIFY(!window.findChild<QLineEdit*>("annotationStartMilliseconds")
               ->isEnabled());
  low->setFocus();
  low->selectAll();
  QTest::keyClicks(low, "99.95");
  QTest::keyClick(low, Qt::Key_Return);
  annotation = findAnnotation(*file, manualUnknown);
  QVERIFY(annotation->segments.empty());
  QCOMPARE(annotation->observation->time, unknown.observation->time);
  QCOMPARE(annotation->observation->frequency.upperHz,
           unknown.observation->frequency.upperHz);
  QCOMPARE(annotation->observation->frequency.lowerHz, 99.95e6);
  window.refresh();
  QTRY_COMPARE(plot->property("annotationHandleCount").toInt(), 2);
  const auto originalSupport = annotation->observation->time;
  const auto range = *annotation->observation;
  const auto center =
      plot->toPixel((range.time.begin + range.time.end) / 2,
                    (range.frequency.lowerHz + range.frequency.upperHz) / 2);
  QVERIFY(plot->plotRect().contains(center));
  const auto historyBeforeDrag =
      window.session().annotations().historySize(*file);
  QTest::mousePress(plot, Qt::LeftButton, Qt::NoModifier, center.toPoint());
  QVERIFY(plot->hasPendingInteraction());
  QTest::mouseMove(plot, center.toPoint() + QPoint(20, -20), 10);
  // This is the normal synchronous stateChanged → refresh path during a drag.
  // The compatibility projection must remain empty for an unknown-time object.
  window.refresh();
  QVERIFY(findAnnotation(*file, manualUnknown)->range == ViewRange{});
  QCOMPARE(findAnnotation(*file, manualUnknown)->observation->frequency,
           range.frequency);
  QVERIFY(plot->hasPendingInteraction());
  QTest::mouseRelease(plot, Qt::LeftButton, Qt::NoModifier,
                      center.toPoint() + QPoint(20, -20));
  QCoreApplication::processEvents();
  annotation = findAnnotation(*file, manualUnknown);
  QVERIFY(annotation->segments.empty());
  QCOMPARE(annotation->observation->time, originalSupport);
  QVERIFY(annotation->observation->frequency != range.frequency);
  QCOMPARE(annotation->originEvidence, manualOrigin);
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, unknown.id)), unknownBefore);
  QVERIFY(*findAnnotation(*file, unknown.id)->originEvidence == originalEvidence);
  QVERIFY(*annotation->conversionSnapshot == unknownSnapshot);
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, sourceId)), sourceBefore);
  QVERIFY(*findAnnotation(*file, id)->conversionSnapshot == sourceSnapshot);
  QCOMPARE(window.session().annotations().historySize(*file),
           historyBeforeDrag + 1);
  QVERIFY(window.session().annotations().undo(*file).ok);
  QCOMPARE(findAnnotation(*file, manualUnknown)->observation,
           std::optional<ViewRange>{range});
}

void UiTests::annotationDeletePreviewPreservesDraftAndChannels() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 8);
  QVERIFY2(published.ok, published.error.c_str());
  auto* file = window.session().activeFile();
  const auto sourceId = annotationUiId(0);
  const auto sourceBefore = encodeAnnotation(*findAnnotation(*file, sourceId));
  QVERIFY(!window.session().annotations().deleteBatch(*file, {sourceId}).ok);
  Channel deniedChannel;
  QVERIFY(!window.session().annotations().bindChannelSource(
      *file, deniedChannel, sourceId,
      findAnnotation(*file, sourceId)->activeSegmentId).ok);
  std::string id;
  const auto converted = annotationUiConvertToManual(window, sourceId, id);
  QVERIFY2(converted.ok, converted.error.c_str());
  QVERIFY(window.session()
              .annotations()
              .reviewBatch(*file, {id}, ReviewStatus::Accepted)
              .ok);
  Channel channel;
  channel.id = "editing-channel";
  channel.name = "Retained snapshot";
  const auto channelRange = activeSegment(*findAnnotation(*file, id))->range;
  channel.centerFrequencyHz =
      (channelRange.frequency.lowerHz + channelRange.frequency.upperHz) / 2;
  channel.bandwidthHz =
      channelRange.frequency.upperHz - channelRange.frequency.lowerHz;
  channel.outputSampleRateHz = 250000;
  channel.sourceTime = channelRange.time;
  channel.visibleSourceTime = channelRange.time;
  channel.visibleBasebandFrequency = {-125000, 125000};
  QVERIFY(window.session()
              .annotations()
              .bindChannelSource(*file, channel, id,
                                 findAnnotation(*file, id)->activeSegmentId)
              .ok);
  file->channels.push_back(channel);
  const auto snapshot = channel.sourceSnapshot;
  // A v6 legacy channel already bound to a read-only label is fixture state,
  // not a new binding through the R5 API. Keep its DSP and creation snapshot.
  auto legacy = channel;
  legacy.id = "legacy-label-channel";
  legacy.name = "Legacy source snapshot";
  const auto* originalSource = findAnnotation(*file, sourceId);
  auto legacySnapshot = std::make_shared<AnnotationChannelSnapshot>(*snapshot);
  legacySnapshot->annotationId = sourceId;
  legacySnapshot->segmentId = originalSource->activeSegmentId;
  legacySnapshot->revisionId = originalSource->revisionId;
  legacySnapshot->name = originalSource->name;
  legacySnapshot->sourceKind = originalSource->sourceKind;
  legacySnapshot->reviewStatus = originalSource->reviewStatus;
  legacy.sourceMarkId = legacy.annotationId = sourceId;
  legacy.segmentId = legacySnapshot->segmentId;
  legacy.revisionId = legacySnapshot->revisionId;
  legacy.sourceSnapshot = legacySnapshot;
  file->channels.push_back(legacy);
  window.session().selectMarks({id, annotationUiId(4)}, id);
  window.session().setAuxiliaryMode(AuxiliaryMode::Psd);
  showWindow(window);
  window.refresh();
  auto* psdScope = window.findChild<QComboBox*>("psdScope");
  QVERIFY(psdScope);
  QVERIFY(revealParameter(psdScope));
  psdScope->setCurrentIndex(1);
  QVERIFY(file->display.psdFromSelection);
  QVERIFY(openInspectorPage(window, "Annotation"));
  QVERIFY(annotationUiOpenSource(window, AnnotationSource::Manual));
  auto* remove = window.findChild<QPushButton*>("deleteSelectedAnnotations");
  QVERIFY(remove && revealParameter(remove));
  auto* panel = window.findChild<AnnotationPanel*>("annotationPanel");
  auto* low = window.findChild<QLineEdit*>("annotationLowerMHz");
  QVERIFY(panel && low);
  QVERIFY(revealParameter(low));
  low->setFocus();
  low->selectAll();
  QTest::keyClicks(low, "99.81");
  const auto original = findAnnotation(*file, id)->segments.front().range;
  const auto revision = findAnnotation(*file, id)->revisionId;
  bool previewChecked = false;
  QTimer::singleShot(0, &window, [&] {
    auto* dialog = window.findChild<QDialog*>("annotationDeleteDialog");
    if (!dialog) return;
    auto* summary = dialog->findChild<QLabel*>("annotationDeleteSummary");
    previewChecked = summary && summary->property("objectCount").toInt() == 2 &&
                     summary->property("segmentCount").toInt() == 2 &&
                     summary->property("channelCount").toInt() == 1;
    dialog->findChild<QPushButton*>("deleteAnnotationsCancel")->click();
  });
  QTest::mouseClick(remove, Qt::LeftButton);
  QVERIFY(previewChecked);
  QCOMPARE(findAnnotation(*file, id)->revisionId, revision);
  QCOMPARE(findAnnotation(*file, id)->segments.front().range, original);
  QVERIFY(low->property("parameterDraft").toBool());
  const auto history = window.session().annotations().historySize(*file);
  QTimer::singleShot(0, &window, [&] {
    auto* dialog = window.findChild<QDialog*>("annotationDeleteDialog");
    if (dialog)
      dialog->findChild<QPushButton*>("deleteAnnotationsConfirm")->click();
  });
  QTest::mouseClick(remove, Qt::LeftButton);
  QVERIFY(findAnnotation(*file, id)->deleted);
  QVERIFY(findAnnotation(*file, annotationUiId(4))->deleted);
  QCOMPARE(file->channels.size(), std::size_t{2});
  QCOMPARE(file->channels.front().sourceSnapshot, snapshot);
  QCOMPARE(file->channels.front().sourceRelation,
           ChannelSourceRelation::Missing);
  QCOMPARE(file->channels.back().sourceSnapshot, legacy.sourceSnapshot);
  QCOMPARE(file->channels.back().sourceTime, legacy.sourceTime);
  QCOMPARE(file->channels.back().centerFrequencyHz, legacy.centerFrequencyHz);
  QCOMPARE(file->channels.back().bandwidthHz, legacy.bandwidthHz);
  QCOMPARE(file->channels.back().outputSampleRateHz, legacy.outputSampleRateHz);
  QCOMPARE(file->channels.back().sourceRelation, ChannelSourceRelation::Current);
  QVERIFY(file->activeMarkId.empty() && !file->display.psdFromSelection);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  QString saveError;
  QVERIFY2(ProjectStore::save(directory.filePath("after-delete.json"),
                              window.session().project(), saveError),
           qPrintable(saveError));
  QCOMPARE(window.session().annotations().historySize(*file), history + 1);
  const auto source = annotationUiSource(*file, "delete-background",
                                         AnnotationSource::Algorithm);
  QVERIFY(window.session()
              .annotations()
              .publishSourceSet(
                  *file, source,
                  {annotationUiValue(*file, source, "delete-new", "Late result",
                                     {{{60000, 70000}, {100.1e6, 100.2e6}}})})
              .ok);
  QVERIFY(window.session().annotations().undo(*file).ok);
  QVERIFY(!findAnnotation(*file, id)->deleted);
  QVERIFY(!findAnnotation(*file, annotationUiId(4))->deleted);
  QVERIFY(findAnnotation(*file, "delete-new"));
  QCOMPARE(file->channels.front().sourceRelation,
           ChannelSourceRelation::Current);
  QCOMPARE(file->channels.front().sourceSnapshot, snapshot);
  QCOMPARE(file->channels.back().sourceSnapshot, legacy.sourceSnapshot);
  QCOMPARE(file->channels.back().outputSampleRateHz, legacy.outputSampleRateHz);
  QCOMPARE(file->channels.back().sourceRelation, ChannelSourceRelation::Current);
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, sourceId)), sourceBefore);
  QVERIFY(window.session().focusMark(id));
  QCOMPARE(file->activeMarkId, id);
  QVERIFY2(ProjectStore::save(directory.filePath("after-undo-locate.json"),
                              window.session().project(), saveError),
           qPrintable(saveError));
}

void UiTests::annotationFileMenuCancelsWithoutSubmittingDrafts() {
  const bool nativeDisabled =
      QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
  const auto restoreDialogs = qScopeGuard([nativeDisabled] {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDisabled);
  });
  MainWindow window;
  // This file-menu scene uses imported/Manual fixtures. Durable Algorithm
  // save/reopen and retirement use the actual small worker roundtrip case.
  const auto published = annotationUiPublish(window, 5);
  QVERIFY2(published.ok, published.error.c_str());
  auto* file = window.session().activeFile();
  const auto sourceId = annotationUiId(0);
  const auto sourceBefore = encodeAnnotation(*findAnnotation(*file, sourceId));
  std::string id;
  const auto converted = annotationUiConvertToManual(window, sourceId, id);
  QVERIFY2(converted.ok, converted.error.c_str());
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto saved = directory.filePath("committed-project.ssproj");
  QString preflightError;
  QVERIFY2(ProjectStore::save(directory.filePath("preflight.json"),
                              window.session().project(), preflightError),
           qPrintable(preflightError));
  window.session().selectMarks({id}, id);
  showWindow(window);
  window.refresh();
  QVERIFY(openInspectorPage(window, "Annotation"));
  auto* low = window.findChild<QLineEdit*>("annotationLowerMHz");
  auto* open = window.findChild<QAction*>("openProjectAction");
  QVERIFY(low && open);
  QVERIFY(revealParameter(low));
  const auto revision = findAnnotation(*file, id)->revisionId;
  const auto original = findAnnotation(*file, id)->segments.front().range;
  low->setFocus();
  low->selectAll();
  QTest::keyClicks(low, "99.81");
  QAction* fileAction = nullptr;
  QMenu* fileMenu = nullptr;
  for (auto* action : window.menuBar()->actions())
    if (action->menu() && action->menu()->actions().contains(open)) {
      fileAction = action;
      fileMenu = action->menu();
      break;
    }
  QVERIFY(fileAction && fileMenu);
  QString modalFailure, stage = "open File menu";
  QElapsedTimer stageTime;
  stageTime.start();
  QTimer modalWatchdog;
  connect(&modalWatchdog, &QTimer::timeout, &window, [&] {
    if (auto* message =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
      modalFailure =
          stage + ": " + message->text() + " " + message->informativeText();
      message->done(QMessageBox::Ok);
      return;
    }
    if (!stageTime.hasExpired(5000)) return;
    modalFailure = stage + ": unexpected modal operation exceeded 5 seconds";
    if (auto* dialog =
            qobject_cast<QDialog*>(QApplication::activeModalWidget()))
      dialog->reject();
    if (auto* popup = QApplication::activePopupWidget()) popup->close();
  });
  modalWatchdog.start(20);
  const auto position = window.menuBar()->actionGeometry(fileAction).center();
  QVERIFY(window.menuBar()->rect().contains(position));
  QTest::mouseClick(window.menuBar(), Qt::LeftButton, Qt::NoModifier, position);
  QTRY_VERIFY(fileMenu->isVisible());
  QCOMPARE(findAnnotation(*file, id)->revisionId, revision);
  QVERIFY(low->property("parameterDraft").toBool());
  bool cancelledDialog = false;
  QTimer cancelDialog;
  connect(&cancelDialog, &QTimer::timeout, &window, [&] {
    auto* dialog =
        qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
    if (!dialog) return;
    auto* buttons = dialog->findChild<QDialogButtonBox*>();
    auto* cancel =
        buttons ? buttons->button(QDialogButtonBox::Cancel) : nullptr;
    cancelledDialog = cancel != nullptr;
    if (cancel)
      QTest::mouseClick(cancel, Qt::LeftButton);
    else
      dialog->reject();
  });
  cancelDialog.start(5);
  stage = "cancel project file dialog";
  stageTime.restart();
  QTest::mouseClick(fileMenu, Qt::LeftButton, Qt::NoModifier,
                    fileMenu->actionGeometry(open).center());
  cancelDialog.stop();
  QVERIFY2(modalFailure.isEmpty(), qPrintable(modalFailure));
  QVERIFY(cancelledDialog);
  QCOMPARE(findAnnotation(*file, id)->revisionId, revision);
  QCOMPARE(findAnnotation(*file, id)->segments.front().range, original);
  QVERIFY(low->property("parameterDraft").toBool());
  QCOMPARE(low->text(), QString("99.81"));

  // Saving has always cancelled uncommitted parameters in saveProject().
  // The File menu must not silently commit them before that policy executes.
  stage = "save committed project state";
  stageTime.restart();
  const bool savedProject = window.saveProject(saved);
  QVERIFY2(modalFailure.isEmpty(), qPrintable(modalFailure));
  QVERIFY2(savedProject, "Saving the preflight-validated project failed");
  QCOMPARE(findAnnotation(*file, id)->revisionId, revision);
  QCOMPARE(findAnnotation(*file, id)->segments.front().range, original);
  QVERIFY(!low->property("parameterDraft").toBool());
  QVERIFY(annotationUiOpenSource(window, AnnotationSource::Manual));
  auto* table = annotationUiActiveTable(window);
  QVERIFY(table && table->isVisible());
  table->setCurrentIndex(annotationUiTableIndex(table, id, 1));
  table->setFocus();
  QTest::keyClick(table, Qt::Key_F2);
  auto* editor = window.findChild<QLineEdit*>("annotationTableEditor");
  QVERIFY(editor && editor->isVisible());
  QTest::keyClicks(editor, "Must not cross project replacement");
  const auto generation = window.session().projectGeneration();
  stage = "replace project with saved state";
  stageTime.restart();
  const bool openedProject = window.openProject(saved);
  QVERIFY2(modalFailure.isEmpty(), qPrintable(modalFailure));
  QVERIFY2(openedProject, "Opening the saved project failed");
  QVERIFY(window.findChild<QWidget*>("annotationPage")->isVisible());
  QVERIFY(window.session().projectGeneration() > generation);
  QVERIFY(!window.findChild<QLineEdit*>("annotationTableEditor"));
  file = window.session().activeFile();
  QVERIFY(file);
  QCOMPARE(findAnnotation(*file, id)->name, std::string("Row 000"));
  QCOMPARE(findAnnotation(*file, id)->segments.front().range, original);
  QCOMPARE(encodeAnnotation(*findAnnotation(*file, sourceId)), sourceBefore);
}
