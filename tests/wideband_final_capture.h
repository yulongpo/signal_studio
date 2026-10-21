#pragma once

// Extends the existing native Capture journey. Synthetic pressure records are
// deliberately separate from the real IQ/model evidence collected before it.

void finalClick(MainWindow& window, const char* name) {
  const auto action = QString::fromLatin1(name);
  if (action == "acceptAnnotations" || action == "rejectAnnotations" ||
      action == "clearAnnotationReview") {
    widebandChooseQuality(window, name);
    return;
  }
  auto* widget = action.startsWith("detection")
                     ? widebandDetectionControl(window, name)
                     : child<QWidget>(window, name);
  if (action.contains("Annotation") || action == "extract")
    widebandShowInspector(window, "contextAnnotation");
  widebandScrollTo(widget);
  widebandNativeClick(widget);
}

QJsonObject finalProcessMemory() {
  QJsonObject result;
#ifdef Q_OS_WIN
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  require(GetProcessMemoryInfo(
              GetCurrentProcess(),
              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
              sizeof(counters)),
          "Cannot measure native UI process memory");
  result = {{"pid", static_cast<qint64>(GetCurrentProcessId())},
            {"rssBytes", static_cast<qint64>(counters.WorkingSetSize)},
            {"peakRssBytes", static_cast<qint64>(counters.PeakWorkingSetSize)},
            {"privateBytes", static_cast<qint64>(counters.PrivateUsage)}};
#endif
  return result;
}

QJsonObject finalLatencies(std::vector<double> milliseconds,
                           bool includesChartFrame = true) {
  require(!milliseconds.empty(), "Latency measurement is empty");
  const QJsonArray samples = [&] {
    QJsonArray values;
    for (double value : milliseconds) values.append(value);
    return values;
  }();
  std::sort(milliseconds.begin(), milliseconds.end());
  auto percentile = [&](double p) {
    return milliseconds[static_cast<std::size_t>(
        std::ceil(p * milliseconds.size()) - 1)];
  };
  return {{"samplesMilliseconds", samples},
          {"p50Milliseconds", percentile(.50)},
          {"p95Milliseconds", percentile(.95)},
          {"includesFirstPaint", true},
          {"includesChartFrame", includesChartFrame},
          {"targetMilliseconds", 200},
          {"targetMet", percentile(.95) <= 200}};
}

void finalExport(MainWindow& window, const char* buttonName,
                 const QString& path) {
  bool dialogAccepted = false;
  QTimer timer;
  timer.setInterval(20);
  QObject::connect(&timer, &QTimer::timeout, [&] {
    auto* dialog =
        qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
    if (!dialog) return;
    dialog->selectFile(path);
    dialogAccepted =
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
    timer.stop();
  });
  timer.start();
  finalClick(window, buttonName);
  timer.stop();
  require(dialogAccepted && QFileInfo(path).size() > 0,
          "Visible file-dialog export did not write the requested file");
}

QJsonObject finalPressure(Capture& capture, QScreen* screen,
                          const QString& savedProject) {
  auto& window = capture.window;
  auto& session = window.session();
  auto* file = session.activeFile();
  require(file && file->metadata.sampleCount >= 400000,
          "Pressure fixture requires the actual 400000-sample background");
  const auto beforeMemory = finalProcessMemory();
  session.annotations().clear();
  file->marks.clear();
  file->sourceSets.clear();
  file->selectedMarkIds.clear();
  file->activeMarkId.clear();
  file->activeSegmentId.clear();
  AnnotationSourceSet source;
  source.id = "m5-synthetic-pressure-source";
  source.fileId = file->metadata.id;
  source.sourceKind = AnnotationSource::Manual;
  source.formatId = "manual-v1";
  source.parserId = "m5-synthetic-pressure";
  source.parserVersion = "1";
  source.originalText = "SYNTHETIC UI PRESSURE: 1000 objects x 100 segments";
  source.sourceIdentity =
      annotationSourceIdentity(file->metadata, session.projectGeneration());
  std::vector<Annotation> annotations;
  annotations.reserve(1000);
  for (int row = 0; row < 1000; ++row) {
    Annotation annotation;
    annotation.id = "pressure-object-" + std::to_string(row);
    annotation.name = "SYNTHETIC pressure " + std::to_string(row);
    annotation.fileId = source.fileId;
    annotation.sourceSetId = source.id;
    annotation.originRecordId = std::to_string(row);
    annotation.sourceKind = AnnotationSource::Manual;
    for (int segment = 0; segment < 100; ++segment) {
      const auto begin = static_cast<SampleIndex>((row * 100 + segment) * 4);
      annotation.segments.push_back(
          {"segment-" + std::to_string(segment),
           {{begin, begin + 2},
            {file->metadata.centerFrequencyHz - 100000,
             file->metadata.centerFrequencyHz + 100000}}});
    }
    auto origin = std::make_shared<OriginEvidence>();
    origin->formatId = source.formatId;
    origin->originRef = annotation.originRecordId;
    origin->originalSegments = annotation.segments;
    annotation.originEvidence = std::move(origin);
    annotations.push_back(std::move(annotation));
  }
  QElapsedTimer setup;
  setup.start();
  const auto published = session.annotations().publishSourceSet(
      *file, source, std::move(annotations));
  require(published.ok, QString::fromStdString(published.error));
  session.setView({{0, 20000},
                   {file->metadata.centerFrequencyHz - 10000000,
                    file->metadata.centerFrequencyHz + 10000000}},
                  false);
  window.refresh();
  require(widebandDisplayReady(window), "Pressure display did not settle");
  const auto setupMs = setup.elapsed();
  click(window, "resultTab");
  auto* table = child<QTableView>(window, "annotationTable");
  auto* filter = child<QComboBox>(window, "annotationSourceFilter");
  require(table->model()->rowCount() == 200 &&
              table->findChildren<QWidget*>().size() < 100,
          "Pressure table must page data without one widget per activity");
  std::vector<double> selections, filters;
  for (int attempt = 0; attempt < 20; ++attempt) {
    const auto before = capture.plots[2]->completedFrameCount();
    const auto index = table->model()->index(attempt % 4, 1);
    table->scrollTo(index);
    QElapsedTimer elapsed;
    elapsed.start();
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                      table->visualRect(index).center(), 0);
    require(
        waitUntil(
            [&] { return capture.plots[2]->completedFrameCount() > before; },
            10000),
        "Pressure selection did not paint the selected chart overlay");
    selections.push_back(elapsed.nsecsElapsed() / 1000000.0);
    // Start at the real row activation, excluding the user's time opening and
    // navigating the native popup. Keep the original first-paint measurement.
    widebandNativeSelect(filter, attempt % 2 ? 0 : 2,
                          [&] { elapsed.restart(); });
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    table->viewport()->repaint();
    filters.push_back(elapsed.nsecsElapsed() / 1000000.0);
  }
  widebandNativeSelect(filter, 0);
  widebandScene(capture, "41-synthetic-100000-activity-pressure", screen);
  const auto result =
      QJsonObject{{"syntheticUiFixture", true},
                  {"modelAccuracyEvidence", false},
                  {"objects", 1000},
                  {"activitySegments", 100000},
                  {"visibleTimeSamples", 20000},
                  {"tableRows", table->model()->rowCount()},
                  {"tableChildWidgets", table->findChildren<QWidget*>().size()},
                  {"setupMilliseconds", setupMs},
                  {"memoryBefore", beforeMemory},
                  {"memoryAfter", finalProcessMemory()},
                  {"selection", finalLatencies(selections)},
                  {"filter", finalLatencies(filters, false)}};
  require(window.openProject(savedProject) && widebandDisplayReady(window),
          "Pressure fixture could not restore the real saved project");
  return result;
}

int runWidebandUnavailableCapture(const QString& outputDirectory,
                                  const QString& project,
                                  const QString& expectedFailure) {
  const QDir output(outputDirectory);
  if (!QFileInfo(outputDirectory).isAbsolute() ||
      !QDir().mkpath(outputDirectory))
    return 2;
  qApp->setProperty("uiStatePersistenceEnabled", false);
  MainWindow window;
  Capture capture{window,
                  output,
                  {child<PlotWidget>(window, "navigationPlot"),
                   child<PlotWidget>(window, "auxPlot"),
                   child<PlotWidget>(window, "mainPlot")}};
  QJsonObject report{{"pass", false},
                     {"mode", "wideband-unavailable"},
                     {"expectedFailure", expectedFailure},
                     {"modelInferenceVerified", false}};
  auto* screen = nativeAcceptanceScreen();
  try {
    require(screen && displayPixelSize(screen) == QSize(3840, 2160) &&
                std::abs(screen->devicePixelRatio() - 1.5) < .001,
            "Availability acceptance requires native selected 4K monitor / 4K "
            "/ 150%");
    report["screen"] = widebandScreenJson(screen);
    require(!report["screen"].toObject()["friendlyName"].toString().isEmpty() &&
                !displayDeviceName(screen).isEmpty(),
            "Availability capture must record the actual monitor identity");
    showFullScreenOnScreen(window, screen);
    require(window.openProject(project) && widebandDisplayReady(window),
            "Historical real-source project could not open without inference");
    auto* file = window.session().activeFile();
    const auto originalCount =
        widebandSourceCounts(*file)["importedLabels"].toInt();
    const auto historicalCount =
        widebandSourceCounts(*file)["algorithm"].toInt();
    require(originalCount == 4 && historicalCount > 0,
            "Availability project must retain real imported/history records");
    QString error;
    const auto actualData = QString::fromStdString(file->metadata.path);
    require(actualData.endsWith(".sigmf-data"),
            "Availability replay requires the project's actual SigMF source");
    require(window.importLabels(actualData.chopped(11) + ".sigmf-meta", &error,
                                false),
            error);
    require(widebandSourceCounts(*file)["importedLabels"].toInt() == 4,
            "Unavailable inference broke idempotent label import");
    startCreating(capture.plots[2]);
    createMark(capture.plots[2], file->view, .1, .2, .3, .4);
    QTest::keyClick(capture.plots[2], Qt::Key_Escape);
    require(widebandSourceCounts(*file)["manual"].toInt() >= 2,
            "Unavailable inference prevented a real native manual chart drag");
    widebandSelectModelSpectrum(window, "ffscn-gui-iq-v1");
    widebandNativeSelect(child<QComboBox>(window, "detectionRangeOrigin"), 1);
    widebandResidentCardAtTop(window);
    auto* run = child<QPushButton>(window, "runWidebandDetection");
    if (expectedFailure == "missing-models") {
      require(!run->isEnabled(),
              "Missing models must prevent detection launch");
      report["diagnostic"] =
          child<QLabel>(window, "detectionPreflight")->text();
      require(!report["diagnostic"].toString().isEmpty(),
              "Missing models have no readable preflight diagnostic");
    } else {
      require(expectedFailure == "no-compatible-gpu" &&
                  qgetenv("CUDA_VISIBLE_DEVICES") == "-1" && run->isEnabled(),
              "GPU refusal check requires valid assets and explicitly hidden "
              "CUDA devices");
      const auto count = file->detectionRuns.size();
      widebandNativeClick(run);
      require(waitUntil(
                  [&] {
                    const auto* current = window.session().activeFile();
                    return current->detectionRuns.size() > count &&
                           detectionIsTerminal(
                               current->detectionRuns.back().lifecycle);
                  },
                  60000),
              "GPU refusal did not produce a terminal task");
      const auto& failed = window.session().activeFile()->detectionRuns.back();
      require(failed.lifecycle == DetectionLifecycle::Failed &&
                  !failed.error.empty() && failed.completedWindows == 0,
              "Unavailable GPU must fail explicitly without fabricated "
              "predictions");
      report["failedRun"] = encodeDetectionRun(failed);
      report["diagnostic"] = QString::fromStdString(failed.error);
      report["deviceIsolationMethod"] =
          "CUDA_VISIBLE_DEVICES=-1 for this process and its worker";
    }
    require(widebandSourceCounts(*window.session().activeFile())["algorithm"]
                    .toInt() == historicalCount,
            "Inference refusal changed historical algorithm annotations");
    widebandScene(capture, "01-label-manual-history-without-inference", screen);
    report["labelsManualHistoryUsable"] = true;
    report["renderEvidence"] = capture.evidence();
    report["pass"] = true;
  } catch (const std::exception& failure) {
    report["error"] = QString::fromUtf8(failure.what());
    window.grab().save(output.filePath("failed-native-diagnostic.png"));
  }
  report["scenes"] = capture.scenes;
  QFile result(output.filePath("wideband-availability-report.json"));
  const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
  if (!result.open(QIODevice::WriteOnly) || result.write(bytes) != bytes.size())
    return 2;
  return report["pass"].toBool() ? 0 : 1;
}

QJsonObject widebandFinalAcceptance(Capture& capture, QScreen* screen,
                                    const QString& project,
                                    bool includePressure = true) {
  auto& window = capture.window;
  auto* file = window.session().activeFile();
  auto* sourceFilter = child<QComboBox>(window, "annotationSourceFilter");
  auto* statusFilter = child<QComboBox>(window, "annotationReviewFilter");
  auto* score = child<QDoubleSpinBox>(window, "annotationMinimumMeanScore");
  widebandNativeClick(child<QPushButton>(window, "resultTab"));
  widebandNativeSelect(sourceFilter, 0);
  widebandNativeSelect(statusFilter, 0);
  const auto scoreValue = [&](const char* value) {
    widebandScrollTo(score);
    widebandNativeReachable(score);
    score->setFocus();
    score->selectAll();
    QTest::keyClicks(score, value);
    QTest::keyClick(score, Qt::Key_Return);
  };
  scoreValue("0");
  auto* runFilter = child<QComboBox>(window, "annotationRunFilter");
  widebandNativeSelect(runFilter, 0);
  const auto allAnnotations = widebandAnnotations(*file);
  QJsonArray allSources, allRuns;
  for (const auto& source : file->sourceSets)
    allSources.append(encodeSourceSet(source));
  for (const auto& run : file->detectionRuns)
    allRuns.append(encodeDetectionRun(run));
  const auto unchanged = [&] {
    QJsonArray sources, runs;
    for (const auto& source : file->sourceSets)
      sources.append(encodeSourceSet(source));
    for (const auto& run : file->detectionRuns)
      runs.append(encodeDetectionRun(run));
    return widebandAnnotations(*file) == allAnnotations &&
           sources == allSources && runs == allRuns;
  };
  auto* visibleTable = child<QTableView>(window, "annotationTable");
  const auto sourceCounts = widebandSourceCounts(*file);
  const auto storedCount = sourceCounts["total"].toInt();
  QJsonArray visibilityChecks;
  for (int layerIndex = 0; layerIndex < 3; ++layerIndex) {
    auto* layer = child<QCheckBox>(
        window, qPrintable(QString("annotationLayer%1").arg(layerIndex)));
    const auto key =
        QStringList{"importedLabels", "manual", "algorithm"}.at(layerIndex);
    const auto sourceCount = sourceCounts[key].toInt();
    require(sourceCount > 0, "Layer verification requires all three sources");
    QTest::mouseClick(layer, Qt::LeftButton, Qt::NoModifier,
                      QPoint(8, layer->height() / 2));
    require(waitUntil([&] {
              return visibleTable->model()->rowCount() ==
                     storedCount - sourceCount;
            }) &&
                unchanged(),
            "Layer hiding changed stored annotations or did not filter rows");
    QTest::mouseClick(layer, Qt::LeftButton, Qt::NoModifier,
                      QPoint(8, layer->height() / 2));
    widebandNativeSelect(sourceFilter, layerIndex + 1);
    require(waitUntil([&] {
              return visibleTable->model()->rowCount() == sourceCount;
            }) &&
                unchanged(),
            "Source filter changed stored records or has wrong visible rows");
    visibilityChecks.append(QJsonObject{{"source", key},
                                        {"sourceCount", sourceCount},
                                        {"storedUnchanged", true}});
    widebandNativeSelect(sourceFilter, 0);
  }
  const auto runId = file->detectionRuns.back().request.runId;
  const auto runIndex = runFilter->findData(QString::fromStdString(runId));
  require(runIndex > 0, "Real model run is absent from the visible run filter");
  widebandNativeSelect(runFilter, runIndex);
  const auto expectedRunCount = std::count_if(
      file->marks.begin(), file->marks.end(), [&](const auto& annotation) {
        return !annotation.deleted && annotation.originEvidence &&
               annotation.originEvidence->runId == runId;
      });
  require(expectedRunCount > 0 && waitUntil([&] {
            return visibleTable->model()->rowCount() == expectedRunCount;
          }) &&
              unchanged(),
          "Run filter changed durable records or did not isolate the real run");
  widebandNativeSelect(runFilter, 0);
  require(
      visibleTable->model()->rowCount() == storedCount && unchanged(),
      "Restoring layers and filters did not restore the visible record set");
  std::string algorithmId;
  for (const auto& annotation : file->marks)
    if (annotation.sourceKind == AnnotationSource::Algorithm &&
        !annotation.segments.empty()) {
      algorithmId = annotation.id;
      break;
    }
  require(!algorithmId.empty(), "Final native review requires a known segment");
  const auto total = file->marks.size();
  auto* annotation = findAnnotation(*file, algorithmId);
  const auto origin = annotation->originEvidence;
  widebandSelectAnnotation(window, algorithmId, "detection-v1");
  for (const auto* action : {"acceptAnnotations", "acceptAnnotations",
                             "rejectAnnotations", "clearAnnotationReview"})
    finalClick(window, action);
  annotation = findAnnotation(*file, algorithmId);
  require(file->marks.size() == total && annotation->originEvidence == origin &&
              annotation->reviewStatus == ReviewStatus::Pending,
          "Repeated review duplicated an object or changed immutable origin");
  widebandScene(capture, "34-same-id-repeat-review", screen);

  const auto hiddenId = file->marks.front().id;
  const auto hiddenBefore = encodeAnnotation(file->marks.front());
  widebandNativeSelect(sourceFilter, 3);
  scoreValue("0.5");
  auto* table = child<QTableView>(window, "annotationTable");
  QTest::mouseClick(
      table->horizontalHeader()->viewport(), Qt::LeftButton, Qt::NoModifier,
      QPoint(table->horizontalHeader()->sectionViewportPosition(7) + 20, 15));
  require(table->model()->rowCount() >= 2,
          "Filtered batch fixture must contain at least two scored objects");
  for (int row = 0; row < 2; ++row) {
    const auto index = table->model()->index(row, 1);
    QTest::mouseClick(table->viewport(), Qt::LeftButton,
                      row ? Qt::ControlModifier : Qt::NoModifier,
                      table->visualRect(index).center());
  }
  require(file->selectedMarkIds.size() == 2,
          "Native Ctrl selection did not choose the two filtered rows");
  const auto selected = file->selectedMarkIds;
  finalClick(window, "acceptAnnotations");
  for (const auto& id : selected)
    require(findAnnotation(*file, id)->reviewStatus == ReviewStatus::Accepted,
            "Batch review missed a selected filtered object");
  require(encodeAnnotation(*findAnnotation(*file, hiddenId)) == hiddenBefore,
          "Batch review changed a hidden original label");
  widebandScene(capture, "35-score-sort-filter-batch-review", screen);
  widebandNativeSelect(sourceFilter, 0);
  scoreValue("0");

  widebandSelectAnnotation(window, algorithmId, "detection-v1");
  const auto geometryBefore = widebandAnnotations(*file);
  const auto segmentBefore = file->activeSegmentId;
  widebandNativeClick(child<QPushButton>(window, "waterfallMode"));
  require(widebandDisplayReady(window), "Waterfall switch did not settle");
  require(
      widebandAnnotations(*file) == geometryBefore &&
          file->activeSegmentId == segmentBefore,
      "T-F/waterfall switch changed physical annotation geometry/selection");
  widebandScene(capture, "36-waterfall-same-active-segment", screen);
  widebandNativeClick(child<QPushButton>(window, "timeFrequencyMode"));
  require(widebandDisplayReady(window), "T-F switch did not settle");

  const auto originalId = hiddenId;
  widebandSelectAnnotation(window, originalId);
  require(!window.findChild<QPushButton*>("beginAnnotationRevision"),
          "Direct editing still requires the obsolete revision prerequisite");
  const auto countBefore = findAnnotation(*file, originalId)->segments.size();
  finalClick(window, "splitAnnotationSegment");
  require(findAnnotation(*file, originalId)->segments.size() == countBefore + 1,
          "Native split did not create a second same-object activity");
  widebandScene(capture, "37-original-label-manual-split", screen);
  finalClick(window, "mergeAnnotationSegments");
  require(findAnnotation(*file, originalId)->segments.size() == countBefore,
          "Native merge did not retain the same annotation object");
  finalClick(window, "restoreAnnotationOrigin");
  require(
      findAnnotation(*file, originalId)->segments ==
          findAnnotation(*file, originalId)->originEvidence->originalSegments,
      "Restore did not recover the immutable original label geometry");
  widebandScene(capture, "38-merge-and-restore-origin", screen);

  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
  const auto json = capture.output.filePath("annotations-export.json");
  const auto csv = capture.output.filePath("annotations-export.csv");
  finalExport(window, "exportAnnotationJson", json);
  finalExport(window, "exportAnnotationCsv", csv);
  QFile jsonFile(json);
  require(jsonFile.open(QIODevice::ReadOnly), "Cannot read native JSON export");
  require(
      jsonFile.readAll() == exportAnnotationsJson(*file),
      "Visible JSON export differs from the full immutable/current records");
  QFile csvFile(csv);
  require(csvFile.open(QIODevice::ReadOnly), "Cannot read native CSV export");
  require(csvFile.readAll() == exportAnnotationsCsv(*file),
          "Visible CSV export differs from one-row-per-segment projection");
  widebandScene(capture, "39-visible-json-csv-export", screen);

  QJsonArray matrix;
  auto* split = child<QCheckBox>(window, "detectionSplitBursts");
  for (const auto* strategy :
       {"detectionStrategySample", "detectionStrategyRoi",
        "detectionStrategyCoarse"}) {
    finalClick(window, strategy);
    for (bool enabled : {true, false}) {
      widebandDetectionControl(window, "detectionSplitBursts");
      if (split->isChecked() != enabled) widebandNativeClick(split);
      require(split->isChecked() == enabled,
              "Native branch split indicator did not apply its requested state");
      const bool fine =
          QString::fromLatin1(strategy) != "detectionStrategyCoarse";
      require(
          child<QWidget>(window, "detectionSampleEdgeRatio")->isEnabled() ==
                  fine &&
              child<QWidget>(window, "detectionSmoothMilliseconds")
                      ->isEnabled() == fine &&
              child<QWidget>(window, "detectionMinOnMilliseconds")
                      ->isEnabled() == (fine && enabled) &&
              child<QWidget>(window, "detectionMinOffMilliseconds")
                      ->isEnabled() == (fine && enabled) &&
              child<QWidget>(window, "detectionCoarseSplitGapMilliseconds")
                      ->isEnabled() == (!fine && enabled),
          "Native branch enable matrix differs from approved M0-P semantics");
      matrix.append(QJsonObject{
          {"strategy", strategy}, {"split", enabled}, {"pass", true}});
    }
  }
  widebandScene(capture, "40-native-branch-enable-matrix", screen);
  require(window.saveProject(project), "Final native scenario save failed");
  QJsonObject result{
      {"layerAndSourceVisibility", visibilityChecks},
      {"runFilterId", QString::fromStdString(runId)},
      {"runFilterVisibleObjects", static_cast<int>(expectedRunCount)},
      {"storedRecordsUnchangedByVisibility", true},
      {"reviewSameIdAndOrigin", true},
      {"filteredBatchHiddenLabelUnchanged", true},
      {"waterfallSelectionGeometryPreserved", true},
      {"splitMergeRestoreSameObject", true},
      {"jsonExportSha256", widebandFileHash(json)},
      {"csvExportSha256", widebandFileHash(csv)},
      {"branchEnableMatrix", matrix}};
  if (includePressure)
    result["pressure"] = finalPressure(capture, screen, project);
  return result;
}
