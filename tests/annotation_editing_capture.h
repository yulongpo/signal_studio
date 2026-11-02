#pragma once

#include <QPointer>
#include <map>

// U2 uses the same native Capture and representative IQ as U1. The extra
// annotations and the unprocessed channel configuration are UI fixtures.
const Annotation& editingMark(MainWindow& window, const std::string& id) {
  const auto* value = findAnnotation(*window.session().activeFile(), id);
  require(value, "Expected annotation is missing");
  return *value;
}

void editingSelect(MainWindow& window, const std::string& id,
                   const std::string& segment = {}) {
  auto& session = window.session();
  const auto result =
      session.annotations().select(*session.activeFile(), {id}, id, segment);
  require(result.ok, QString::fromStdString(result.error));
  window.refresh();
  widebandShowInspector(window, "contextAnnotation");
  auto* table = child<QTableView>(window, "annotationTable");
  require(
      waitUntil([&] { return interactionTableIndex(*table, id).isValid(); }),
      "The editing target is not on the visible table page");
}

void editingScene(Capture& capture, const QString& name, QScreen* screen,
                  QWidget* target = nullptr, bool requiresRenderedData = true) {
  widebandScene(capture, name, screen, target, requiresRenderedData);
  auto record = capture.scenes.last().toObject();
  const auto& file = *capture.window.session().activeFile();
  record["activeAnnotationId"] = QString::fromStdString(file.activeMarkId);
  record["activeSegmentId"] = QString::fromStdString(file.activeSegmentId);
  record["tableCounts"] =
      child<QLabel>(capture.window, "annotationCounts")->text();
  record["treeCounts"] =
      child<QLabel>(capture.window, "treeAnnotationCounts")->toolTip();
  record["treeSummary"]=child<QLabel>(capture.window,"treeAnnotationCounts")->text();
  require(record["tableCounts"].toString().startsWith(
              record["treeCounts"].toString() + " · 页 "),
          "Tree and table counts differ during editing");
  capture.scenes[capture.scenes.size() - 1] = record;
}

QLineEdit* editingTableEditor(MainWindow& window, const std::string& id,
                              int column) {
  auto* table = child<QTableView>(window, "annotationTable");
  const auto index = interactionTableIndex(*table, id).siblingAtColumn(column);
  require(index.isValid(), "Editable annotation cell is not on this page");
  table->scrollTo(index);
  QCoreApplication::processEvents();
  const auto rect = table->visualRect(index);
  require(!rect.isEmpty() && table->viewport()->rect().contains(rect),
          "Editable annotation cell is clipped in the actual table");
  QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                    rect.center());
  require(table->currentIndex() == index && table->hasFocus(),
          "Click did not focus the actual editable annotation cell");
  QTest::keyClick(table, Qt::Key_F2);
  QLineEdit* editor = nullptr;
  require(waitUntil([&] {
            editor = window.findChild<QLineEdit*>("annotationTableEditor");
            return editor && editor->isVisible() && editor->hasFocus();
          }),
          "F2 did not open the actual focused table editor");
  require(editor->property("annotationId").toString().toStdString() == id,
          "The cell draft is bound to another annotation");
  return editor;
}

void editingType(QLineEdit& editor, const char* text) {
  widebandScrollTo(&editor);
  widebandNativeReachable(&editor);
  editor.setFocus();
  editor.selectAll();
  QTest::keyClicks(&editor, text);
}

void editingDrag(PlotWidget& plot, QPoint begin, QPoint end) {
  require(plot.plotRect().contains(begin) && plot.plotRect().contains(end),
          "Editing drag must remain in the physical chart");
  QTest::mousePress(&plot, Qt::LeftButton, Qt::NoModifier, begin);
  for (int step = 1; step <= 8; ++step)
    QTest::mouseMove(&plot, begin + (end - begin) * step / 8, 10);
  QTest::mouseRelease(&plot, Qt::LeftButton, Qt::NoModifier, end);
}

void editingDialog(MainWindow& window, const char* name,
                   const std::function<void()>& open,
                   const std::function<void(QDialog&)>& body) {
  bool handled = false;
  QString failure;
  QTimer::singleShot(100, &window, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    try {
      require(dialog && dialog->objectName() == QString::fromLatin1(name),
              "Expected annotation dialog did not open");
      body(*dialog);
      require(!dialog->isVisible(), "The native dialog action did not finish");
    } catch (const std::exception& error) {
      failure = QString::fromUtf8(error.what());
      if (dialog) dialog->reject();
    }
    handled = true;
  });
  open();
  require(waitUntil([&] { return handled; }, 5000),
          "The annotation dialog callback did not run");
  require(failure.isEmpty(), failure);
}

void interactionEditingScenes(Capture& capture, QScreen* screen) {
  auto& window = capture.window;
  auto& session = window.session();
  auto* plot = capture.plots[2];
  const auto imported =
      std::find_if(session.activeFile()->marks.begin(),
                   session.activeFile()->marks.end(), [](const auto& value) {
                     return value.sourceKind == AnnotationSource::ImportedLabel;
                   });
  require(imported != session.activeFile()->marks.end(),
          "The actual imported labels are absent");
  const auto importedId = imported->id;
  std::map<std::string, OriginEvidence> origins;
  for (const auto& id :
       {importedId, std::string("manual-1"), std::string("algo-located"),
        std::string("algo-observation")})
    origins.emplace(id, *editingMark(window, id).originEvidence);

  editingSelect(window, "manual-1", "manual-1-s1");
  const auto channelId = session.createChannel(
      "UI snapshot fixture (unprocessed)", "manual-1", 2497.175e6, .75e6, 4e6,
      {80000, 120000}, ChannelFilter::Standard, false, true);
  require(!channelId.empty(), "The associated snapshot fixture was rejected");
  session.project().narrowbandWorkspaceOpen = false;
  const auto snapshot = *session.activeFile()->channels.back().sourceSnapshot;
  window.refresh();

  editingSelect(window, importedId);
  const auto importedBefore = editingMark(window, importedId);
  const auto importedRange = activeSegment(importedBefore)->range;
  const auto importedPoint = plot->toPixel(
      (importedRange.time.begin + importedRange.time.end) / 2,
      (importedRange.frequency.lowerHz + importedRange.frequency.upperHz) / 2);
  editingDrag(*plot, importedPoint.toPoint(),
              importedPoint.toPoint() + QPoint(20, -8));
  require(waitUntil([&] {
            return editingMark(window, importedId).revisionId !=
                   importedBefore.revisionId;
          }),
          "Imported label graph edit still requires a revision gate");
  require(editingMark(window, importedId).sourceKind ==
                  AnnotationSource::ImportedLabel &&
              editingMark(window, importedId).revisions.size() ==
                  importedBefore.revisions.size() + 1,
          "A drag must revise the same imported object exactly once");
  editingScene(capture, "01-direct-imported-graph-edit", screen);
  click(window, "undoAnnotations");
  require(editingMark(window, importedId).segments == importedBefore.segments,
          "One undo did not restore the imported-label drag");

  editingSelect(window, "algo-located", "algo-located-s2");
  const auto algorithmBefore = editingMark(window, "algo-located");
  QPointer<QLineEdit> editor = editingTableEditor(window, "algo-located", 6);
  editingType(*editor, "2502.27");
  require(
      editingMark(window, "algo-located").segments == algorithmBefore.segments,
      "Typing into a cell applied an unfinished draft");
  QTest::keyClick(editor.data(), Qt::Key_Return);
  require(waitUntil([&] {
            return editingMark(window, "algo-located").revisionId !=
                   algorithmBefore.revisionId;
          }),
          "Algorithm table edit did not automatically commit a revision");
  const auto algorithmAfter = editingMark(window, "algo-located");
  require(
      algorithmAfter.segments.front() == algorithmBefore.segments.front() &&
          std::abs((algorithmAfter.segments.back().range.frequency.lowerHz +
                    algorithmAfter.segments.back().range.frequency.upperHz) /
                       2 -
                   2502.27e6) < .01,
      "A current-segment edit changed another segment or frequency frame");
  editingScene(capture, "02-table-f2-algorithm-edit", screen);

  editingSelect(window, "manual-1", "manual-1-s2");
  const auto manualBeforeName = editingMark(window, "manual-1");
  editor = editingTableEditor(window, "manual-1", 1);
  editingType(*editor, "Manual 01 revisedx");
  QTest::keyClick(editor.data(), Qt::Key_Backspace);
  editor->setCursorPosition(0);
  QTest::keyClick(editor.data(), Qt::Key_Delete);
  require(!editingMark(window, "manual-1").deleted &&
              editingMark(window, "manual-1").name == manualBeforeName.name,
          "Delete inside an editor deleted or committed the annotation");
  editingType(*editor, "Manual 01 revised");
  editingScene(capture, "03-name-draft-delete-edits-text", screen);
  require(editor && editor->isVisible() && editor->hasFocus(),
          "Rendering or background refresh replaced the active cell draft");
  QTest::keyClick(editor.data(), Qt::Key_Return);
  require(waitUntil([&] {
            return editingMark(window, "manual-1").name == "Manual 01 revised";
          }) &&
              editingMark(window, "manual-1").reviewStatus ==
                  manualBeforeName.reviewStatus,
          "A name edit failed or reset the optional quality judgment");

  const auto manualBeforeRange = editingMark(window, "manual-1");
  widebandShowInspector(window, "contextAnnotation");
  auto* rangeButton = child<QPushButton>(window, "editAnnotationRange");
  widebandScrollTo(rangeButton);
  editingDialog(
      window, "annotationRangeDialog",
      [&] { click(window, "editAnnotationRange"); },
      [&](QDialog& dialog) {
        editingType(
            *child<QLineEdit>(dialog, "annotationRangeStartMilliseconds"),
            "13");
        editingType(*child<QLineEdit>(dialog, "annotationRangeEndMilliseconds"),
                    "15");
        editingType(*child<QLineEdit>(dialog, "annotationRangeLowerMHz"),
                    "2496.9");
        editingType(*child<QLineEdit>(dialog, "annotationRangeUpperMHz"),
                    "2497.7");
        require(editingMark(window, "manual-1").segments ==
                    manualBeforeRange.segments,
                "A complete-range draft partially committed its fields");
        editingScene(capture, "04-atomic-range-draft", screen, &dialog);
        click(dialog, "applyAnnotationRange");
      });
  const auto manualAfterRange = editingMark(window, "manual-1");
  require(
      manualAfterRange.segments.front() == manualBeforeRange.segments.front() &&
          manualAfterRange.segments.back().range.time ==
              TimeRange{260000, 300000} &&
          manualAfterRange.revisions.size() ==
              manualBeforeRange.revisions.size() + 1,
      "The complete range did not commit exactly one current-segment edit");

  editingSelect(window, "algo-observation");
  const auto observationBefore = editingMark(window, "algo-observation");
  const auto observation = *observationBefore.observation;
  const auto observationPoint = plot->toPixel(
      (observation.time.begin + observation.time.end) / 2,
      (observation.frequency.lowerHz + observation.frequency.upperHz) / 2);
  editingDrag(*plot, observationPoint.toPoint(),
              observationPoint.toPoint() + QPoint(25, -7));
  require(waitUntil([&] {
            return editingMark(window, "algo-observation").revisionId !=
                   observationBefore.revisionId;
          }),
          "Unknown-time frequency edit did not commit directly");
  const auto observationAfter = editingMark(window, "algo-observation");
  require(
      observationAfter.segments.empty() &&
          observationAfter.observation->time == observation.time &&
          observationAfter.observation->frequency != observation.frequency &&
          observationAfter.observationTimeQuality ==
              observationBefore.observationTimeQuality,
      "A frequency-only edit invented an activity time or segment");
  editingScene(capture, "05-unknown-time-frequency-only", screen);

  editingSelect(window, "algo-located", "algo-located-s2");
  const auto beforeResize = editingMark(window, "algo-located");
  const auto resizeRange = beforeResize.segments.back().range;
  const auto corner =
      plot->toPixel(resizeRange.time.end, resizeRange.frequency.upperHz)
          .toPoint();
  editingDrag(*plot, corner, corner + QPoint(8, -3));
  require(waitUntil([&] {
            return editingMark(window, "algo-located").revisionId !=
                   beforeResize.revisionId;
          }) &&
              editingMark(window, "algo-located").segments.front() ==
                  beforeResize.segments.front(),
          "Continuous algorithm edits changed another segment or were gated");
  click(window, "undoAnnotations");
  require(editingMark(window, "algo-located").segments == beforeResize.segments,
          "One undo did not restore only the last drag");
  editingScene(capture, "06-current-segment-single-command-undo", screen);

  const auto selection = session.annotations().select(
      *session.activeFile(), {"manual-1", "algo-located"}, "algo-located",
      "algo-located-s2");
  require(selection.ok, QString::fromStdString(selection.error));
  window.refresh();
  widebandShowInspector(window, "contextAnnotation");
  editingDialog(
      window, "annotationDeleteDialog",
      [&] { click(window, "deleteSelectedAnnotations"); },
      [&](QDialog& dialog) {
        const auto* summary = child<QLabel>(dialog, "annotationDeleteSummary");
        require(summary->property("objectCount").toInt() == 2 &&
                    summary->property("segmentCount").toInt() == 4 &&
                    summary->property("channelCount").toInt() == 1,
                "Batch delete preview did not count objects, segments, and "
                "channels");
        editingScene(capture, "07-batch-delete-preview", screen, &dialog);
        click(dialog, "deleteAnnotationsConfirm");
      });
  const auto channel = [&]() -> const Channel& {
    const auto& channels = session.activeFile()->channels;
    const auto found =
        std::find_if(channels.begin(), channels.end(),
                     [&](const auto& c) { return c.id == channelId; });
    require(found != channels.end(), "Deleting a mark deleted its channel");
    return *found;
  };
  require(editingMark(window, "manual-1").deleted &&
              editingMark(window, "algo-located").deleted &&
              *channel().sourceSnapshot == snapshot &&
              channel().sourceRelation == ChannelSourceRelation::Missing,
          "Soft deletion changed the channel snapshot or source relation");
  editingScene(capture, "08-source-deleted-channel-snapshot-kept", screen);

  auto source = interactionSource(session, *session.activeFile(),
                                  "u2-background", AnnotationSource::Algorithm);
  const auto published = session.annotations().publishSourceSet(
      *session.activeFile(), source,
      {interactionFixture(*session.activeFile(), source, "u2-background-result",
                          "Background UI fixture",
                          {interactionRange(session.activeFile()->metadata, 7,
                                            8, 2504, 2504.2)})});
  require(published.ok, QString::fromStdString(published.error));
  window.refresh();
  click(window, "undoAnnotations");
  require(!editingMark(window, "manual-1").deleted &&
              !editingMark(window, "algo-located").deleted &&
              !editingMark(window, "u2-background-result").deleted &&
              *channel().sourceSnapshot == snapshot,
          "Undo removed later background publication or changed a channel");
  editingScene(capture, "09-undo-keeps-later-background-results", screen);

  const auto projectPath = capture.output.filePath("u2-edited-project.ssproj");
  require(window.saveProject(projectPath) && window.openProject(projectPath),
          "The edited .ssproj project did not save and reopen");
  require(widebandDisplayReady(window),
          "Reopened actual IQ display did not settle");
  for (const auto& [id, origin] : origins)
    require(*editingMark(window, id).originEvidence == origin,
            "Editing or persistence changed immutable original evidence");
  require(editingMark(window, "algo-observation").segments.empty() &&
              !editingMark(window, "u2-background-result").deleted &&
              *channel().sourceSnapshot == snapshot,
          "Reopening lost the unknown-time state, late result, or channel "
          "snapshot");
  editingSelect(window, "manual-1", "manual-1-s2");
  editingScene(capture, "10-origin-and-edits-preserved-after-reopen", screen);
  auto final = capture.scenes.last().toObject();
  final["reopenVerified"] = true;
  final["originalEvidenceUnchanged"] = true;
  final["projectPath"] = projectPath;
  final["associatedChannelDdcVerified"] = false;
  capture.scenes[capture.scenes.size() - 1] = final;
}

int runAnnotationEditingCapture(const QString& outputDirectory) {
  return runAnnotationInteractionsCapture("U2", outputDirectory,
                                          interactionEditingScenes, 10);
}
