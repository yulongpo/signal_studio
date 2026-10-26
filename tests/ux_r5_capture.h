#pragma once

// Include inside ui_capture.cpp's anonymous namespace, AFTER ux_r4_capture.h.
// The translation unit already supplies the Qt/project headers and the native
// screen, IQ import, QWindow input, tree/index and channelsConfirm helpers.
// CLI: --ux-r5 <new-absolute-directory> [--software-renderer]
// This is implementation evidence, never the independent final Review.

QString uxR5CaptureText(const std::string& value) {
  return QString::fromStdString(value);
}

void uxR5CaptureKeys(QWindow* native, const QString& value) {
  // Qt 6.11 has keyClicks(QWidget*), but no keyClicks(QWindow*) overload.
  // These test inputs are ASCII; retain QPA routing for every key as well.
  const auto bytes = value.toLatin1();
  require(native && QString::fromLatin1(bytes) == value,
          "Native key sequence must be ASCII");
  for (const auto key : bytes) QTest::keyClick(native,key);
}

QJsonArray uxR5CaptureIds(const std::vector<std::string>& ids) {
  QJsonArray result;
  for (const auto& id : ids) result.append(uxR5CaptureText(id));
  return result;
}

std::vector<std::string> uxR5CaptureLiveIds(const FileState& file,
                                          AnnotationSource source) {
  std::vector<std::string> result;
  for (const auto& mark : file.marks)
    if (mark.sourceKind == source && annotationInContext(file, mark))
      result.push_back(mark.id);
  return result;
}

QJsonObject uxR5CaptureImmutable(const Annotation& mark) {
  auto result = encodeAnnotation(mark);
  // Selecting a segment is allowed; source geometry/evidence/revisions are not.
  result.remove("activeSegmentId");
  return result;
}

QJsonObject uxR5CaptureSources(const FileState& file) {
  QJsonObject marks, sets;
  for (const auto& mark : file.marks)
    if (mark.sourceKind != AnnotationSource::Manual)
      marks[uxR5CaptureText(mark.id)] = uxR5CaptureImmutable(mark);
  for (const auto& set : file.sourceSets)
    if (set.sourceKind != AnnotationSource::Manual)
      sets[uxR5CaptureText(set.id)] = encodeSourceSet(set);
  return {{"marks", marks}, {"sourceSets", sets}};
}

const char* uxR5CaptureTab(AnnotationSource source) {
  switch (source) {
    case AnnotationSource::ImportedLabel: return "importedLabelsTab";
    case AnnotationSource::Manual: return "manualAnnotationsTab";
    case AnnotationSource::Algorithm: return "detectionResultsTab";
  }
  throw std::runtime_error("Invalid UX-R5 source tab");
}

const char* uxR5CaptureTable(AnnotationSource source) {
  switch (source) {
    case AnnotationSource::ImportedLabel: return "importedLabelTable";
    case AnnotationSource::Manual: return "manualAnnotationTable";
    case AnnotationSource::Algorithm: return "detectionResultTable";
  }
  throw std::runtime_error("Invalid UX-R5 source table");
}

struct UxR5NativeCapture {
  MainWindow& window;
  QDir output;
  bool software;
  QScreen* screen = nullptr;
  QJsonObject report;
  QJsonArray scenes;

  FileState& file() const {
    auto* value = window.session().activeFile();
    require(value, "UX-R5 has no active IQ source");
    return *value;
  }
  AnnotationPanel* panel() const {
    return child<AnnotationPanel>(window, "annotationPanel");
  }
  PlotWidget* plot() const { return child<PlotWidget>(window, "mainPlot"); }
  QJsonObject menuCapabilities(bool editable) const {
    for (const auto* id : {"renameMarkAction","deleteMarksAction"})
      require(child<QAction>(window,id)->isEnabled() == editable,"Global annotation command capability mismatch");
    for (const auto* id : {"renameMark","deleteMarks","treeDeleteMarks"})
      require(child<QPushButton>(window,id)->isEnabled() == editable,"Annotation button capability mismatch");
    auto* menu = plot()->createContextMenu(plot()->plotRect().center().toPoint());
    const bool rename = child<QAction>(*menu,"contextRenameAction")->isEnabled();
    const bool remove = child<QAction>(*menu,"contextDeleteAction")->isEnabled();
    delete menu;
    require(rename == editable && remove == editable,"Graph context command capability mismatch");
    return {{"editable",editable},{"globalAndInspectorActionsChecked",true},
        {"graphMenuActionsChecked",true},{"graphMenuShownHere",false},
        {"treePopupAndDirectCommandCoveredByQtTest",true}};
  }
  QJsonObject nativeMenuPopups(bool editable, const QString& prefix) {
    const auto nativeContextClick = [&](QWidget* target, QPoint point) {
#ifdef Q_OS_WIN
      const auto native = reinterpret_cast<HWND>(window.winId());
      require(waitUntil([&] { SetForegroundWindow(native); return GetForegroundWindow() == native && IsWindowVisible(native); },3000),
          "Context click target is not the visible foreground test window");
      const auto local = target->mapTo(&window,point);
      POINT physical{LONG(std::lround(local.x()*window.devicePixelRatioF())),
                     LONG(std::lround(local.y()*window.devicePixelRatioF()))};
      require(ClientToScreen(native,&physical) && SetCursorPos(physical.x,physical.y),"Cannot position native context click");
      INPUT events[2]{};
      events[0].type = events[1].type = INPUT_MOUSE;
      events[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
      events[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
      require(SendInput(2,events,sizeof(INPUT)) == 2,"Native context click was not delivered");
#else
      Q_UNUSED(target); Q_UNUSED(point);
      require(false,"Native context clicks require Windows");
#endif
    };
    const auto inspect = [&](const char* menuName, const char* renameName,
                             const char* deleteName, const QString& suffix,
                             const std::function<void()>& trigger) {
      bool seen = false;
      QString failure;
      QJsonObject facts;
      QTimer driver;
      QObject::connect(&driver,&QTimer::timeout,&window,[&] {
        auto* menu = window.findChild<QMenu*>(menuName);
        if (!menu || !menu->isVisible() || seen) return;
        seen = true;
        auto* rename = menu->findChild<QAction*>(renameName);
        auto* remove = menu->findChild<QAction*>(deleteName);
        if (!rename || !remove || rename->isEnabled() != editable || remove->isEnabled() != editable)
          failure = "Actual popup command capability mismatch";
        if (menu->screen() != screen || !menu->windowHandle()) failure = "Popup left the native acceptance screen";
        const auto path = output.filePath(prefix+suffix+".png");
        if (!menu->grab().save(path)) failure = "Cannot retain native popup";
        facts = {{"renameEnabled",rename && rename->isEnabled()},
            {"deleteEnabled",remove && remove->isEnabled()}, {"png",path},
            {"actualScreen",uxR3ScreenIdentity(menu->screen())}};
        if (menu->windowHandle()) QTest::keyClick(menu->windowHandle(),Qt::Key_Escape);
        else menu->close();
      });
      driver.start(10);
      trigger();
      const bool opened = waitUntil([&] { return seen; },3000);
      driver.stop();
      require(opened && failure.isEmpty(),"Native menu did not pass: "+QString(menuName)+" "+failure);
      return facts;
    };
    auto* edit = window.menuBar()->actions().at(1)->menu();
    edit->setObjectName("uxR5NativeEditMenu");
    auto global = inspect("uxR5NativeEditMenu","renameMarkAction","deleteMarksAction","-global-menu",[&] {
      const auto point = window.menuBar()->actionGeometry(edit->menuAction()).center();
      QTest::mouseClick(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,window.menuBar()->mapTo(&window,point));
    });
    auto* tree = child<QTreeWidget>(window,"projectTree");
    auto* item = interactionTreeItem(*tree,file().activeMarkId);
    require(item,"Selected source is absent from native menu tree");
    for (auto* parent=item->parent();parent;parent=parent->parent()) parent->setExpanded(true);
    tree->scrollToItem(item);
    tree->setCurrentItem(item,0,QItemSelectionModel::NoUpdate);
    tree->setFocus();
    auto treeFacts = inspect("annotationTreeContextMenu","annotationTreeRenameAction","annotationTreeDeleteAction","-tree-menu",[&] {
      nativeContextClick(tree->viewport(),tree->visualItemRect(item).center());
    });
    plot()->setFocus();
    auto graph = inspect("chartContextMenu","contextRenameAction","contextDeleteAction","-graph-menu",[&] {
      nativeContextClick(plot(),plot()->plotRect().center().toPoint());
    });
    return {{"global",global},{"tree",treeFacts},{"graph",graph},
        {"input","QWindow menu-bar click, Win32 SendInput context clicks to verified foreground test window, native Escape"}};
  }
  QString projectPath() const { return output.filePath("r5-native.ssproj"); }
  QString assetRoot() const {
    return ProjectFileCodec::assetDirectory(projectPath(), window.session().project());
  }
  void checkpoint() {
    report["scenes"] = scenes;
    QFile record(output.filePath("report.json"));
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    require(record.open(QIODevice::WriteOnly) && record.write(bytes) == bytes.size() &&
                record.flush(), "Cannot checkpoint UX-R5 report");
  }
  void step(const QString& message) {
    report["lastStep"] = message;
    QFile log(output.filePath("steps.txt"));
    const auto line = (QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) +
                       " " + message + "\n").toUtf8();
    require(log.open(QIODevice::Append) && log.write(line) == line.size() && log.flush(),
            "Cannot retain UX-R5 diagnostic steps");
    checkpoint();
  }
  void tap(const char* name) {
    step("click " + QString::fromLatin1(name));
    auto* control = child<QWidget>(window, name);
    widebandScrollTo(control);
    widebandNativeClick(control);
  }
  QTableView* tab(AnnotationSource source) {
    auto* button = child<QAbstractButton>(window, uxR5CaptureTab(source));
    auto* table = child<QTableView>(window, uxR5CaptureTable(source));
    if (!button->isChecked() || !table->isVisible()) tap(uxR5CaptureTab(source));
    require(waitUntil([&] { return button->isChecked() && table->isVisible() &&
        panel()->sourceTab() == source; }), "Native source tab did not open");
    return table;
  }
  void pointClick(QWidget* widget, QPoint point,
                  Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    require(widget && widget->isVisible() && widget->rect().contains(point),
            "Native click is outside its visible widget");
    const auto mapped = widget->mapTo(&window, point);
    require(widebandVisibleRect(widget, window).contains(mapped),
            "Native click is clipped by an ancestor");
    auto* hit = window.childAt(mapped);
    require(hit && (hit == widget || widget->isAncestorOf(hit)),
            "Native click is obstructed: " + widget->objectName());
    QTest::mouseClick(window.windowHandle(), Qt::LeftButton, modifiers, mapped);
    QCoreApplication::processEvents();
  }
  void row(AnnotationSource source, const std::string& id,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    auto* table = tab(source);
    // interactionClickRow uses the retired mixed table. Reuse only its ID
    // lookup, then inject input through this actual tab's native viewport.
    auto index = interactionTableIndex(*table, id);
    require(index.isValid(), "Object is absent from its source table: " + uxR5CaptureText(id));
    table->scrollTo(index);
    require(waitUntil([&] {
      index = interactionTableIndex(*table, id);
      return index.isValid() && table->viewport()->rect().contains(table->visualRect(index));
    }), "Source row is not fully exposed");
    const auto view = file().view;
    pointClick(table->viewport(), table->visualRect(index).center(), modifiers);
    require(waitUntil([&] { return file().activeMarkId == id; }),
            "Native row did not select its business ID");
    require(file().view == view, "Ordinary table selection changed the view");
  }
  void type(const char* name, const QString& value, bool enter = true) {
    auto* edit = child<QLineEdit>(window, name);
    widebandScrollTo(edit);
    widebandNativeClick(edit);
    require(edit->hasFocus(), "Native editor did not receive focus");
    QTest::keyClick(window.windowHandle(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(window.windowHandle(), Qt::Key_Backspace);
    uxR5CaptureKeys(window.windowHandle(), value);
    if (enter) QTest::keyClick(window.windowHandle(), Qt::Key_Return);
    QCoreApplication::processEvents();
  }
  void settle() {
    require(waitUntil([&] {
      if (file().metadata.demo || file().metadata.availability.status != LoadStatus::Ready ||
          !window.session().spectrogram(file().metadata.id)) return false;
      for (auto* chart : window.findChildren<PlotWidget*>()) {
        if (!chart->isVisible()) continue;
        if (!chart->isDisplaySettled()) return false;
        const auto stats = chart->renderStatistics();
        if (software) {
          if (!chart->renderingBackend().contains("QPainter") ||
              stats["gpuDataDrawCalls"].toInteger() != 0) return false;
        } else if (!chart->gpuReady() || !chart->renderingBackend().contains("D3D11") ||
                   chart->completedFrameCount() == 0 ||
                   stats["gpuDataDrawCalls"].toInteger() <= 0) return false;
      }
      return software || (plot()->textureUploadCount() > 0 &&
          plot()->renderStatistics()["gpuHeatmapDrawCalls"].toInteger() > 0);
    }, 30000), "Real IQ charts did not settle on the requested renderer");
    QTest::qWait(100);
  }
  QJsonObject state() const {
    QJsonArray sources, objects, overlays, rectangles, charts;
    if (const auto* active = window.session().activeFile()) {
      for (const auto& set : active->sourceSets)
        sources.append(QJsonObject{{"id", uxR5CaptureText(set.id)},
            {"kind", int(set.sourceKind)}, {"runId", uxR5CaptureText(set.runId)},
            {"fileId", uxR5CaptureText(set.fileId)}, {"sampleChannel", int(set.sampleChannel)}});
      for (const auto& mark : active->marks) {
        objects.append(QJsonObject{{"id", uxR5CaptureText(mark.id)},
            {"sourceSetId", uxR5CaptureText(mark.sourceSetId)},
            {"originRecordId", uxR5CaptureText(mark.originRecordId)},
            {"kind", int(mark.sourceKind)}, {"deleted", mark.deleted},
            {"segmentCount", int(mark.segments.size())},
            {"timeUnknown", mark.segments.empty() && bool(mark.observation)}});
        if (annotationOverlayVisible(*active,mark)) overlays.append(uxR5CaptureText(mark.id));
      }
    }
    for (const auto* name : {"mainPlot", "projectTree", "properties", "annotationInspector",
         "importedLabelsTab", "manualAnnotationsTab", "detectionResultsTab", "taskTab", "logTab",
         "importedLabelTable", "manualAnnotationTable", "detectionResultTable",
         "annotationIdentity", "annotationReadOnlyRange", "annotationGeometryEditor",
         "annotationInspectorLocate", "annotationInspectorConvert", "annotationInspectorCreateChannel",
         "convertAnnotationsToManual", "annotationQuickFilter", "currentDetectionStatus"}) {
      auto* widget = window.findChild<QWidget*>(name);
      if (!widget) continue;
      QJsonObject item{{"objectName", name}, {"visible", widget->isVisible()},
          {"enabled", widget->isEnabled()},
          {"rect", rectJson(QRect(widget->mapTo(&window, QPoint()), widget->size()))}};
      if (widget->isVisible()) item["visibleRect"] = rectJson(widebandVisibleRect(widget, window));
      if (auto* table = qobject_cast<QTableView*>(widget)) item["rows"] = table->model()->rowCount();
      rectangles.append(item);
    }
    for (auto* chart : window.findChildren<PlotWidget*>())
      if (chart->isVisible()) charts.append(QJsonObject{{"objectName", chart->objectName()},
          {"backend", chart->renderingBackend()}, {"render", chart->renderStatistics()},
          {"rect", rectJson(QRect(chart->mapTo(&window, QPoint()), chart->size()))}});
    auto* active = window.session().activeFile();
    return {{"fileId", active ? uxR5CaptureText(active->metadata.id) : QString{}},
        {"sampleChannel", active ? int(active->metadata.sampleFormat.selectedChannel) : -1},
        {"activeMarkId", active ? uxR5CaptureText(active->activeMarkId) : QString{}},
        {"activeSegmentId", active ? uxR5CaptureText(active->activeSegmentId) : QString{}},
        {"selectedIds", active ? uxR5CaptureIds(active->selectedMarkIds) : QJsonArray{}},
        {"sourceIds", sources}, {"objects", objects}, {"overlayIds",overlays},
        {"rects", rectangles}, {"charts", charts},
        {"inspectorIdentity",child<QLabel>(window,"annotationIdentity")->text()},
        {"actualScreen", uxR3ScreenIdentity(window.screen())},
        {"fullScreen", window.isFullScreen()}, {"logicalWindowSize", sizeJson(window.size())},
        {"devicePixelRatio", window.devicePixelRatioF()},
        {"backend", plot()->renderingBackend()}, {"render", plot()->renderStatistics()},
        {"plotRect", rectJson(plot()->plotRect().toRect())},
        {"annotationHandleCount", plot()->property("annotationHandleCount").toInt()}};
  }
  void scene(const QString& name, const QStringList& matrix,
             const std::function<QJsonObject()>& action) {
    report["lastScene"] = name;
    const auto index = scenes.size();
    scenes.append(QJsonObject{{"scene", name}, {"pass", false}, {"status", "running"},
        {"matrix", QJsonArray::fromStringList(matrix)}, {"before", state()}});
    step(name + " begin");
    QElapsedTimer sceneTime;
    sceneTime.start();
    try {
      auto facts = action();
      settle();
#ifdef Q_OS_WIN
      RECT nativeClient{};
      require(IsWindowVisible(reinterpret_cast<HWND>(window.winId())) &&
          GetClientRect(reinterpret_cast<HWND>(window.winId()),&nativeClient) &&
          nativeClient.right-nativeClient.left == 3840 && nativeClient.bottom-nativeClient.top == 2160,
          "Native Win32 window is hidden or its physical client is not 4K");
#endif
      require(screen && nativeAcceptanceScreen() == screen && window.screen() == screen &&
          window.windowHandle()->screen() == screen && window.isFullScreen() &&
          window.size() == QSize(2560,1440) &&
          std::abs(window.devicePixelRatioF()-1.5) < .001,
          "Native monitor/fullscreen/DPI changed during UX-R5");
      // QWidget::grab includes the QRhiWidget's native readback. Never use
      // QScreen::grabWindow: DWM may return an old backing image on Windows.
      const auto pixels = window.grab();
      const auto path = output.filePath(name + ".png");
      require(!pixels.isNull() && pixels.size() == QSize(3840,2160) &&
          std::abs(pixels.devicePixelRatioF()-1.5) < .001 && pixels.save(path),
          "Native QWidget/QRhi readback failed");
      auto entry = state();
      entry["scene"] = name;
      entry["matrix"] = QJsonArray::fromStringList(matrix);
      entry["facts"] = facts;
      entry["pass"] = true;
      entry["status"] = "passed";
      entry["png"] = path;
      entry["pixelSize"] = sizeJson(pixels.size());
      entry["captureMethod"] = "native QWidget grab with QRhi readback";
      entry["nativeWindowVisibleAnd4kClientVerified"] = true;
      entry["nativeJourneyElapsedMs"] = sceneTime.elapsed();
      entry["memory"] = finalProcessMemory();
      scenes[index] = entry;
      step(name + " passed");
    } catch (const std::exception& error) {
      auto entry = scenes[index].toObject();
      entry["status"] = "failed";
      entry["error"] = QString::fromUtf8(error.what());
      entry["afterFailure"] = state();
      const auto path = output.filePath(name + "-failed.png");
      if (window.isVisible() && window.grab().save(path)) entry["failedPng"] = path;
      scenes[index] = entry;
      step(name + " failed: " + QString::fromUtf8(error.what()));
      throw;
    }
  }
  QJsonObject durableRun(const std::string& runId) {
    QString error;
    DetectionRun run;
    require(loadDetectionRun(assetRoot(), uxR5CaptureText(runId), run, error, true), error);
    require(run.lifecycle == DetectionLifecycle::Completed && !run.chunks.empty() &&
        run.readFrames > 0 && run.completedWindows > 0 &&
        run.completedWindows == run.eligibleWindows, "Missing complete durable manifest/chunks");
    DetectionPublication publication;
    require(recoverDetectionPublication(assetRoot(), run, publication, error), error);
    // resultCount counts durable protocol records (including an end marker),
    // not necessarily the number of final projected annotations.
    const auto* slot = currentDetectionSlot(file(), file().metadata.sampleFormat.selectedChannel);
    require(slot && slot->committedRunId == runId && !slot->stale && slot->pendingRunId.empty(),
            "Completed publication is not the sole committed slot");
    std::size_t runs = 0, sets = 0, marks = 0;
    for (const auto& current : file().detectionRuns)
      if (current.request.sourceIdentity.selectedSampleChannel == slot->sampleChannel) {
        ++runs; require(current.request.runId == runId, "Old run remains indexed");
      }
    for (const auto& set : file().sourceSets)
      if (set.sourceKind == AnnotationSource::Algorithm && set.sampleChannel == slot->sampleChannel) {
        ++sets; require(set.runId == runId, "Old Algorithm source remains indexed");
      }
    for (const auto& mark : file().marks) {
      if (mark.sourceKind != AnnotationSource::Algorithm) continue;
      const auto* set = findSourceSet(file(), mark.sourceSetId);
      require(set, "Algorithm object has no source set");
      if (set->sampleChannel != slot->sampleChannel) continue;
      ++marks;
      const auto found = std::find_if(publication.annotations.begin(), publication.annotations.end(),
          [&](const auto& original) { return original.id == mark.id; });
      require(found != publication.annotations.end(), "Formal result is absent from durable replay");
      auto actual = uxR5CaptureImmutable(mark);
      auto replay = uxR5CaptureImmutable(*found);
      if (found->revisions.empty()) {
        // Fixture shards have immutable observations, with the initial user
        // revision envelope generated once by publishSourceSet. Its timestamp
        // cannot equal a replay that has not been published. Assert that
        // envelope explicitly, then compare every saved source payload field.
        require(mark.revisions.size() == 1 && mark.revisions.front().action == "source-publication" &&
            mark.revisions.front().actor == "source" && mark.reviewHistory.size() == 1 &&
            mark.reviewRevisionId == mark.revisionId && mark.reviewStatus == ReviewStatus::Pending,
            "Initial fixture publication revision envelope is invalid");
        for (const auto* key : {"revisions", "reviewHistory", "reviewRevisionId", "revisionId", "reviewStatus"}) {
          actual.remove(key);
          replay.remove(key);
        }
      }
      require(set->runId == runId && actual == replay,
          "Formal Algorithm object differs from canonical durable replay");
    }
    require(runs == 1 && sets == 1 && marks == publication.annotations.size(),
            "Current result was not replaced as one complete set");
    auto* tasks = child<QAbstractItemView>(window,"detectionRunList");
    require(waitUntil([&] { return tasks->model()->rowCount() == 1 &&
        tasks->model()->index(0,0).data(Qt::UserRole).toString() == uxR5CaptureText(runId); }),
        "Task UI retained a retired run or lost the current run ID");
    return {{"run", encodeDetectionRun(run)}, {"publicationSourceId", uxR5CaptureText(publication.sourceSet.id)},
        {"canonicalReplayCount", int(publication.annotations.size())},
        {"durableProtocolRecordCount",QString::number(run.resultCount)},
        {"manifestAndChunksVerifiedByProductStore", true}, {"singleAuthoritySlot", true},
        {"taskListRunIds",QJsonArray{uxR5CaptureText(runId)}}};
  }
  DetectionRequest request(const std::string& id, TimeRange time) const {
    DetectionRequest result;
    result.runId = id;
    result.sourceIdentity = annotationSourceIdentity(file().metadata, window.session().projectGeneration());
    const auto* profile = findDetectionProfile("pipeline-fixture-v1");
    require(profile, "Registered pipeline fixture profile is missing");
    result.profile = *profile;
    result.profile.fftSize = 2048;
    result.profile.modelFrames = 2;
    result.profile.modelHopFrames = 1;
    result.parameters.minHits = 1;
    result.parameters.mergeFrequencyHz = 0;
    result.parameters.timeStrategy = DetectionTimeStrategy::None;
    result.requestedRange = fullRange(file().metadata);
    result.requestedRange.time = time;
    result.contextRange = result.requestedRange;
    result.rangeOrigin = "ux-r5-small-system-pipeline-fixture";
    return result;
  }
  QJsonObject pipeline(const std::string& id, TimeRange time) {
    step("start actual pipeline-fixture-v1 worker " + uxR5CaptureText(id));
    QString error;
    auto* detection = child<DetectionPanel>(window, "detectionPanel");
    require(detection->startRequest(request(id,time), error), error);
    require(waitUntil([&] {
      const auto* slot = currentDetectionSlot(file(), file().metadata.sampleFormat.selectedChannel);
      if (slot && slot->committedRunId == id && !slot->stale) return true;
      for (const auto& run : file().detectionRuns)
        if (run.request.runId == id && detectionIsTerminal(run.lifecycle) &&
            run.lifecycle != DetectionLifecycle::Completed)
          throw std::runtime_error("Pipeline fixture failed: " + run.error);
      return false;
    },60000), "Pipeline fixture did not commit a Completed publication");
    window.refresh();
    auto facts = durableRun(id);
    require(!uxR5CaptureLiveIds(file(),AnnotationSource::Algorithm).empty(),
            "Positive pipeline fixture unexpectedly produced no observations");
    facts["execution"] = "actual worker, pipeline-fixture-v1; system integration only";
    facts["modelInferenceVerified"] = false;
    return facts;
  }
};

int runUxR5Capture(const QString& directory, bool software, bool menusOnly = false) {
  QDir output(directory);
  if (!QFileInfo(directory).isAbsolute() || !output.mkpath(".") ||
      QFileInfo::exists(output.filePath("report.json")) ||
      QFileInfo::exists(output.filePath("steps.txt")) ||
      QFileInfo::exists(output.filePath("r5-native.ssproj"))) return 2;
  const auto previousPersistence = qApp->property("uiStatePersistenceEnabled");
  const auto previousSoftware = qApp->property("softwareRenderer");
  qApp->setProperty("uiStatePersistenceEnabled", false);
  qApp->setProperty("softwareRenderer", software);
  struct RestoreUxR5Properties {
    QVariant persistence, renderer;
    ~RestoreUxR5Properties() {
      qApp->setProperty("uiStatePersistenceEnabled", persistence);
      qApp->setProperty("softwareRenderer", renderer);
    }
  } restore{previousPersistence,previousSoftware};
  MainWindow window;
  UxR5NativeCapture capture{window,output,software};
  auto& report = capture.report;
  report = {{"pass",false}, {"mode",menusOnly ? "ux-r5-menus" : "ux-r5"}, {"role","implementation/test harness"},
      {"softwareRenderer",software}, {"platform",QGuiApplication::platformName()},
      {"binary",QCoreApplication::applicationFilePath()}, {"qtVersion",QString::fromLatin1(qVersion())},
      {"startedUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {"modelInferenceVerified",false}, {"fieldRfAccuracyVerified",false},
      {"largeRfbenchValidationExecuted",false}, {"independentReviewPerformed",false},
      {"coverageBoundaries",QJsonArray{
          "One actual imported TorchSig-generated IQ file; no field RF acquisition claim",
          "100 controlled generator TXT source objects; not a large RFbench run",
          "Positive runs use the actual pipeline fixture worker, not SCN/FFSCN model inference",
          "Zero-result replacement uses a controlled durable SCN-profile publication; no model execution",
          "Service write-API denial, legacy Algorithm-bound channels, codec migration, fault injection, multi-file/channel isolation and model accuracy belong to separate tests",
          "Implementation/test evidence only; final independent Review and build execution are external"}}};
  if (menusOnly) report["coverageBoundaries"] = QJsonArray{
      "Targeted menu regression on actual IQ, ordinary labels, Manual and actual fixture-worker sources",
      "Three source kinds; global/tree/graph native popups; no clipboard, DDC or replacement revalidation claim",
      "Full UX-R5 journeys and unchanged subsystem evidence are recorded separately"};
  try {
    capture.step("setup native monitor and isolated .ssproj");
    require(QGuiApplication::platformName() == "windows", "UX-R5 requires native Windows Qt");
    for (const auto* name : {"QT_SCALE_FACTOR","QT_SCREEN_SCALE_FACTORS","QT_FONT_DPI",
         "QT_DEVICE_PIXEL_RATIO","QT_AUTO_SCREEN_SCALE_FACTOR","QT_ENABLE_HIGHDPI_SCALING",
         "QT_SCALE_FACTOR_ROUNDING_POLICY","QT_USE_PHYSICAL_DPI"})
      require(!qEnvironmentVariableIsSet(name), QString("DPI override key is present (even if empty): %1").arg(name));
    QJsonArray screens, candidates;
    for (auto* candidate : QGuiApplication::screens()) {
      screens.append(uxR3ScreenIdentity(candidate));
      if (displayPixelSize(candidate) == QSize(3840,2160) &&
          candidate->geometry().size() == QSize(2560,1440) &&
          std::abs(candidate->devicePixelRatio()-1.5) < .001)
        candidates.append(uxR3ScreenIdentity(candidate));
    }
    report["screens"] = screens;
    report["screenCandidates"] = candidates;
    capture.screen = nativeAcceptanceScreen();
    require(capture.screen && candidates.size() == 1,
            "BLOCKED: exactly one connected actual 4K/150% acceptance monitor is required");
    report["screen"] = uxR3ScreenIdentity(capture.screen);
    require(!report["screen"].toObject()["friendlyName"].toString().isEmpty() &&
        !report["screen"].toObject()["monitorDevicePath"].toString().isEmpty(),
        "BLOCKED: actual Windows monitor identity is unavailable");
    showFullScreenOnScreen(window,capture.screen);
    require(QTest::qWaitForWindowExposed(&window) && waitUntil([&] {
      return window.isFullScreen() && window.screen() == capture.screen &&
          window.size() == QSize(2560,1440);
    },5000), "Native fullscreen window was not exposed on the selected monitor");
    QString error;
    require(window.createProject(output.absolutePath(),"r5-native",&error),error);
    report["projectPath"] = capture.projectPath();
    const auto data = widebandRepresentativeDataPath();
    const auto meta = data.chopped(11)+".sigmf-meta";
    const auto supplemental = data.chopped(11)+".json";
    require(QFileInfo::exists(data) && QFileInfo::exists(meta) && QFileInfo::exists(supplemental),
            "BLOCKED: small ordinary TorchSig IQ/meta/supplemental fixture is unavailable");
    report["source"] = widebandImportActualSource(window,data,meta,capture.screen);
    auto sourceFacts = report["source"].toObject();
    sourceFacts["dataPath"] = data;
    sourceFacts["metaPath"] = meta;
    sourceFacts["supplementalPath"] = supplemental;
    sourceFacts["provenance"] = "TorchSig-generated sample; real file import and CPU DSP, synthetic signal";
    sourceFacts["fieldRfAcquisition"] = false;
    report["source"] = sourceFacts;
    // addImportedSource schedules discovery with singleShot(0), parses on a
    // label worker, then queues publication back to Qt. The shared IQ helper
    // returns as soon as the IQ import closes, before that publication exists.
    // Keep automatic discovery as an acceptance gate: explicit re-import is
    // only checked AFTER success and must never rescue a failed automatic path.
    QJsonObject labelImport{{"mode","automatic queued discovery after actual IQ import"},
        {"expectedAutomaticCount",4},{"countAtIqHelperReturn",
          int(uxR5CaptureLiveIds(capture.file(),AnnotationSource::ImportedLabel).size())},
        {"explicitFallbackAttempted",false},{"timeoutMs",20000}};
    report["labelImport"] = labelImport;
    capture.step("await four actual labels from automatic queued discovery");
    QElapsedTimer discoveryTime;
    discoveryTime.start();
    const bool discovered = waitUntil([&] {
      return uxR5CaptureLiveIds(capture.file(),AnnotationSource::ImportedLabel).size() == 4;
    },20000);
    labelImport["elapsedMs"] = discoveryTime.elapsed();
    labelImport["actualImportedIds"] = uxR5CaptureIds(uxR5CaptureLiveIds(capture.file(),AnnotationSource::ImportedLabel));
    labelImport["manualCountAfterDiscovery"] = int(uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size());
    labelImport["undoCommandsAfterDiscovery"] = int(window.session().annotations().historySize(capture.file()));
    labelImport["sourceState"] = capture.state();
    labelImport["productLog"] = child<QPlainTextEdit>(window,"resultsPanel")->toPlainText();
    report["labelImport"] = labelImport;
    capture.step(discovered ? "automatic discovery published four actual labels" : "automatic label discovery failed or timed out");
    require(discovered,"Automatic discovery must publish all four actual TorchSig labels within 20 seconds; see labelImport source state/product log");
    require(uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).empty() &&
        window.session().annotations().historySize(capture.file()) == 0,
        "Native-parser Imported labels were incorrectly converted to Manual or entered Manual undo history");
    const auto importedBefore = uxR5CaptureSources(capture.file());
    require(window.importLabels(meta,&error,false),error);
    require(uxR5CaptureSources(capture.file()) == importedBefore &&
        window.session().annotations().historySize(capture.file()) == 0,
        "Explicit repeat import duplicated/revised actual labels or added an undo command");
    labelImport["automaticCountVerified"] = 4;
    labelImport["explicitRepeatImportIdempotent"] = true;
    labelImport["sourceImportsOutsideManualUndo"] = true;
    report["labelImport"] = labelImport;
    window.refresh();
    capture.step("automatic label discovery and explicit repeat import verified");
    auto* labelsTable = capture.tab(AnnotationSource::ImportedLabel);
    const auto labels = capture.panel()->sortedVisibleIds();
    require(labels.size() == 4, "Imported table lost ordinary source objects");
    const auto ordinarySourceId = findAnnotation(capture.file(),labels.front())->sourceSetId;
    require(!findSourceSet(capture.file(),ordinarySourceId)->supplementalOriginalText.empty(),
            "Ordinary TorchSig supplemental source was not imported");
    const auto seed = window.session().annotations().createManual(capture.file(),
        findAnnotation(capture.file(),labels.front())->name,
        {{40000,80000},{2495.9e6,2496.1e6}});
    require(seed.ok,uxR5CaptureText(seed.error));
    const auto manualSeed = seed.id;
    window.refresh();
    report["firstPublication"] = capture.pipeline("ux-r5-pipeline-first",{0,400000});
    const auto firstAlgorithms = uxR5CaptureLiveIds(capture.file(),AnnotationSource::Algorithm);
    const auto firstSources = uxR5CaptureSources(capture.file());
    if (menusOnly) {
      const std::array<std::pair<AnnotationSource,std::string>,3> values{{
          {AnnotationSource::ImportedLabel,labels.front()}, {AnnotationSource::Algorithm,firstAlgorithms.front()},
          {AnnotationSource::Manual,manualSeed}}};
      for (const auto& [source,id] : values) {
        const auto name = source == AnnotationSource::ImportedLabel ? QString("menu-imported")
            : source == AnnotationSource::Algorithm ? QString("menu-algorithm") : QString("menu-manual");
        capture.scene(name,{"RD-01","RD-02","RD-04"},[&] {
          capture.row(source,id);
          const auto before = encodeAnnotation(*findAnnotation(capture.file(),id));
          const auto capabilities = capture.menuCapabilities(source == AnnotationSource::Manual);
          const auto popups = capture.nativeMenuPopups(source == AnnotationSource::Manual,name);
          require(encodeAnnotation(*findAnnotation(capture.file(),id)) == before,"Menu checks changed the selected source");
          return QJsonObject{{"sourceId",uxR5CaptureText(id)},{"sourceKind",int(source)},
              {"capabilities",capabilities},{"popups",popups},{"sourceUnchanged",true}};
        });
      }
      require(capture.scenes.size() == 3,"Native menu scene count changed");
      report["menuRegressionVerified"] = true;
      report["fullJourneyExecuted"] = false;
    } else {
    std::string singleCopy, observationCopy;
    std::vector<std::string> bulkCopies, batchIds;
    std::string batchSourceId;
    Channel channel;
    ChannelSampleData firstDdc;
    QJsonObject observationBeforeReplacement;

    capture.scene("01-three-source-tabs",{"UI-01","UI-02"},[&] {
      require(!window.findChild<QWidget*>("annotationSourceFilter") &&
          !window.findChild<QWidget*>("annotationTable") &&
          !window.findChild<QAbstractButton*>("resultTab"), "Retired mixed-source UI is still present");
      QJsonArray tabs;
      for (const auto kind : {AnnotationSource::ImportedLabel,AnnotationSource::Manual,AnnotationSource::Algorithm}) {
        auto* table = capture.tab(kind);
        const auto expected = uxR5CaptureLiveIds(capture.file(),kind);
        require(table->model()->rowCount() == int(expected.size()),"Source row count differs from business objects");
        QJsonArray ids;
        for (int row=0;row<table->model()->rowCount();++row) {
          const auto id = table->model()->index(row,1).data(Qt::UserRole).toString().toStdString();
          const auto* mark = findAnnotation(capture.file(),id);
          require(mark && mark->sourceKind == kind &&
              std::find(expected.begin(),expected.end(),id) != expected.end(),"Cross-tab object contamination");
          ids.append(uxR5CaptureText(id));
        }
        tabs.append(QJsonObject{{"tab",uxR5CaptureTab(kind)}, {"table",table->objectName()},
            {"ids",ids}, {"hit",widebandUxReachable(child<QWidget>(window,uxR5CaptureTab(kind)),window)}});
      }
      capture.tap("taskTab");
      require(child<QWidget>(window,"detectionPanel")->isVisible(),"Task page is inaccessible");
      capture.tap("logTab");
      require(child<QWidget>(window,"resultsPanel")->isVisible(),"Log page is inaccessible");
      capture.tab(AnnotationSource::ImportedLabel);
      return QJsonObject{{"tabs",tabs},{"taskAndLogClicked",true},{"sameNameDifferentSourceIds",true}};
    });

    capture.scene("02-readonly-native-input",{"RD-01","RD-02"},[&] {
      QString unexpectedModal;
      QTimer modalGuard;
      modalGuard.setInterval(60);
      QObject::connect(&modalGuard,&QTimer::timeout,&window,[&] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
          unexpectedModal = dialog->objectName()+" / "+dialog->windowTitle();
          dialog->reject();
        }
      });
      modalGuard.start();
      QJsonArray results;
      for (const auto& id : {labels.front(),firstAlgorithms.front()}) {
        window.session().setView(fullRange(capture.file().metadata)); window.refresh();
        const auto kind = findAnnotation(capture.file(),id)->sourceKind;
        capture.row(kind,id);
        widebandShowInspector(window,"contextAnnotation");
        require(child<QWidget>(window,"annotationReadOnlyRange")->isVisible() &&
            !child<QWidget>(window,"annotationGeometryEditor")->isVisible(),"Read-only inspector exposed editors");
        const auto menuPermissions = capture.menuCapabilities(false);
        capture.settle(); capture.plot()->repaint();
        require(capture.plot()->property("annotationHandleCount").isValid() &&
            capture.plot()->property("annotationHandleCount").toInt() == 0,"Read-only selection has editable handles");
        const auto before = uxR5CaptureSources(capture.file());
        const auto mark = *findAnnotation(capture.file(),id);
        const auto range = mark.segments.empty() ? *mark.observation : activeSegment(mark)->range;
        const auto center = capture.plot()->toPixel((range.time.begin+range.time.end)/2,
            (range.frequency.lowerHz+range.frequency.upperHz)/2).toPoint();
        const auto edge = capture.plot()->toPixel(range.time.end,range.frequency.upperHz).toPoint();
        const auto clamp = [&](QPoint p) {
          const auto box = capture.plot()->plotRect().toRect().adjusted(5,5,-5,-5);
          return QPoint(std::clamp(p.x(),box.left(),box.right()),std::clamp(p.y(),box.top(),box.bottom()));
        };
        uxR3Drag(window,*capture.plot(),clamp(center),clamp(center+QPoint(30,20)));
        uxR3Drag(window,*capture.plot(),clamp(edge),clamp(edge+QPoint(-25,20)));
        capture.row(kind,id);
        QTest::keyClick(window.windowHandle(),Qt::Key_F2);
        QTest::keyClick(window.windowHandle(),Qt::Key_Delete);
        QCoreApplication::processEvents();
        require(unexpectedModal.isEmpty() && !window.findChild<QLineEdit*>("annotationTableEditor") &&
            uxR5CaptureSources(capture.file()) == before,"Readonly drag/F2/Delete changed a source or opened an editor: "+unexpectedModal);
        results.append(QJsonObject{{"id",uxR5CaptureText(id)}, {"handles",0},
            {"centerDrag",pointJson(center)}, {"edgeDrag",pointJson(edge)},
            {"geometryEvidenceRevisionsUnchanged",true},{"f2AndDeleteRejected",true},
            {"menuCapabilities",menuPermissions}});
      }
      require(uxR5CaptureSources(capture.file()) == firstSources,"Read-only source evidence drifted");
      return QJsonObject{{"sources",results},{"serviceApiDenialCoveredHere",false}};
    });

    capture.scene("03-tab-exclusive-overlay-select-locate",{"UI-03","UI-05"},[&] {
      for (const auto* name : {"annotationLayer0","annotationLayer1","annotationLayer2","annotationLayerBar"})
        require(!window.findChild<QWidget*>(name),"Removed layer checkbox/bar still exists");
      QJsonArray sources;
      const auto calculations = capture.plot()->powerGenerationCount();
      for (const auto source : {AnnotationSource::ImportedLabel,AnnotationSource::Manual,AnnotationSource::Algorithm}) {
        capture.tab(source);
        QJsonArray ids;
        for (const auto& mark : capture.file().marks) {
          const bool expected = mark.sourceKind == source && annotationInContext(capture.file(),mark);
          require(annotationOverlayVisible(capture.file(),mark) == expected,"Tab shows another source's overlay");
          if (expected) ids.append(uxR5CaptureText(mark.id));
        }
        sources.append(QJsonObject{{"source",int(source)},{"overlayIds",ids}});
      }
      require(capture.plot()->powerGenerationCount() == calculations,"Source tabs reran spectral analysis");
      capture.row(AnnotationSource::ImportedLabel,labels.front());
      require(annotationOverlayVisible(capture.file(),*findAnnotation(capture.file(),labels.front())) &&
          labelsTable->model()->rowCount() == 4 && capture.file().activeMarkId == labels.front(),
          "Tab did not restore its source overlay/table/selection");
      widebandShowInspector(window,"contextAnnotation");
      window.session().setView(fullRange(capture.file().metadata)); window.refresh();
      const auto before = capture.file().view;
      capture.tap("annotationInspectorLocate");
      require(capture.file().view != before,"Explicit locate failed for the current source tab");
      const auto located = capture.file().view;
      window.session().setView(fullRange(capture.file().metadata)); window.refresh();
      return QJsonObject{{"selectedId",uxR5CaptureText(labels.front())},{"exclusiveSourceOverlays",sources},
          {"layerCheckboxesRemoved",true},{"spectralAnalysisRerun",false},
          {"explicitLocateChangedView",true},{"locatedBegin",QString::number(located.time.begin)},
          {"locatedEnd",QString::number(located.time.end)}};
    });

    capture.scene("04-single-convert-edit-dedup",{"CV-01","CV-07","RD-04"},[&] {
      capture.row(AnnotationSource::ImportedLabel,labels.front());
      const auto before = uxR5CaptureSources(capture.file());
      const auto history = window.session().annotations().historySize(capture.file());
      capture.tap("convertAnnotationsToManual");
      singleCopy = capture.file().activeMarkId;
      const auto* copy = findAnnotation(capture.file(),singleCopy);
      const auto* source = findAnnotation(capture.file(),labels.front());
      require(copy && copy->id != source->id && copy->sourceSetId != source->sourceSetId &&
          copy->sourceKind == AnnotationSource::Manual && copy->conversionSnapshot &&
          copy->conversionSnapshot->sourceAnnotationId == source->id &&
          copy->conversionSnapshot->sourceSegments == source->segments &&
          copy->segments.size() == source->segments.size() && !copy->revisionId.empty() &&
          capture.panel()->sourceTab() == AnnotationSource::Manual &&
          window.session().annotations().historySize(capture.file()) == history+1,
          "Single conversion did not create one independent Manual transaction");
      const auto count = uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size();
      capture.tap("undoAnnotations");
      require(uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size()+1 == count,
              "One undo did not remove the single conversion");
      capture.tap("redoAnnotations");
      capture.row(AnnotationSource::ImportedLabel,labels.front());
      capture.tap("convertAnnotationsToManual");
      require(uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size() == count &&
          window.session().annotations().historySize(capture.file()) == history+1,
          "Repeated conversion was not a no-op");
      capture.row(AnnotationSource::Manual,singleCopy);
      widebandShowInspector(window,"contextAnnotation");
      capture.menuCapabilities(true);
      const auto original = *findAnnotation(capture.file(),singleCopy)->originEvidence;
      const auto segment = *activeSegment(*findAnnotation(capture.file(),singleCopy));
      const auto next = segment.range.time.begin+2000;
      require(next < segment.range.time.end,"Representative segment is too short for native editing");
      capture.type("annotationStartMilliseconds",QString::number(next/capture.file().metadata.sampleRateHz*1000,'g',17),false);
      require(activeSegment(*findAnnotation(capture.file(),singleCopy))->range == segment.range,
              "Typing committed an unfinished Manual geometry draft");
      QTest::keyClick(window.windowHandle(),Qt::Key_Return);
      require(activeSegment(*findAnnotation(capture.file(),singleCopy))->range.time.begin == next,
              "Native Enter did not commit Manual geometry");
      capture.tap("undoAnnotations");
      require(activeSegment(*findAnnotation(capture.file(),singleCopy))->range == segment.range,"Manual geometry undo failed");
      capture.tap("redoAnnotations");
      require(activeSegment(*findAnnotation(capture.file(),singleCopy))->range.time.begin == next &&
          *findAnnotation(capture.file(),singleCopy)->originEvidence == original &&
          uxR5CaptureSources(capture.file()) == before,"Manual editing changed its origin/source");
      return QJsonObject{{"sourceId",uxR5CaptureText(labels.front())},{"manualId",uxR5CaptureText(singleCopy)},
          {"deduplicated",true},{"nativeGeometryEditUndoRedo",true},{"sourceUnchanged",true}};
    });

    capture.scene("05-multiple-convert-selection",{"UI-04","CV-03"},[&] {
      capture.row(AnnotationSource::ImportedLabel,labels[0]);
      capture.row(AnnotationSource::ImportedLabel,labels[2],Qt::ShiftModifier);
      require(capture.file().selectedMarkIds.size() == 3,"Native Shift range did not select exactly three source IDs");
      capture.row(AnnotationSource::ImportedLabel,labels[1]);
      capture.row(AnnotationSource::ImportedLabel,labels[2],Qt::ControlModifier);
      const auto selected = capture.file().selectedMarkIds;
      require(selected.size() == 2 && std::find(selected.begin(),selected.end(),labels[1]) != selected.end() &&
          std::find(selected.begin(),selected.end(),labels[2]) != selected.end(),"Native Ctrl selection IDs differ");
      const auto before = uxR5CaptureSources(capture.file());
      const auto count = uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size();
      const auto history = window.session().annotations().historySize(capture.file());
      capture.tap("convertAnnotationsToManual");
      const auto copies = capture.file().selectedMarkIds;
      require(copies.size() == 2 && uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size() == count+2 &&
          window.session().annotations().historySize(capture.file()) == history+1,"Multi-conversion was not one atomic command");
      capture.tap("undoAnnotations");
      require(uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size() == count,"One undo did not remove the whole batch");
      capture.tap("redoAnnotations");
      require(uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size() == count+2 &&
          uxR5CaptureSources(capture.file()) == before,"Multi-conversion redo/source preservation failed");
      return QJsonObject{{"sourceIds",uxR5CaptureIds(selected)},{"manualIds",uxR5CaptureIds(copies)},
          {"ctrlAndShiftNative",true},{"oneUndoRedoForEntireBatch",true}};
    });

    capture.scene("06-controlled-100-conversion",{"UI-04","CV-03","CV-04","CV-07","QA-03"},[&] {
      // A small, explicitly controlled generator-format label source. These
      // ranges test import/conversion contracts; they are not RF ground truth.
      QString text = "1:\ttime_start: 0.00000\ttime_end: 0.02000\tsig_len: 400000\r\n";
      for (int i=1;i<=100;++i)
        text += QString("\t%1):\tmod: R5_BATCH_QPSK\tsys: QPSK\tFC: %2\tBW: 200000\tFD: 100000\tLEN: 400000\tSPS: 20.00\tCNR: 12.00\tSNR: 12.00\tTIME: 0.00250,0.00850 0.01000,0.01600\r\n")
            .arg(i).arg(11000000+i*10000);
      const auto path = output.filePath("controlled-generator-100.txt");
      QFile labelFile(path);
      const auto bytes = text.toUtf8();
      require(labelFile.open(QIODevice::WriteOnly|QIODevice::NewOnly) &&
          labelFile.write(bytes) == bytes.size() && labelFile.flush(),"Cannot write controlled generator source");
      labelFile.close();
      capture.tab(AnnotationSource::ImportedLabel);
      require(capture.panel()->importLabels(path,&error,false),error);
      window.refresh();
      capture.type("annotationQuickFilter","R5_BATCH_QPSK");
      batchIds = capture.panel()->sortedVisibleIds();
      require(batchIds.size() == 100,"Ordinary generator parser did not import all 100 objects");
      batchSourceId = findAnnotation(capture.file(),batchIds.front())->sourceSetId;
      for (const auto& id : batchIds) {
        const auto* mark = findAnnotation(capture.file(),id);
        require(mark && mark->sourceKind == AnnotationSource::ImportedLabel &&
            mark->sourceSetId == batchSourceId && mark->segments.size() == 2,"Invalid controlled source object");
      }
      capture.row(AnnotationSource::ImportedLabel,batchIds.front());
      QTest::keyClick(window.windowHandle(),Qt::Key_A,Qt::ControlModifier);
      require(capture.file().selectedMarkIds.size() == 100,"Ctrl+A did not select the complete filtered source set");
      const auto before = uxR5CaptureSources(capture.file());
      const auto count = uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size();
      const auto history = window.session().annotations().historySize(capture.file());
      const auto memoryBefore = finalProcessMemory();
      QElapsedTimer conversionTime;
      conversionTime.start();
      capture.tap("convertAnnotationsToManual");
      const auto conversionResponseMs = conversionTime.elapsed();
      bulkCopies = capture.file().selectedMarkIds;
      require(bulkCopies.size() == 100 && uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size() == count+100 &&
          window.session().annotations().historySize(capture.file()) == history+1,"100 conversions were not one complete transaction");
      std::vector<std::string> copiedSources;
      for (const auto& id : bulkCopies) {
        const auto* copy = findAnnotation(capture.file(),id);
        require(copy && copy->sourceKind == AnnotationSource::Manual && copy->conversionSnapshot &&
            copy->sourceSetId != batchSourceId && copy->segments.size() == 2 && !copy->revisionId.empty(),"Invalid bulk Manual copy");
        const auto& snapshot = *copy->conversionSnapshot;
        const auto* source = findAnnotation(capture.file(),snapshot.sourceAnnotationId);
        require(source && std::find(batchIds.begin(),batchIds.end(),source->id) != batchIds.end() &&
            snapshot.sourceSet == *findSourceSet(capture.file(),batchSourceId) && snapshot.sourceSegments == source->segments,
            "Bulk copy lost its self-contained source snapshot");
        require(std::find(copiedSources.begin(),copiedSources.end(),source->id) == copiedSources.end(),"Duplicate source in batch");
        copiedSources.push_back(source->id);
        for (std::size_t i=0;i<2;++i)
          require(copy->segments[i].range == source->segments[i].range &&
              copy->segments[i].timeQuality == source->segments[i].timeQuality &&
              copy->segments[i].frequencyFrame == source->segments[i].frequencyFrame,"Batch changed segment coordinates/quality");
      }
      capture.tap("undoAnnotations");
      require(uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size() == count,"100-object undo left partial copies");
      capture.tap("redoAnnotations");
      require(uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size() == count+100,"100-object redo lost copies");
      capture.tab(AnnotationSource::ImportedLabel);
      capture.row(AnnotationSource::ImportedLabel,batchIds.front());
      QTest::keyClick(window.windowHandle(),Qt::Key_A,Qt::ControlModifier);
      capture.tap("convertAnnotationsToManual");
      require(uxR5CaptureLiveIds(capture.file(),AnnotationSource::Manual).size() == count+100 &&
          window.session().annotations().historySize(capture.file()) == history+1 &&
          uxR5CaptureSources(capture.file()) == before,"100-object dedup changed history or sources");
      capture.type("annotationQuickFilter","");
      return QJsonObject{{"controlledSourcePath",path},{"sourceSetId",uxR5CaptureText(batchSourceId)},
          {"sourceIds",uxR5CaptureIds(batchIds)},{"manualIds",uxR5CaptureIds(bulkCopies)},
          {"count",100},{"atomicUndoRedo",true},{"deduplicated",true},{"largeRfbench",false},
          {"conversionResponseMs",conversionResponseMs},{"memoryBefore",memoryBefore},
          {"memoryAfter",finalProcessMemory()},
          {"timingIncludes","native button dispatch and synchronous UI refresh; separate from chart settling"}};
    });

    capture.scene("07-unknown-observation-conversion",{"CV-02","CV-05"},[&] {
      capture.row(AnnotationSource::Algorithm,firstAlgorithms.front());
      const auto source = *findAnnotation(capture.file(),firstAlgorithms.front());
      require(source.segments.empty() && source.observation,"Pipeline fixture must preserve unknown activity time");
      const auto before = uxR5CaptureSources(capture.file());
      const auto view = capture.file().view;
      capture.tap("convertAnnotationsToManual");
      observationCopy = capture.file().activeMarkId;
      const auto* copy = findAnnotation(capture.file(),observationCopy);
      require(copy && copy->sourceKind == AnnotationSource::Manual && copy->id != source.id &&
          copy->segments.empty() && copy->observation == source.observation &&
          copy->observationTimeQuality == source.observationTimeQuality && copy->conversionSnapshot &&
          copy->conversionSnapshot->sourceObservation == source.observation && capture.file().view == view &&
          uxR5CaptureSources(capture.file()) == before,"Observation conversion invented activity time or changed its source");
      widebandShowInspector(window,"contextAnnotation");
      require(!child<QWidget>(window,"annotationStartMilliseconds")->isEnabled() &&
          !child<QWidget>(window,"annotationEndMilliseconds")->isEnabled() &&
          !child<QPushButton>(window,"annotationInspectorCreateChannel")->isEnabled(),
          "Unknown-time Manual copy acquired an implicit located-segment channel");
      observationBeforeReplacement = uxR5CaptureImmutable(*copy);
      return QJsonObject{{"sourceId",uxR5CaptureText(source.id)},{"manualId",uxR5CaptureText(observationCopy)},
          {"activitySegments",0},{"observationBegin",QString::number(copy->observation->time.begin)},
          {"observationEnd",QString::number(copy->observation->time.end)},{"activityTimeStillUnknown",true}};
    });

    capture.scene("08-cross-view-manual-channel-ddc",{"UI-05","CP-05"},[&] {
      window.session().setView(fullRange(capture.file().metadata)); window.refresh();
      const auto view = capture.file().view;
      auto* tree = child<QTreeWidget>(window,"projectTree");
      auto* item = interactionTreeItem(*tree,labels.front());
      require(item,"Imported source is missing from the tree");
      for (auto* parent=item->parent();parent;parent=parent->parent()) parent->setExpanded(true);
      tree->scrollToItem(item); QCoreApplication::processEvents();
      // The tree grows inside the resources scroll area. Expose this row in
      // that outer viewport, rather than only scrolling the tree itself.
      const auto treePoint = tree->visualItemRect(item).center();
      for (auto* ancestor = tree->parentWidget(); ancestor; ancestor=ancestor->parentWidget())
        if (auto* scroll = qobject_cast<QScrollArea*>(ancestor); scroll && scroll->widget()) {
          const auto point = tree->viewport()->mapTo(scroll->widget(), treePoint);
          scroll->ensureVisible(point.x(), point.y(), 4, 4);
        }
      QCoreApplication::processEvents();
      capture.pointClick(tree->viewport(),tree->visualItemRect(item).center());
      require(capture.file().activeMarkId == labels.front() && capture.file().view == view &&
          capture.panel()->sourceTab() == AnnotationSource::ImportedLabel,"Native tree selection did not link without zoom");
      capture.row(AnnotationSource::Manual,manualSeed);
      const auto seedMark = *findAnnotation(capture.file(),manualSeed);
      window.session().selectMarks({}); window.refresh();
      // The Manual tab alone controls visible/hit-testable overlays.
      const auto target = capture.plot()->toPixel((seedMark.range.time.begin+seedMark.range.time.end)/2,
          (seedMark.range.frequency.lowerHz+seedMark.range.frequency.upperHz)/2).toPoint();
      capture.pointClick(capture.plot(),target);
      require(capture.file().activeMarkId == manualSeed && capture.file().view == view &&
          capture.panel()->sourceTab() == AnnotationSource::Manual,"Native graph selection did not switch source tab without zoom");
      capture.settle(); capture.plot()->repaint();
      require(capture.plot()->property("activeAnnotationId").toString() == uxR5CaptureText(manualSeed) &&
          interactionTableIndex(*capture.tab(AnnotationSource::Manual),manualSeed).isValid(),"Graph/table IDs diverged");
      capture.row(AnnotationSource::Manual,bulkCopies.front());
      widebandShowInspector(window,"contextAnnotation");
      const auto segments = findAnnotation(capture.file(),bulkCopies.front())->segments;
      const auto inspectorView = capture.file().view;
      capture.tap("annotationNextSegment");
      require(capture.file().activeSegmentId == segments[1].id && capture.file().view == inspectorView,
              "Inspector segment selection changed the view or chose a different segment ID");
      capture.tap("annotationPreviousSegment");
      require(capture.file().activeSegmentId == segments[0].id && capture.file().view == inspectorView,
              "Inspector previous segment selection did not preserve the view");
      capture.row(AnnotationSource::Manual,singleCopy);
      widebandShowInspector(window,"contextAnnotation");
      QString modalFailure;
      bool handled = false;
      QTimer dialogDriver;
      QElapsedTimer dialogDeadline;
      dialogDeadline.start();
      dialogDriver.setInterval(100);
      QObject::connect(&dialogDriver,&QTimer::timeout,&window,[&] {
        if (dialogDeadline.elapsed() > 10000) {
          modalFailure = "Native channel dialog did not appear within 10 seconds";
          dialogDriver.stop();
          if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) modal->reject();
          return;
        }
        auto* dialog = window.findChild<QDialog*>("channelConfigDialog");
        if (!dialog || !dialog->isVisible() || handled) return;
        handled = true; dialogDriver.stop();
        try {
          dialog = channelsDialog(window,capture.screen);
          const auto path = output.filePath("08-manual-channel-dialog.png");
          require(dialog->grab().save(path),"Cannot retain native channel configuration");
          channel = channelsConfirm(window,*dialog,false);
          report["channelDialogScreenshot"] = path;
        } catch (const std::exception& failure) {
          modalFailure = QString::fromUtf8(failure.what()); dialog->reject();
        }
      });
      dialogDriver.start();
      capture.tap("annotationInspectorCreateChannel");
      require(waitUntil([&] { return !channel.id.empty() || !modalFailure.isEmpty(); },10000) &&
          modalFailure.isEmpty() && channel.sourceSnapshot &&
          channel.sourceSnapshot->sourceKind == AnnotationSource::Manual &&
          channel.sourceSnapshot->annotationId == singleCopy,"Manual channel dialog failed: "+modalFailure);
      dialogDriver.stop();
      ChannelDspPlan plan;
      require(makeChannelDspPlan(capture.file().metadata,channel,plan,error) &&
          processChannelSamples(capture.file().metadata,channel,plan,{0,512},firstDdc) &&
          firstDdc.samples.size() == 512 && firstDdc.outputSamples == TimeRange{0,512} &&
          firstDdc.sourceSamples.begin >= channel.sourceTime.begin &&
          firstDdc.sourceSamples.end <= channel.sourceTime.end &&
          firstDdc.sourceSamples.end <= availableSamples(capture.file().metadata),"Actual CPU DDC512 failed: "+error);
      double energy = 0;
      for (const auto value : firstDdc.samples) {
        require(std::isfinite(value.real()) && std::isfinite(value.imag()),"Non-finite real DDC output");
        energy += std::norm(value);
      }
      require(energy > 0,"Actual DDC output has zero energy");
      report["cpuDdcOutputSamples"] = 512;
      report["channelId"] = uxR5CaptureText(channel.id);
      return QJsonObject{{"treeSourceId",uxR5CaptureText(labels.front())},{"graphManualId",uxR5CaptureText(manualSeed)},
          {"channelId",uxR5CaptureText(channel.id)},{"snapshotAnnotationId",uxR5CaptureText(singleCopy)},
          {"cpuDdcOutputSamples",512},{"energy",energy},{"ordinarySelectionZoomed",false},
          {"inspectorSegmentSelectionZoomed",false},
          {"legacyAlgorithmBoundChannelVerifiedHere",false}};
    });

    const auto resizeInspector = [&](int target) {
      auto* properties = child<QWidget>(window,"properties");
      auto* handle = child<QWidget>(window,"rightPanelResize");
      widebandNativeReachable(handle);
      const auto start = handle->mapTo(&window,handle->rect().center());
      const auto end = start+QPoint(properties->width()-target,0);
      QTest::mousePress(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,start);
      for (int i=1;i<=8;++i) QTest::mouseMove(window.windowHandle(),start+(end-start)*i/8,12);
      QTest::mouseRelease(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,end);
      require(waitUntil([&] { return properties->width() == target; }),"Actual sidebar resize did not reach requested width");
      child<QScrollArea>(window,"propScroll")->verticalScrollBar()->setValue(0);
      QCoreApplication::processEvents();
    };
    const auto inspectorGeometry = [&] {
      auto* inspector = child<QWidget>(window,"annotationInspector");
      QJsonArray controls;
      std::vector<QWidget*> fields;
      for (auto* widget : inspector->findChildren<QWidget*>()) {
        if (!widget->isVisible() || widget->window() != &window ||
            (!qobject_cast<QLabel*>(widget) && !qobject_cast<QAbstractButton*>(widget) &&
             !qobject_cast<QLineEdit*>(widget) && !qobject_cast<QComboBox*>(widget))) continue;
        const QRect rect(widget->mapTo(inspector,QPoint()),widget->size());
        require(rect.left() >= 0 && rect.right() < inspector->width(),
                "Inspector horizontal overflow: "+widget->objectName());
        if (auto* label = qobject_cast<QLabel*>(widget); label && label->wordWrap() && !label->text().isEmpty())
          require(label->height() >= label->heightForWidth(label->width()),"Inspector wrapped text is clipped");
        for (auto* previous : fields)
          if (!widget->isAncestorOf(previous) && !previous->isAncestorOf(widget))
            require(!rect.intersects(QRect(previous->mapTo(inspector,QPoint()),previous->size())),
                    "Inspector controls overlap: "+widget->objectName()+" / "+previous->objectName());
        fields.push_back(widget);
        controls.append(QJsonObject{{"objectName",widget->objectName()},{"rect",rectJson(rect)},
            {"enabled",widget->isEnabled()}});
      }
      QJsonArray hits;
      for (const auto* name : {"annotationInspectorLocate","annotationInspectorCreateChannel","annotationStartMilliseconds",
           "annotationEndMilliseconds","annotationLowerMHz","annotationUpperMHz"}) {
        auto* widget = child<QWidget>(window,name);
        widebandScrollTo(widget);
        hits.append(widebandUxReachable(widget,window));
        if (qobject_cast<QAbstractButton*>(widget)) require(widget->height() >= 28,"Inspector action hit target is too short");
      }
      const auto before = capture.file().view;
      capture.tap("annotationInspectorLocate");
      require(capture.file().activeMarkId == singleCopy,"Inspector locate acted on a different object");
      window.session().setView(before); window.refresh();
      return QJsonObject{{"propertyWidth",child<QWidget>(window,"properties")->width()},
          {"controls",controls},{"nativeHits",hits},{"locateClicked",true}};
    };
    capture.scene("09-long-name-width-320",{"UI-06"},[&] {
      capture.row(AnnotationSource::Manual,singleCopy);
      const auto name = QString("UXR5_LONG_NAME_")+QString(60,'L')+"_END";
      const auto before = uxR5CaptureSources(capture.file());
      QTest::keyClick(window.windowHandle(),Qt::Key_F2);
      auto* editor = child<QLineEdit>(window,"annotationTableEditor");
      require(editor->isVisible() && editor->hasFocus(),"F2 did not open the actual Manual name editor");
      QTest::keyClick(window.windowHandle(),Qt::Key_A,Qt::ControlModifier);
      uxR5CaptureKeys(window.windowHandle(),name);
      QTest::keyClick(window.windowHandle(),Qt::Key_Return);
      require(findAnnotation(capture.file(),singleCopy)->name == name.toStdString() &&
          uxR5CaptureSources(capture.file()) == before,"Native Manual rename failed or renamed its source");
      widebandShowInspector(window,"contextAnnotation");
      resizeInspector(320);
      return inspectorGeometry();
    });
    capture.scene("10-long-name-width-650",{"UI-06"},[&] {
      resizeInspector(650); return inspectorGeometry();
    });

    const auto copyEvidence = [&](QWidget* entry, const std::string& id, bool supplement,
                                   const QString& filename) {
      const auto before = uxR5CaptureSources(capture.file());
      const auto* mark = findAnnotation(capture.file(),id);
      require(mark,"Evidence copy object missing");
      const auto* source = findSourceSet(capture.file(),mark->sourceSetId);
      require(source,"Evidence copy source missing");
      const auto primary = QString::fromUtf8(source->originalText);
      const auto extra = QString::fromUtf8(source->supplementalOriginalText);
      const auto hash = uxR5CaptureText(supplement ? source->supplementalHash : source->sourceHash);
      const auto originalPath = uxR5CaptureText(source->originalPath);
      require(!primary.isEmpty() && !hash.isEmpty() && (!supplement || !extra.isEmpty()),"Required source evidence is empty");
      auto* toggle = child<QToolButton>(*entry,"annotationEvidenceDetailsToggle");
      widebandScrollTo(toggle); widebandNativeClick(toggle);
      auto* dialog = child<QDialog>(*entry,"annotationEvidenceDetailsDialog");
      require(waitUntil([&] { return dialog->isVisible() && dialog->windowHandle(); }),"Nonmodal evidence details did not open");
      require(!dialog->isModal() && dialog->screen() == capture.screen &&
          std::abs(dialog->devicePixelRatioF()-1.5) < .001,"Evidence dialog left the actual screen or became modal");
      QApplication::clipboard()->setText("UXR5_COPY_SENTINEL");
      widebandNativeClick(child<QPushButton>(*dialog,"annotationCopyEvidence"));
      QString copied;
      require(waitUntil([&] {
        copied = QApplication::clipboard()->text();
        return copied.contains(primary) && copied.contains(originalPath) && copied.contains(hash) &&
            (!supplement || copied.contains(extra));
      },5000),"System clipboard did not contain the complete original source/evidence");
      const auto* rawToggleName = entry->property("evidenceMode").toString() == "auxiliary"
          ? "auxiliaryOriginalFieldsToggle" : "annotationOriginalFieldsToggle";
      auto* rawToggle = child<QToolButton>(*dialog,rawToggleName);
      if (!rawToggle->isChecked()) { widebandScrollTo(rawToggle); widebandNativeClick(rawToggle); }
      auto* search = child<QLineEdit>(*dialog,"annotationEvidenceSearch");
      widebandScrollTo(search); widebandNativeClick(search);
      QTest::keyClick(dialog->windowHandle(),Qt::Key_A,Qt::ControlModifier);
      const auto needle = supplement ? QString("n_events") : QString("time_start:");
      uxR5CaptureKeys(dialog->windowHandle(),needle);
      QTest::keyClick(dialog->windowHandle(),Qt::Key_Return);
      const auto raw = dialog->findChildren<QPlainTextEdit*>();
      require(std::any_of(raw.begin(),raw.end(),[&](const auto* text) {
        return text->textCursor().selectedText() == needle;
      }),"Full evidence search did not expose the original text");
      const auto path = output.filePath(filename);
      require(dialog->grab().save(path),"Cannot retain full evidence dialog readback");
      dialog->close();
      require(uxR5CaptureSources(capture.file()) == before,"Evidence disclosure/copy mutated immutable source records");
      return QJsonObject{{"objectId",uxR5CaptureText(id)},{"sourceSetId",uxR5CaptureText(mark->sourceSetId)},
          {"characters",copied.size()},{"completePrimary",true},{"completeSupplemental",supplement},
          {"sourceHashAndPathPresent",true},{"searchMatch",needle},{"png",path},
          {"actualScreen",uxR3ScreenIdentity(dialog->screen())}};
    };
    capture.scene("11-width-390-full-evidence-copy",{"UI-06","QA-02","QA-03"},[&] {
      resizeInspector(390);
      const auto geometry = inspectorGeometry();
      capture.row(AnnotationSource::ImportedLabel,batchIds.front());
      widebandShowInspector(window,"contextAnnotation");
      auto* inspector = child<QWidget>(window,"annotationInspector");
      auto primary = copyEvidence(child<QWidget>(*inspector,"annotationEvidenceWidget"),batchIds.front(),false,
                                  "11-complete-generator-evidence.png");
      require(primary["characters"].toInteger() > 16000,"Primary copy did not exercise bounded summary versus full source");
      capture.row(AnnotationSource::ImportedLabel,labels.front());
      auto supplement = copyEvidence(child<QWidget>(*inspector,"annotationEvidenceWidget"),labels.front(),true,
                                     "11-ordinary-supplemental-inspector.png");
      capture.tap("detectionEvidenceMode");
      auto auxiliary = copyEvidence(child<QWidget>(window,"auxiliaryDetectionEvidence"),labels.front(),true,
                                    "11-ordinary-supplemental-auxiliary.png");
      capture.tap("waveMode");
      capture.row(AnnotationSource::Manual,singleCopy);
      report["fullCopyCharacters"] = primary["characters"];
      report["ordinarySupplementalEntriesVerified"] = 2;
      return QJsonObject{{"geometry",geometry},{"primary",primary},{"supplementalInspector",supplement},
          {"supplementalAuxiliary",auxiliary},{"uxR4FullEvidencePreserved",true}};
    });

    capture.scene("12-configuration-results-stale",{"RP-04","RP-05"},[&] {
      widebandShowInspector(window,"contextDetection");
      auto* model = child<QComboBox>(window,"detectionModel");
      const auto oldValue = model->currentData();
      int target = model->findData("scn-max-psd-v1");
      if (target == model->currentIndex()) target = model->findData("ffscn-gui-iq-v1");
      widebandNativeSelect(model,target);
      require(model->currentData() != oldValue,"Native configuration change did not apply");
      const auto* slot = currentDetectionSlot(capture.file(),capture.file().metadata.sampleFormat.selectedChannel);
      require(slot && slot->stale && slot->committedRunId == "ux-r5-pipeline-first", "Committed configuration did not mark old results stale");
      capture.tab(AnnotationSource::Algorithm);
      require(child<QLabel>(window,"currentDetectionStatus")->text().contains("待更新") &&
          child<QTableView>(window,"detectionResultTable")->model()->rowCount() == 0 &&
          !child<QPushButton>(window,"convertAnnotationsToManual")->isEnabled(),"Stale result is still presented as a current convertible result");
      return QJsonObject{{"changedUiModel",model->currentData().toString()},{"oldRunRetainedOnlyForRollback",true},
          {"statusText",child<QLabel>(window,"currentDetectionStatus")->text()},
          {"pendingPreviewPublicationCoveredHere",false},{"failedCancelledWorkerCoveredHere",false}};
    });
    capture.scene("13-new-roi-publication-replaces",{"RP-01","RP-08","CV-02","CV-08"},[&] {
      const auto facts = capture.pipeline("ux-r5-pipeline-replacement",{200000,280000});
      for (const auto& id : firstAlgorithms)
        require(!findAnnotation(capture.file(),id),"Previous run's Algorithm object survived replacement");
      const auto* copy = findAnnotation(capture.file(),observationCopy);
      require(copy && uxR5CaptureImmutable(*copy) == observationBeforeReplacement,"Replacement changed independent Manual observation copy");
      capture.row(AnnotationSource::Manual,observationCopy);
      widebandShowInspector(window,"contextAnnotation");
      const auto old = *copy->observation;
      const auto lower = old.frequency.lowerHz+1000;
      capture.type("annotationLowerMHz",QString::number(lower/1e6,'g',17));
      copy = findAnnotation(capture.file(),observationCopy);
      require(copy->sourceKind == AnnotationSource::Manual && copy->segments.empty() &&
          copy->observation->time == old.time && std::abs(copy->observation->frequency.lowerHz-lower) < .01 &&
          copy->conversionSnapshot->sourceKind == AnnotationSource::Algorithm,
          "Manual observation could not be edited independently after source retirement");
      observationBeforeReplacement = uxR5CaptureImmutable(*copy);
      capture.tab(AnnotationSource::Algorithm);
      auto result = facts;
      result["oldAlgorithmIdsAbsent"] = uxR5CaptureIds(firstAlgorithms);
      result["manualCopyEditedAfterRetirement"] = uxR5CaptureText(observationCopy);
      result["outsideRoi"] = "not detected, not a negative result";
      return result;
    });
    capture.scene("14-durable-zero-publication",{"RP-03","RP-10"},[&] {
      // Controlled durable publication, deliberately not a model/worker run.
      // Use the production SCN-profile replay format, including a real final
      // zero-count chunk and complete coverage. Do not hand-insert marks.
      const auto oldIds = uxR5CaptureLiveIds(capture.file(),AnnotationSource::Algorithm);
      DetectionRun run;
      run.request = capture.request("ux-r5-controlled-zero",{200000,280000});
      run.request.profile = *findDetectionProfile("scn-max-psd-v1");
      run.request.profile.modelFrames = run.request.profile.modelHopFrames = 1;
      run.request.parameters.timeStrategy = DetectionTimeStrategy::None;
      run.lifecycle = DetectionLifecycle::Running;
      run.startedAtMilliseconds = std::uint64_t(QDateTime::currentMSecsSinceEpoch());
      run.readFrames = run.eligibleWindows = run.completedWindows = 1;
      run.coverage = {{DetectionCoverageKind::Read,run.request.contextRange,"controlled durable zero fixture"},
          {DetectionCoverageKind::Eligible,run.request.requestedRange,""},
          {DetectionCoverageKind::Completed,run.request.requestedRange,""}};
      require(appendDetectionChunk(capture.assetRoot(),run,
          QJsonArray{QJsonObject{{"kind","frequency-publication-end-v1"},{"count","0"}}},{},error),error);
      run.lifecycle = DetectionLifecycle::Completed;
      run.computationPhase = "completed";
      run.finishedAtMilliseconds = run.startedAtMilliseconds+1;
      require(writeDetectionRun(capture.assetRoot(),run,error),error);
      DetectionPublication publication;
      require(recoverDetectionPublication(capture.assetRoot(),run,publication,error) && publication.annotations.empty(),error);
      Project candidate = window.session().project();
      DetectionResultTicket ticket;
      require(DetectionResultService::beginDetection(candidate,run.request,window.session().projectGeneration(),ticket,error),error);
      auto pending = std::find_if(candidate.files.begin(),candidate.files.end(),[&](const auto& file) {
        return file.metadata.id == ticket.fileId;
      });
      require(pending != candidate.files.end(),"Controlled pending source disappeared");
      pending->detectionRuns.push_back(run);
      require(ProjectFileCodec::save(capture.projectPath(),candidate,error),error);
      window.session().project() = std::move(candidate);
      const auto committed = DetectionResultService::replaceCurrentDetection(window.session().project(),
          window.session().annotations(),ticket,window.session().projectGeneration(),run,
          std::move(publication),capture.projectPath());
      require(committed.committed,committed.error);
      window.refresh();
      const auto facts = capture.durableRun(run.request.runId);
      capture.tab(AnnotationSource::Algorithm);
      require(child<QTableView>(window,"detectionResultTable")->model()->rowCount() == 0 &&
          child<QLabel>(window,"currentDetectionStatus")->text().contains("未检出"),"Zero result did not clear the old formal table");
      for (const auto& id : oldIds) require(!findAnnotation(capture.file(),id),"Zero publication left an old Algorithm object");
      auto result = facts;
      result["execution"] = "controlled Completed manifest + zero publication chunk + production replay/transaction; no model inference";
      result["removedIds"] = uxR5CaptureIds(oldIds);
      result["statusText"] = child<QLabel>(window,"currentDetectionStatus")->text();
      return result;
    });

    capture.scene("15-save-reopen-no-old-run",{"RP-10","CV-08","CP-05"},[&] {
      capture.step("save through MainWindow .ssproj codec");
      // The historical channelsSave helper preflights legacy JSON in a new
      // directory without this project's detection assets. Exercise the real
      // .ssproj action directly, retaining any modal error instead of hanging.
      QString modalFailure;
      QTimer unexpectedModal;
      QObject::connect(&unexpectedModal,&QTimer::timeout,&window,[&] {
        if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
          modalFailure = message->windowTitle()+": "+message->text();
          message->reject();
        }
      });
      unexpectedModal.start(25);
      const bool saved = window.saveProject(capture.projectPath());
      require(saved,"MainWindow save failed: "+modalFailure);
      const auto generation = window.session().projectGeneration();
      const bool opened = window.openProject(capture.projectPath());
      require(opened,"MainWindow could not reopen the saved native .ssproj: "+modalFailure);
      unexpectedModal.stop();
      require(window.session().projectGeneration() > generation,"Reopen did not replace the session generation");
      capture.settle();
      const auto facts = capture.durableRun("ux-r5-controlled-zero");
      const auto discovered = discoverDetectionRuns(capture.assetRoot(),window.session().project(),error);
      require(error.isEmpty() && discovered.size() == 1 &&
          discovered.front().request.runId == "ux-r5-controlled-zero","Recovery/discovery resurrected a retired run: "+error);
      require(uxR5CaptureLiveIds(capture.file(),AnnotationSource::Algorithm).empty(),"Reopen resurrected Algorithm objects");
      auto* tree = child<QTreeWidget>(window,"projectTree");
      for (const auto& id : firstAlgorithms) require(!interactionTreeItem(*tree,id),"Tree resurrected old run objects");
      require(findAnnotation(capture.file(),observationCopy) &&
          uxR5CaptureImmutable(*findAnnotation(capture.file(),observationCopy)) == observationBeforeReplacement,
          "Reopen lost the independent converted Algorithm snapshot");
      for (const auto& id : bulkCopies) require(findAnnotation(capture.file(),id) &&
          annotationInContext(capture.file(),*findAnnotation(capture.file(),id)) &&
          findAnnotation(capture.file(),id)->conversionSnapshot,"Reopen lost a member of the 100-object batch");
      const auto found = std::find_if(capture.file().channels.begin(),capture.file().channels.end(),[&](const auto& value) {
        return value.id == channel.id;
      });
      require(found != capture.file().channels.end() && found->sourceSnapshot &&
          *found->sourceSnapshot == *channel.sourceSnapshot && found->sourceTime == channel.sourceTime &&
          found->centerFrequencyHz == channel.centerFrequencyHz && found->bandwidthHz == channel.bandwidthHz &&
          found->outputSampleRateHz == channel.outputSampleRateHz,"Reopen changed the Manual channel/DSP creation snapshot");
      ChannelDspPlan plan;
      ChannelSampleData reopened;
      require(makeChannelDspPlan(capture.file().metadata,*found,plan,error) &&
          processChannelSamples(capture.file().metadata,*found,plan,{0,512},reopened) &&
          reopened.samples == firstDdc.samples,"Reopen changed the actual 512 CPU DDC samples: "+error);
      require(QFileInfo::exists(data) && QFileInfo::exists(meta) && QFileInfo::exists(supplemental),"External source assets were removed");
      capture.tab(AnnotationSource::Algorithm);
      require(child<QTableView>(window,"detectionResultTable")->model()->rowCount() == 0 &&
          child<QLabel>(window,"currentDetectionStatus")->text().contains("未检出"),"Reopened zero-result UI is incorrect");
      report["saveReopenVerified"] = true;
      report["noRetiredRunDiscovery"] = true;
      report["manualChannelSnapshotReopened"] = true;
      auto result = facts;
      result["discoveredRunIds"] = QJsonArray{"ux-r5-controlled-zero"};
      result["bulkManualCopiesReopened"] = int(bulkCopies.size());
      result["cpuDdcOutputSamples"] = int(reopened.samples.size());
      result["ddcSamplesEqualWithoutHashing"] = true;
      result["legacyAlgorithmBoundChannelVerifiedHere"] = false;
      return result;
    });
    require(capture.scenes.size() == 15,"Core UX-R5 scene count changed unexpectedly");
    }
    report["pass"] = true;
  } catch (const std::exception& error) {
    report["error"] = QString::fromUtf8(error.what());
    report["pass"] = false;
    report["status"] = QString::fromUtf8(error.what()).startsWith("BLOCKED:") ? "blocked" : "failed";
  }
  report["finishedUtc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  if (report["pass"].toBool()) report["status"] = "passed";
  try { capture.checkpoint(); } catch (const std::exception&) { return 2; }
  QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
  return report["pass"].toBool() ? 0 : 1;
}
