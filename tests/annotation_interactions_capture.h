#pragma once

// Included after the existing native capture helpers. Actual IQ powers the
// charts; the explicitly named annotation fixtures test interaction, not model
// quality. All screenshots use Capture's native monitor/DPI verification.
QTreeWidgetItem* interactionTreeItem(QTreeWidget& tree, const std::string& id) {
  for (QTreeWidgetItemIterator item(&tree); *item; ++item)
    if ((*item)->data(0, Qt::UserRole).toString() == "mark" &&
        (*item)->data(0, Qt::UserRole + 1).toString().toStdString() == id)
      return *item;
  return nullptr;
}

QModelIndex interactionTableIndex(QTableView& table, const std::string& id) {
  for (int row = 0; row < table.model()->rowCount(); ++row) {
    const auto index = table.model()->index(row, 1);
    if (index.data(Qt::UserRole).toString().toStdString() == id) return index;
  }
  return {};
}

void interactionClickRow(MainWindow& window, const std::string& id,
                         Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  auto* table = child<QTableView>(window, "annotationTable");
  const auto index = interactionTableIndex(*table, id);
  require(index.isValid(), "Expected annotation is not on the current page");
  table->scrollTo(index);
  QTest::mouseClick(table->viewport(), Qt::LeftButton, modifiers,
                    table->visualRect(index).center());
  require(waitUntil([&] {
            return window.session().activeFile()->activeMarkId == id;
          }),
          "Clicked annotation is not the current business object");
}

ViewRange interactionRange(const FileMetadata& metadata, double beginMs,
                           double endMs, double lowerMHz, double upperMHz) {
  return {{static_cast<SampleIndex>(
               std::llround(beginMs * metadata.sampleRateHz / 1000)),
           static_cast<SampleIndex>(
               std::llround(endMs * metadata.sampleRateHz / 1000))},
          {lowerMHz * 1e6, upperMHz * 1e6}};
}

Annotation interactionFixture(const FileState& file,
                              const AnnotationSourceSet& set, std::string id,
                              std::string name,
                              const std::vector<ViewRange>& ranges,
                              std::optional<ViewRange> observation = {}) {
  Annotation value;
  value.id = std::move(id);
  value.name = std::move(name);
  value.fileId = file.metadata.id;
  value.sourceSetId = set.id;
  value.originRecordId = value.id;
  value.sourceKind = set.sourceKind;
  value.observation = observation;
  value.observationTimeQuality = TimeQuality::WindowObservation;
  for (const auto& range : ranges) {
    Segment segment;
    segment.id = value.id + "-s" + std::to_string(value.segments.size() + 1);
    segment.range = range;
    segment.timeQuality = set.sourceKind == AnnotationSource::Manual
                              ? TimeQuality::Manual
                              : TimeQuality::AlgorithmEstimated;
    segment.frequencyQuality = FrequencyQuality::Manual;
    value.segments.push_back(std::move(segment));
  }
  auto evidence = std::make_shared<OriginEvidence>();
  evidence->formatId = set.formatId;
  evidence->originRef = value.id;
  evidence->originalFieldsJson =
      R"({"syntheticAnnotation":true,"modelInference":false})";
  evidence->originalSegments = value.segments;
  evidence->originalObservation = observation;
  evidence->coordinateInterpretation = "RF Hz, real-source sample coordinates";
  if (set.sourceKind == AnnotationSource::Algorithm) evidence->scoreMean = .82;
  value.originEvidence = std::move(evidence);
  return value;
}

AnnotationSourceSet interactionSource(const Session& session,
                                      const FileState& file,
                                      const std::string& id,
                                      AnnotationSource kind) {
  AnnotationSourceSet set;
  set.id = id;
  set.fileId = file.metadata.id;
  set.sampleChannel = file.metadata.sampleFormat.selectedChannel;
  set.sourceKind = kind;
  set.formatId = kind == AnnotationSource::Manual ? "manual-v1"
                 : kind == AnnotationSource::ImportedLabel
                     ? "torchsig-rfbench-sigmf-v1"
                     : "detection-v1";
  set.parserId = "native-interaction-capture";
  set.parserVersion = "1";
  set.sourceHash = "explicit-ui-fixture:" + id;
  set.originalText = "Synthetic annotations for native interaction only.\n";
  set.sourceIdentity =
      annotationSourceIdentity(file.metadata, session.projectGeneration());
  return set;
}

void interactionPublishFixtures(MainWindow& window, bool large) {
  auto& session = window.session();
  auto& file = *session.activeFile();
  auto set =
      interactionSource(session, file, large ? "u1-bulk" : "u1-algorithm",
                        AnnotationSource::Algorithm);
  std::vector<Annotation> values;
  const auto range = [&](double begin, double end, double lower, double upper) {
    return interactionRange(file.metadata, begin, end, lower, upper);
  };
  if (large) {
    for (int index = 1; index <= 260; ++index) {
      const auto suffix = QString("%1").arg(index, 3, 10, QLatin1Char('0'));
      const auto geometry =
          index == 260
              ? range(17, 19, 2508.5, 2508.7)
              : range(.3 + index % 25 * .65, 1.1 + index % 25 * .65,
                      2490.6 + index % 17 * 1.02, 2490.76 + index % 17 * 1.02);
      values.push_back(
          interactionFixture(file, set, "bulk-" + suffix.toStdString(),
                             "批量 " + suffix.toStdString(), {geometry}));
    }
  } else {
    values.push_back(interactionFixture(
        file, set, "algo-located", "ALG 01 · 交互夹具双活动段",
        {range(2.9, 7.1, 2502, 2502.34), range(9.4, 16.6, 2502, 2502.34)}));
    values.push_back(interactionFixture(file, set, "algo-observation",
                                        "ALG 02 · 交互夹具时间未定位", {},
                                        range(0, 13.2768, 2499.38, 2500.12)));
    values.push_back(interactionFixture(file, set, "algo-ignored",
                                        "ALG 03 · 交互夹具已忽略",
                                        {range(5, 15, 2505.1, 2505.8)}));
    auto manual =
        interactionSource(session, file, "u1-manual", AnnotationSource::Manual);
    const auto result = session.annotations().publishSourceSet(
        file, manual,
        {interactionFixture(
            file, manual, "manual-1", "Manual 01 · 交互夹具双突发",
            {range(4, 6, 2496.8, 2497.55), range(9, 12, 2496.8, 2497.55)})});
    require(result.ok, QString::fromStdString(result.error));
  }
  const auto result =
      session.annotations().publishSourceSet(file, set, std::move(values));
  require(result.ok, QString::fromStdString(result.error));
  if (!large) {
    const auto review = session.annotations().reviewBatch(
        file, {"algo-ignored"}, ReviewStatus::Rejected);
    require(review.ok, QString::fromStdString(review.error));
  }
  window.refresh();
}

void interactionScene(Capture& capture, const QString& name, QScreen* screen) {
  const auto& file = *capture.window.session().activeFile();
  auto* inspector = child<QWidget>(capture.window, "annotationInspector");
  require(inspector->isVisible() == !file.activeMarkId.empty(),
          "Annotation inspector visibility disagrees with the current object");
  if (inspector->isVisible()) widebandScrollTo(inspector);
  capture.scene(name);
  auto record = capture.scenes.last().toObject();
  record["screen"] = widebandScreenJson(screen);
  record["captureKind"] = "native-main-window";
  record["sourceCounts"] = widebandSourceCounts(file);
  record["activeAnnotationId"] = QString::fromStdString(file.activeMarkId);
  record["activeSegmentId"] = QString::fromStdString(file.activeSegmentId);
  record["tableCounts"] =
      child<QLabel>(capture.window, "annotationCounts")->text();
  record["treeCounts"] =
      child<QLabel>(capture.window, "treeAnnotationCounts")->text();
  require(record["tableCounts"].toString().startsWith(
              record["treeCounts"].toString() + " · 页 "),
          "Tree and table annotation counts disagree");
  QJsonArray layers;
  for (int index = 0; index < 3; ++index) {
    const auto* layer = child<QCheckBox>(
        capture.window, qPrintable(QString("annotationLayer%1").arg(index)));
    layers.append(
        QJsonObject{{"index", index}, {"checked", layer->isChecked()}});
  }
  record["annotationLayers"] = layers;
  capture.scenes[capture.scenes.size() - 1] = record;
  require(record["pass"].toBool(), record["error"].toString());
  require(capture.window.screen() == screen,
          "Interaction scene moved away from the designated display");
}

void interactionSelectionScenes(Capture& capture, QScreen* screen) {
  auto& window = capture.window;
  auto& session = window.session();
  auto* file = session.activeFile();
  const auto select = [&](const std::string& id,
                          const std::string& segmentId = {}) {
    const auto result =
        session.annotations().select(*file, {id}, id, segmentId);
    require(result.ok, QString::fromStdString(result.error));
    window.refresh();
  };
  const auto initialView = file->view;
  select("algo-observation");
  interactionScene(capture, "01-three-sources-unknown-time", screen);
  select("manual-1", "manual-1-s2");
  auto* table = child<QTableView>(window, "annotationTable");
  auto* tree = child<QTreeWidget>(window, "projectTree");
  require(table->model()->columnCount() == 11,
          "The approved object/current-segment table must have 11 columns");
  const auto manual = interactionTableIndex(*table, "manual-1");
  require(manual.isValid() && table->model()
                                  ->index(manual.row(), 3)
                                  .data()
                                  .toString()
                                  .contains("2/2"),
          "Main table must describe the current second segment");
  interactionScene(capture, "02-current-segment-controls", screen);

  const auto imported = std::find_if(
      file->marks.begin(), file->marks.end(), [](const Annotation& annotation) {
        return annotation.sourceKind == AnnotationSource::ImportedLabel;
      });
  require(imported != file->marks.end(), "Actual imported labels are missing");
  const auto importedId = imported->id;
  interactionClickRow(window, importedId, Qt::ControlModifier);
  require(file->selectedMarkIds.size() == 2 &&
              file->activeMarkId == importedId && file->view == initialView,
          "Ctrl click must retain selection and make the clicked row current");
  auto* selectedNode = interactionTreeItem(*tree, importedId);
  require(selectedNode && selectedNode->isSelected(),
          "Tree selection did not project the table's current object");
  interactionScene(capture, "03-table-tree-plot-selection", screen);

  auto* hoverNode = interactionTreeItem(*tree, "algo-observation");
  require(hoverNode, "Unknown-time item is absent from the tree");
  tree->scrollToItem(hoverNode);
  const auto beforeHoverIds = file->selectedMarkIds;
  const auto beforePower = capture.plots[2]->powerGenerationCount();
  const auto beforeUploads = capture.plots[2]->textureUploadCount();
  const auto point =
      tree->viewport()->mapToGlobal(tree->visualItemRect(hoverNode).center());
  QTest::mouseMove(window.windowHandle(),
                   window.windowHandle()->mapFromGlobal(point));
  require(waitUntil([&] { return file->hoveredMarkId == "algo-observation"; }),
          "Tree hover did not reach the unified hover state");
  require(file->selectedMarkIds == beforeHoverIds &&
              file->view == initialView &&
              capture.plots[2]->powerGenerationCount() == beforePower &&
              capture.plots[2]->textureUploadCount() == beforeUploads,
          "Hover must not change selection/view or recompute spectrum data");
  interactionScene(capture, "04-linked-hover-preserves-spectrum", screen);

  interactionPublishFixtures(window, true);
  file = session.activeFile();
  select("bulk-260", "bulk-260-s1");
  require(waitUntil([&] {
            return interactionTableIndex(*table, "bulk-260").isValid() &&
                   interactionTreeItem(*tree, "bulk-260");
          }),
          "Selection beyond 100/200 did not load tree and table page");
  click(window, "annotationsPreviousPage");
  require(!interactionTableIndex(*table, "bulk-260").isValid(),
          "Previous page did not independently navigate the table");
  const auto* farAnnotation = findAnnotation(*file, "bulk-260");
  const auto& range = farAnnotation->segments.front().range;
  const auto plotPoint = capture.plots[2]->toPixel(
      (range.time.begin + range.time.end) / 2,
      (range.frequency.lowerHz + range.frequency.upperHz) / 2);
  QTest::mouseClick(capture.plots[2], Qt::LeftButton, Qt::NoModifier,
                    plotPoint.toPoint());
  require(waitUntil([&] {
            return interactionTableIndex(*table, "bulk-260").isValid();
          }),
          "Graph selection did not jump to the object's table page");
  require(file->activeMarkId == "bulk-260" &&
              file->activeSegmentId == "bulk-260-s1" &&
              file->view == initialView,
          "Graph selection lost ID/current segment or implicitly zoomed");
  interactionScene(capture, "05-graph-loads-far-tree-and-page", screen);

  click(window, "treeSelectAll");
  require(file->selectedMarkIds.size() == 268,
          "Tree select-all did not include all matching table pages");
  child<QComboBox>(window, "annotationSourceFilter")->setCurrentIndex(3);
  require(file->selectedMarkIds.size() == 263 &&
              std::all_of(file->selectedMarkIds.begin(),
                          file->selectedMarkIds.end(),
                          [&](const auto& id) {
                            return findAnnotation(*file, id)->sourceKind ==
                                   AnnotationSource::Algorithm;
                          }),
          "Changing filters retained hidden selected objects");
  interactionScene(capture, "06-matching-selection-across-pages", screen);
  click(window, "annotationLayer2");
  require(file->selectedMarkIds.empty() && file->activeMarkId.empty() &&
              table->model()->rowCount() == 0,
          "Closing a source layer retained invisible selection");
  interactionScene(capture, "07-closed-layer-prunes-selection", screen);
  click(window, "annotationLayer2");
  child<QComboBox>(window, "annotationSourceFilter")->setCurrentIndex(0);
  require(file->selectedMarkIds.empty(),
          "Restoring filters must not restore hidden old selection");
  interactionScene(capture, "08-filter-restored-without-old-selection", screen);
}

int runAnnotationInteractionsCapture(const QString& node,
                                     const QString& outputDirectory) {
  if (node != "U1" || !QFileInfo(outputDirectory).isAbsolute() ||
      !QDir().mkpath(outputDirectory))
    return 2;
  qApp->setProperty("uiStatePersistenceEnabled", false);
  const QDir output(outputDirectory);
  QJsonObject report{{"pass", false},
                     {"mode", "annotation-interactions"},
                     {"node", node},
                     {"platform", QGuiApplication::platformName()},
                     {"screens", screensJson()},
                     {"syntheticAnnotations", true},
                     {"modelInferenceVerified", false},
                     {"cpuDdcVerified", false},
                     {"startedUtc", QDateTime::currentDateTimeUtc().toString(
                                        Qt::ISODateWithMs)}};
  MainWindow window;
  Capture capture{window,
                  output,
                  {child<PlotWidget>(window, "navigationPlot"),
                   child<PlotWidget>(window, "auxPlot"),
                   child<PlotWidget>(window, "mainPlot")}};
  auto* screen = QGuiApplication::screens().value(1);
  try {
    report["screen"] = widebandScreenJson(screen);
    require(
        QGuiApplication::platformName() == "windows" && screen &&
            displayPixelSize(screen) == QSize(3840, 2160) &&
            screen->geometry().size() == QSize(2560, 1440) &&
            std::abs(screen->devicePixelRatio() - 1.5) < .001 &&
            report["screen"].toObject()["friendlyName"].toString().compare(
                "Redmi 27 NU", Qt::CaseInsensitive) == 0 &&
            displayDeviceName(screen).endsWith("DISPLAY6", Qt::CaseInsensitive),
        "Native acceptance requires connected display 2, Redmi 27 NU / "
        "DISPLAY6, actual 3840x2160 / 150%");
    for (const auto* name :
         {"QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_FONT_DPI",
          "QT_DEVICE_PIXEL_RATIO", "QT_AUTO_SCREEN_SCALE_FACTOR",
          "QT_ENABLE_HIGHDPI_SCALING", "QT_SCALE_FACTOR_ROUNDING_POLICY",
          "QT_USE_PHYSICAL_DPI"})
      require(qgetenv(name).isEmpty(), "DPI overrides must be absent");
    showFullScreenOnScreen(window, screen);
    require(QTest::qWaitForWindowExposed(&window, 5000) &&
                waitUntil(
                    [&] {
                      return window.isFullScreen() &&
                             window.screen() == screen &&
                             window.size() == QSize(2560, 1440);
                    },
                    5000),
            "Native window did not fill the specified second screen");
    QString error;
    const auto projectName = "u1-native-project-" +
                             QUuid::createUuid().toString(QUuid::WithoutBraces);
    require(window.createProject(outputDirectory, projectName, &error), error);
    const QString data =
        "W:/wbSig53_rfbench20m/train/baseline/0000000.sigmf-data";
    const auto meta = data.chopped(11) + ".sigmf-meta";
    require(
        widebandFileHash(data) ==
            "3a47e55b5f02851d702d5c5782998bf3e374f67cde592c35689acf9ec5399254",
        "Actual IQ differs from the approved 20 MS/s representative file");
    auto source = widebandImportActualSource(window, data, meta, screen);
    source["dataPath"] = data;
    source["iqSha256"] = widebandFileHash(data);
    source["metaPath"] = meta;
    source["metaSha256"] = widebandFileHash(meta);
    report["source"] = source;
    require(window.importLabels(meta, &error, false), error);
    auto* file = window.session().activeFile();
    window.session().setView(fullRange(file->metadata));
    file->display.mainMode = MainMode::TimeFrequency;
    file->display.stftSize = 2048;
    file->display.psdSize = 4096;
    file->display.colorScale = true;
    file->display.grid = true;
    interactionPublishFixtures(window, false);
    click(window, "resultTab");
    auto* detectionToggle =
        child<QToolButton>(window, "detectionSectionToggle");
    widebandScrollTo(detectionToggle);
    if (detectionToggle->isChecked())
      QTest::mouseClick(detectionToggle, Qt::LeftButton);
    require(widebandDisplayReady(window),
            "Actual IQ display did not settle on hardware QRhi");
    interactionSelectionScenes(capture, screen);
    report["renderEvidence"] = capture.evidence();
    report["fullScreen"] = window.isFullScreen();
    report["logicalWindowSize"] = sizeJson(window.size());
    report["pass"] = capture.passed && capture.scenes.size() == 8;
  } catch (const std::exception& error) {
    report["error"] = QString::fromUtf8(error.what());
    QTextStream(stderr) << error.what() << '\n';
  }
  report["scenes"] = capture.scenes;
  report["sceneCount"] = capture.scenes.size();
  report["finishedUtc"] =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  QFile reportFile(output.filePath("annotation-interactions-report.json"));
  const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
  if (!reportFile.open(QIODevice::WriteOnly) ||
      reportFile.write(bytes) != bytes.size())
    return 1;
  return report["pass"].toBool() ? 0 : 1;
}
