#pragma once

#include <QDialogButtonBox>
#include <QFileDialog>

#include "infrastructure/project_store.h"

// Small synthetic Qt scenes exercise the editing contract. They do not run a
// detector or claim real RF accuracy, throughput, or native GPU rendering.
void UiTests::annotationDirectTableDraftsAndPublication() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 8);
  QVERIFY2(published.ok, published.error.c_str());
  auto* file = window.session().activeFile();
  const auto importedId = annotationUiId(0);
  QVERIFY(window.session()
              .annotations()
              .reviewBatch(*file, {importedId}, ReviewStatus::Accepted)
              .ok);
  window.session().selectMarks({importedId}, importedId);
  showWindow(window);
  window.refresh();
  window.findChild<QPushButton*>("resultTab")->click();
  auto* table = window.findChild<QTableView*>("annotationTable");
  QVERIFY(table && table->isVisible());
  auto index = annotationUiTableIndex(table, importedId, 1);
  QVERIFY(index.flags() & Qt::ItemIsEditable);
  table->scrollTo(index);
  QVERIFY(
      table->viewport()->rect().contains(table->visualRect(index).center()));
  const auto origin = findAnnotation(*file, importedId)->originEvidence;
  QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                     table->visualRect(index).center());
  QPointer<QLineEdit> editor =
      window.findChild<QLineEdit*>("annotationTableEditor");
  QVERIFY(editor && editor->isVisible());
  QTest::keyClicks(editor, "Direct imported name");
  QSignalSpy reset(table->model(), &QAbstractItemModel::modelReset);
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
  QCOMPARE(editor->text(), QString("Direct imported name"));
  QCOMPARE(findAnnotation(*file, importedId)->name, std::string("Row 000"));
  QTest::keyClick(editor, Qt::Key_Return);
  QCOMPARE(findAnnotation(*file, importedId)->name,
           std::string("Direct imported name"));
  QCOMPARE(findAnnotation(*file, importedId)->reviewStatus,
           ReviewStatus::Accepted);
  QCOMPARE(findAnnotation(*file, importedId)->originEvidence, origin);
  QTRY_COMPARE(table->model()->rowCount(), 9);
  QVERIFY(findAnnotation(*file, "editing-new"));

  // Manual and Algorithm use the same F2 entry and retain one stable object.
  for (const auto& id : {annotationUiId(4), annotationUiId(5)}) {
    window.session().selectMarks({id}, id);
    window.refresh();
    index = annotationUiTableIndex(table, id, 1);
    table->setCurrentIndex(index);
    table->setFocus();
    QTest::keyClick(table, Qt::Key_F2);
    editor = window.findChild<QLineEdit*>("annotationTableEditor");
    QVERIFY(editor && editor->isVisible());
    QTest::keyClicks(editor, "F2 direct edit");
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(findAnnotation(*file, id)->name, std::string("F2 direct edit"));
    QCoreApplication::processEvents();
  }

  // An editor Delete deletes text. Escape leaves the current revision intact.
  window.session().selectMarks({importedId}, importedId);
  window.refresh();
  index = annotationUiTableIndex(table, importedId, 1);
  table->setCurrentIndex(index);
  table->setFocus();
  QTest::keyClick(table, Qt::Key_F2);
  editor = window.findChild<QLineEdit*>("annotationTableEditor");
  QVERIFY(editor && editor->isVisible());
  const auto revision = findAnnotation(*file, importedId)->revisionId;
  QTest::keyClick(editor, Qt::Key_Delete);
  QVERIFY(!findAnnotation(*file, importedId)->deleted);
  QVERIFY(editor->text().isEmpty());
  QTest::keyClick(editor, Qt::Key_Escape);
  QCOMPARE(findAnnotation(*file, importedId)->revisionId, revision);
  QCoreApplication::processEvents();

  // Committing before a click must not replace its actual target mid-event.
  index = annotationUiTableIndex(table, importedId, 1);
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
  QCOMPARE(findAnnotation(*file, importedId)->name,
           std::string("Commit before next click"));
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{1});
  QTest::mouseRelease(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      click);
  QCOMPARE(file->selectedMarkIds.size(), std::size_t{9});
  QCoreApplication::processEvents();

  // Actual Cancel clicks must bypass the generic outside-click commit policy.
  window.session().selectMarks({importedId}, importedId);
  window.refresh();
  auto* low = window.findChild<QLineEdit*>("annotationLowerMHz");
  auto* cancel = window.findChild<QPushButton*>("cancelAnnotationGeometry");
  auto* properties = window.findChild<QScrollArea*>("propScroll");
  QVERIFY(low && cancel && properties);
  properties->ensureWidgetVisible(cancel);
  QCoreApplication::processEvents();
  QVERIFY(properties->viewport()->rect().contains(
      properties->viewport()->mapFromGlobal(
          cancel->mapToGlobal(cancel->rect().center()))));
  const auto beforeCancel = findAnnotation(*file, importedId)->revisionId;
  low->setFocus();
  low->selectAll();
  QTest::keyClicks(low, "99.81");
  QTest::mouseClick(cancel, Qt::LeftButton);
  QCOMPARE(findAnnotation(*file, importedId)->revisionId, beforeCancel);
  QVERIFY(!low->property("parameterDraft").toBool());
  index = annotationUiTableIndex(table, importedId, 1);
  table->setCurrentIndex(index);
  table->setFocus();
  QTest::keyClick(table, Qt::Key_F2);
  editor = window.findChild<QLineEdit*>("annotationTableEditor");
  QVERIFY(editor && editor->isVisible());
  QTest::keyClicks(editor, "Must cancel with real button");
  QTest::mouseClick(cancel, Qt::LeftButton);
  QCOMPARE(findAnnotation(*file, importedId)->revisionId, beforeCancel);
  QCOMPARE(findAnnotation(*file, importedId)->name,
           std::string("Commit before next click"));
  QVERIFY(!window.findChild<QLineEdit*>("annotationTableEditor"));
}

void UiTests::annotationAtomicRangeAndUnknownFrequency() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 8);
  QVERIFY2(published.ok, published.error.c_str());
  auto* file = window.session().activeFile();
  const auto id = annotationUiId(5);
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
  auto* panel = window.findChild<AnnotationPanel*>("annotationPanel");
  QVERIFY(panel);
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
  panel->editRange();
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
  window.session().selectMarks({unknown.id}, unknown.id);
  window.refresh();
  auto* low = window.findChild<AdaptiveValueEdit*>("annotationLowerMHz");
  QVERIFY(low && low->isEnabled());
  QVERIFY(!window.findChild<QLineEdit*>("annotationStartMilliseconds")
               ->isEnabled());
  low->setFocus();
  low->selectAll();
  QTest::keyClicks(low, "99.95");
  QTest::keyClick(low, Qt::Key_Return);
  annotation = findAnnotation(*file, unknown.id);
  QVERIFY(annotation->segments.empty());
  QCOMPARE(annotation->observation->time, unknown.observation->time);
  QCOMPARE(annotation->observation->frequency.upperHz,
           unknown.observation->frequency.upperHz);
  QCOMPARE(annotation->observation->frequency.lowerHz, 99.95e6);
  window.refresh();
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
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
  QVERIFY(findAnnotation(*file, unknown.id)->range == ViewRange{});
  QCOMPARE(findAnnotation(*file, unknown.id)->observation->frequency,
           range.frequency);
  QVERIFY(plot->hasPendingInteraction());
  QTest::mouseRelease(plot, Qt::LeftButton, Qt::NoModifier,
                      center.toPoint() + QPoint(20, -20));
  QCoreApplication::processEvents();
  annotation = findAnnotation(*file, unknown.id);
  QVERIFY(annotation->segments.empty());
  QCOMPARE(annotation->observation->time, originalSupport);
  QVERIFY(annotation->observation->frequency != range.frequency);
  QCOMPARE(annotation->originEvidence, publishedOrigin);
  QVERIFY(*annotation->originEvidence == originalEvidence);
  QCOMPARE(window.session().annotations().historySize(*file),
           historyBeforeDrag + 1);
  QVERIFY(window.session().annotations().undo(*file).ok);
  QCOMPARE(findAnnotation(*file, unknown.id)->observation,
           std::optional<ViewRange>{range});
}

void UiTests::annotationDeletePreviewPreservesDraftAndChannels() {
  MainWindow window;
  const auto published = annotationUiPublish(window, 8);
  QVERIFY2(published.ok, published.error.c_str());
  auto* file = window.session().activeFile();
  const auto id = annotationUiId(0);
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
  window.session().selectMarks({id, annotationUiId(4)}, id);
  showWindow(window);
  window.refresh();
  auto* psdScope = window.findChild<QComboBox*>("psdScope");
  QVERIFY(psdScope);
  psdScope->setCurrentIndex(1);
  QVERIFY(file->display.psdFromSelection);
  auto* panel = window.findChild<AnnotationPanel*>("annotationPanel");
  auto* low = window.findChild<QLineEdit*>("annotationLowerMHz");
  QVERIFY(panel && low);
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
  panel->deleteSelected();
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
  panel->deleteSelected();
  QVERIFY(findAnnotation(*file, id)->deleted);
  QVERIFY(findAnnotation(*file, annotationUiId(4))->deleted);
  QCOMPARE(file->channels.size(), std::size_t{1});
  QCOMPARE(file->channels.front().sourceSnapshot, snapshot);
  QCOMPARE(file->channels.front().sourceRelation,
           ChannelSourceRelation::Missing);
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
  const auto published = annotationUiPublish(window, 8);
  QVERIFY2(published.ok, published.error.c_str());
  auto* file = window.session().activeFile();
  const auto id = annotationUiId(0);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto saved = directory.filePath("committed-project.json");
  QString preflightError;
  QVERIFY2(ProjectStore::save(directory.filePath("preflight.json"),
                              window.session().project(), preflightError),
           qPrintable(preflightError));
  window.session().selectMarks({id}, id);
  showWindow(window);
  window.refresh();
  auto* low = window.findChild<QLineEdit*>("annotationLowerMHz");
  auto* open = window.findChild<QAction*>("openProjectJsonAction");
  QVERIFY(low && open);
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
  window.findChild<QPushButton*>("resultTab")->click();
  auto* table = window.findChild<QTableView*>("annotationTable");
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
  QVERIFY(window.session().projectGeneration() > generation);
  QVERIFY(!window.findChild<QLineEdit*>("annotationTableEditor"));
  file = window.session().activeFile();
  QVERIFY(file);
  QCOMPARE(findAnnotation(*file, id)->name, std::string("Row 000"));
  QCOMPARE(findAnnotation(*file, id)->segments.front().range, original);
}
