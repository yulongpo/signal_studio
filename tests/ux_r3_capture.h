#pragma once

// Include LAST inside ui_capture.cpp's anonymous namespace. Qt/standard and
// project headers are already included there at global scope. Do not include
// headers here: introducing a previously unseen header inside that namespace
// can put Qt/std declarations in the wrong namespace.
// CLI: --ux-r3 <absolute-output-directory> [--software-renderer]

QJsonObject uxR3ScreenIdentity(QScreen* screen) {
  auto result = widebandScreenJson(screen);
#ifdef Q_OS_WIN
  UINT32 pathCount = 0, modeCount = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
    return result;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                         &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
    return result;
  for (UINT32 i = 0; i < pathCount; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
        QString::fromWCharArray(source.viewGdiDeviceName) != displayDeviceName(screen))
      continue;
    DISPLAYCONFIG_TARGET_PREFERRED_MODE preferred{};
    preferred.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE;
    preferred.header.size = sizeof(preferred);
    preferred.header.adapterId = paths[i].targetInfo.adapterId;
    preferred.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&preferred.header) == ERROR_SUCCESS)
      result["preferredPixelSize"] = sizeJson(QSize(preferred.width, preferred.height));
    result["outputTechnology"] = static_cast<qint64>(paths[i].targetInfo.outputTechnology);
    break;
  }
#endif
  return result;
}

QJsonArray uxR3ImportedEvidence(const FileState& file) {
  QJsonArray result;
  for (const auto& mark : file.marks)
    if (!mark.deleted && mark.sourceKind == AnnotationSource::ImportedLabel) {
      auto encoded = encodeAnnotation(mark);
      encoded.remove("activeSegmentId"); // Selection is allowed; evidence/revisions are not changed.
      result.append(encoded);
    }
  return result;
}

// QTest's QWindow overload routes the mouse through actual native window hit
// testing, including QRhi child surfaces and application event filters.
void uxR3Drag(MainWindow& window, PlotWidget& plot, QPoint begin, QPoint end) {
  require(plot.isVisible() && plot.rect().contains(begin) && plot.rect().contains(end),
          "UX-R3 drag endpoints must be visible in the real chart widget");
  auto* native = window.windowHandle();
  require(native, "UX-R3 requires an exposed native window");
  const auto map = [&](QPoint point) { return window.mapFromGlobal(plot.mapToGlobal(point)); };
  QTest::mouseMove(native, map(begin), 20);
  QTest::mousePress(native, Qt::LeftButton, Qt::NoModifier, map(begin));
  for (int step = 1; step <= 8; ++step)
    QTest::mouseMove(native, map(begin + (end - begin) * step / 8), 12);
  QTest::mouseRelease(native, Qt::LeftButton, Qt::NoModifier, map(end));
  QCoreApplication::processEvents();
}

int runUxR3Capture(const QString& outputDirectory, bool software) {
  QDir output(outputDirectory);
  if (!QFileInfo(outputDirectory).isAbsolute() || !output.mkpath(".")) {
    QTextStream(stderr) << "UX-R3 requires a writable absolute output directory\n";
    return 2;
  }
  // Every invocation has a separate directory, including failed/repeated runs.
  const QString runName = QString("ux-r3-%1-%2")
      .arg(software ? "software" : "gpu", QUuid::createUuid().toString(QUuid::WithoutBraces));
  if (!output.mkdir(runName)) return 2;
  const QDir run(output.filePath(runName));
  const QString reportPath = run.filePath("ux-r3-report.json");
  const auto previousSoftware = qApp->property("softwareRenderer");
  const auto previousPersistence = qApp->property("uiStatePersistenceEnabled");
  struct RestoreUxR3Properties {
    QVariant software, persistence;
    ~RestoreUxR3Properties() {
      qApp->setProperty("softwareRenderer", software);
      qApp->setProperty("uiStatePersistenceEnabled", persistence);
    }
  } restore{previousSoftware, previousPersistence};
  qApp->setProperty("softwareRenderer", software);
  qApp->setProperty("uiStatePersistenceEnabled", false);
  QJsonArray screens, scenes;
  for (auto* candidate : QGuiApplication::screens())
    screens.append(uxR3ScreenIdentity(candidate));
  QJsonObject report{{"schemaVersion", 1}, {"mode", "ux-r3-axis-native"},
      {"softwareRenderer", software}, {"pass", false}, {"exitCode", 1},
      {"nativeAcceptance", false}, {"screens", screens},
      {"platform", QGuiApplication::platformName()},
      {"qtVersion", QString::fromLatin1(qVersion())},
      {"binaryPath", QCoreApplication::applicationFilePath()},
      {"startedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {"expectedScenes", 24}, {"modelInferenceVerified", false},
      {"geometryBaseline", "previous fixed 88 logical-pixel left; no historical before-PNG is fabricated"},
      {"evidenceScope", "real IQ / native axis geometry and gestures; no RF accuracy claim"},
      {"reportPath", reportPath}, {"status", "running"}};
  const auto write = [&] {
    report["scenes"] = scenes;
    report["completedScenes"] = scenes.size();
    int failed = 0;
    for (const auto& entry : scenes) if (!entry.toObject()["pass"].toBool()) ++failed;
    report["failedScenes"] = failed;
    QFile file(reportPath);
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
  };
  if (!write()) return 2;
  int exitCode = 1;
  std::unique_ptr<MainWindow> owner;
  QString stage = "screen-probe";
  try {
    require(QGuiApplication::platformName() == "windows", "UX-R3 requires native Windows");
    QScreen* screen = nullptr;
    int candidates = 0;
    for (auto* candidate : QGuiApplication::screens())
      if (displayPixelSize(candidate) == QSize(3840, 2160) &&
          candidate->geometry().size() == QSize(2560, 1440) &&
          std::abs(candidate->devicePixelRatio() - 1.5) < .001) {
        screen = candidate;
        ++candidates;
      }
    report["geometryCandidateCount"] = candidates;
    if (candidates != 1 || !screen) {
      exitCode = 3;
      throw std::runtime_error("BLOCKED: exactly one physical 4K/150% monitor is required");
    }
    const auto identity = uxR3ScreenIdentity(screen);
    const QPointer<QScreen> target(screen);
    report["screen"] = identity;
    const auto preferred = identity["preferredPixelSize"].toObject();
    if (identity["friendlyName"].toString().compare("Redmi 27 NU", Qt::CaseInsensitive) != 0 ||
        identity["monitorDevicePath"].toString().isEmpty() ||
        identity["devicePath"].toString().isEmpty() ||
        preferred["width"].toInt() != 3840 || preferred["height"].toInt() != 2160) {
      exitCode = 3;
      throw std::runtime_error("BLOCKED: the unique monitor is not the actual Redmi 27 NU with native 4K preferred mode");
    }
    for (const auto* name : {"QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_FONT_DPI",
                            "QT_DEVICE_PIXEL_RATIO", "QT_AUTO_SCREEN_SCALE_FACTOR",
                            "QT_ENABLE_HIGHDPI_SCALING", "QT_SCALE_FACTOR_ROUNDING_POLICY",
                            "QT_USE_PHYSICAL_DPI"})
      require(qgetenv(name).isEmpty(), QString("UX-R3 forbids DPI overrides: %1").arg(name));
    stage = "fullscreen";
    owner = std::make_unique<MainWindow>();
    auto& window = *owner;
    showFullScreenOnScreen(window, screen);
    require(QTest::qWaitForWindowExposed(&window, 5000) && waitUntil([&] {
      return window.screen() == screen && window.isFullScreen() &&
             window.size() == QSize(2560, 1440) &&
             std::abs(window.devicePixelRatioF() - 1.5) < .001;
    }, 10000), "UX-R3 actual Redmi fullscreen did not settle");
    Capture capture{window, run,
        {child<PlotWidget>(window, "navigationPlot"), child<PlotWidget>(window, "auxPlot"),
         child<PlotWidget>(window, "mainPlot")}};
    auto* auxiliary = capture.plots[1];
    auto* main = capture.plots[2];
    const auto requireTarget = [&] {
      require(target && window.screen() == target.data() && window.isFullScreen() &&
                  displayPixelSize(target.data()) == QSize(3840, 2160) &&
                  target->geometry().size() == QSize(2560, 1440) &&
                  window.size() == QSize(2560, 1440) &&
                  std::abs(target->devicePixelRatio() - 1.5) < .001,
              "UX-R3 lost the actual Redmi 4K/150% fullscreen target");
      const auto actual = uxR3ScreenIdentity(target.data());
      require(actual["monitorDevicePath"] == identity["monitorDevicePath"] &&
                  actual["friendlyName"] == identity["friendlyName"],
              "UX-R3 native monitor identity changed during capture");
    };
    stage = "real-source-import";
    QString error;
    require(window.createProject(run.absolutePath(), "axis-real-iq", &error), error);
    const QString data = widebandRepresentativeDataPath();
    require(data.endsWith(".sigmf-data"), "UX-R3 representative input must be SigMF data");
    const QString meta = data.chopped(11) + ".sigmf-meta";
    report["source"] = QJsonObject{{"dataPath", data}, {"metaPath", meta}};
    require(QFileInfo::exists(data) && QFileInfo::exists(meta), "UX-R3 actual IQ/metadata is missing");
    auto source = widebandImportActualSource(window, data, meta, screen);
    source["dataPath"] = data;
    source["metaPath"] = meta;
    source["iqSha256"] = widebandFileHash(data);
    source["metaSha256"] = widebandFileHash(meta);
    source["dataBytes"] = QString::number(QFileInfo(data).size());
    report["source"] = source;
    stage = "automatic-labels";
    report["labels"] = QJsonObject{{"countAfterSourceImport", uxR3ImportedEvidence(*window.session().activeFile()).size()},
        {"expectedAutomaticCount", 4}};
    require(waitUntil([&] {
      const auto* file = window.session().activeFile();
      return file && !file->metadata.demo && uxR3ImportedEvidence(*file).size() == 4;
    }, 20000), "UX-R3 expected the four actual labels to be discovered automatically");
    const auto imported = uxR3ImportedEvidence(*window.session().activeFile());
    const auto sourceSetCount = window.session().activeFile()->sourceSets.size();
    window.discoverLabels();
    require(uxR3ImportedEvidence(*window.session().activeFile()) == imported &&
                window.session().activeFile()->sourceSets.size() == sourceSetCount,
            "UX-R3 repeated discovery duplicated or revised existing imported labels");
    report["labels"] = QJsonObject{{"automaticImported", 4}, {"rediscoveryIdempotent", true},
        {"sourceSets", static_cast<int>(sourceSetCount)}, {"originalEvidence", imported}};
    report["nativeAcceptance"] = true;
    require(write(), "UX-R3 could not checkpoint source/screen evidence");

    const auto settled = [&] {
      return waitUntil([&] {
        const auto* file = window.session().activeFile();
        if (!file || file->metadata.demo || file->metadata.availability.status != LoadStatus::Ready ||
            !main->currentSpectrogram() || auxiliary->sourcePointCount() == 0 ||
            auxiliary->drawnPointCount() == 0 ||
            (file->display.auxiliaryMode == AuxiliaryMode::Psd && !auxiliary->currentPowerFrame())) return false;
        for (auto* plot : capture.plots)
          if (!plot->isVisible() || !plot->isDisplaySettled()) return false;
        return true;
      }, 20000);
    };
    const auto nativePoint = [&](PlotWidget& plot, QPoint local) {
      requireTarget();
      require(plot.isVisible() && plot.rect().contains(local), "UX-R3 native point is outside the widget");
      const auto at = window.mapFromGlobal(plot.mapToGlobal(local));
      auto* hit = window.childAt(at);
      require(hit && (hit == &plot || plot.isAncestorOf(hit)),
              "UX-R3 native chart point is obstructed");
      return at;
    };
    const auto physicalSnapshot = [&] { return snapshotJson(window.session().snapshot()); };
    const auto sameImported = [&] {
      require(uxR3ImportedEvidence(*window.session().activeFile()) == imported,
              "UX-R3 axis operation changed original imported label evidence");
    };
    const auto axisMeasure = [&](PlotWidget& plot) {
      auto evidence = plot.axisLayoutStatistics();
      const auto layout = plot.axisLayout();
      const QFontMetricsF metrics(plot.font(), &plot);
      QJsonArray widths;
      double maximumWidth = 0;
      for (const auto& value : evidence["yTicks"].toArray()) {
        const double width = axis_layout::textWidth(metrics, value.toString());
        maximumWidth = std::max(maximumWidth, width);
        widths.append(QJsonObject{{"text", value.toString()}, {"inkWidth", width}});
      }
      const double titleWidth = plot.axisTitleMode() == PlotWidget::AxisTitleMode::Rotated
                                   ? layout.budget.rotatedTitleWidth : 0;
      evidence["independentYInkWidths"] = widths;
      evidence["measuredMaximumYInkWidth"] = maximumWidth;
      evidence["availableYTickWidth"] = layout.plot.left() - 8 - 7 - titleWidth;
      evidence["tickBudgetFits"] = maximumWidth <= layout.plot.left() - 8 - 7 - titleWidth + .1;
      evidence["xBandsSeparate"] = layout.xTicks.bottom() <= layout.xTitle.top() &&
          layout.xTicks.top() > layout.plot.bottom() + metrics.height() / 2;
      evidence["bottomTextInsideWidget"] = layout.xTitle.bottom() <= plot.height() + .1;
      evidence["topTextSeparate"] = plot.axisTitleMode() == PlotWidget::AxisTitleMode::Rotated ||
          layout.header.bottom() <= layout.plot.top() - metrics.height() / 2;
      evidence["xTitleFits"] = axis_layout::textWidth(metrics, evidence["xTitle"].toString()) <= layout.xTitle.width();
      const bool rotated = plot.axisTitleMode() == PlotWidget::AxisTitleMode::Rotated;
      const QString title = rotated || plot.axisTitleMode() == PlotWidget::AxisTitleMode::Header
                                ? evidence["yTitle"].toString() : evidence["yUnit"].toString();
      const auto titleBox = metrics.boundingRect(
          QRectF(0, 0, rotated ? layout.plot.height() : layout.header.width(), 10000),
          Qt::TextWordWrap, title);
      evidence["yTitleMeasuredHeight"] = titleBox.height();
      evidence["yTitleAvailableHeight"] = rotated ? layout.rotatedTitle.width() : layout.header.height();
      evidence["yTitleFits"] = titleBox.height() <=
          (rotated ? layout.rotatedTitle.width() : layout.header.height()) + .1;
      QJsonArray xInk;
      double previousRight = -1e100;
      bool xInside = true;
      const auto xLabels = evidence["xTicks"].toArray();
      for (qsizetype i = 0; i < xLabels.size(); ++i) {
        const auto text = xLabels[i].toString();
        const double width = axis_layout::textWidth(metrics, text);
        const double position = layout.plot.left() + layout.plot.width() * i /
            std::max<qsizetype>(1, xLabels.size() - 1);
        const double left = std::clamp(position - width / 2, layout.plot.left() - 3,
            std::max(layout.plot.left() - 3, layout.plot.right() - width + 3));
        if (left < previousRight + 8) continue; // Same density policy as the chart.
        xInside &= left >= 0 && left + width <= plot.width();
        xInk.append(QJsonObject{{"text", text}, {"left", left}, {"inkWidth", width}});
        previousRight = left + width;
      }
      evidence["paintedXTickBounds"] = xInk;
      evidence["xTickInkInsideWidget"] = xInside && !xInk.isEmpty();
      evidence["plotInWindow"] = QJsonObject{
          {"x", layout.plot.left() + plot.mapTo(&window, QPoint()).x()},
          {"y", layout.plot.top() + plot.mapTo(&window, QPoint()).y()},
          {"width", layout.plot.width()}, {"height", layout.plot.height()}};
      return evidence;
    };

    const auto coordinateProbe = [&](QJsonArray& points) {
      auto* file = window.session().activeFile();
      const auto view = file->view;
      const bool waterfall = file->display.mainMode == MainMode::Waterfall;
      for (const auto fraction : {QPointF(.19, .73), QPointF(.53, .51), QPointF(.81, .27)}) {
        const auto rect = main->plotRect();
        const QPoint point = QPointF(rect.left() + fraction.x() * rect.width(),
                                      rect.top() + fraction.y() * rect.height()).toPoint();
        const double x = (point.x() - rect.left()) / rect.width();
        const double y = (point.y() - rect.top()) / rect.height();
        const long double expectedSample = view.time.begin +
            (waterfall ? y : x) * static_cast<long double>(view.time.end - view.time.begin);
        const double expectedFrequency = view.frequency.lowerHz +
            (waterfall ? x : 1 - y) * (view.frequency.upperHz - view.frequency.lowerHz);
        QTest::mouseClick(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                          nativePoint(*main, point));
        QCoreApplication::processEvents();
        const auto pinned = window.session().linkedCursor(file->metadata.id);
        const double sampleError = static_cast<double>(std::abs(
            static_cast<long double>(pinned.sourceSample) - expectedSample));
        const double frequencyError = std::abs(pinned.frequencyHz - expectedFrequency);
        const auto roundtrip = main->toPixel(pinned.sourceSample, pinned.frequencyHz);
        const double pixelError = QLineF(roundtrip, QPointF(point)).length();
        points.append(QJsonObject{{"click", pointJson(point)},
            {"actualSample", QString::number(pinned.sourceSample)},
            {"expectedSample", QString::number(static_cast<double>(expectedSample), 'g', 17)},
            {"actualFrequencyHz", pinned.frequencyHz}, {"expectedFrequencyHz", expectedFrequency},
            {"sampleError", sampleError}, {"frequencyErrorHz", frequencyError},
            {"logicalPixelError", pixelError}, {"pinned", pinned.pinned}});
        require(pinned.pinned && sampleError <= 1.01 && frequencyError < 1e-4 && pixelError <= 1.1,
                "UX-R3 physical cursor click/roundtrip exceeds one sample or 1.1 logical pixels");
        require(file->view == view, "UX-R3 coordinate click unexpectedly changed physical view");
        for (auto* menu : window.findChildren<QMenu*>("overlappingAnnotationsMenu"))
          if (menu->isVisible()) QTest::keyClick(menu, Qt::Key_Escape);
      }
      const auto auxiliaryRect = auxiliary->plotRect();
      const QPoint point = QPointF(auxiliaryRect.left() + auxiliaryRect.width() * .61,
                                    auxiliaryRect.top() + auxiliaryRect.height() * .57).toPoint();
      const double fraction = (point.x() - auxiliaryRect.left()) / auxiliaryRect.width();
      const bool psd = file->display.auxiliaryMode == AuxiliaryMode::Psd;
      const long double expectedSample = view.time.begin +
          fraction * static_cast<long double>(view.time.end - view.time.begin);
      const double expectedFrequency = view.frequency.lowerHz +
          fraction * (view.frequency.upperHz - view.frequency.lowerHz);
      QTest::mouseClick(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                        nativePoint(*auxiliary, point));
      QCoreApplication::processEvents();
      const auto pinned = window.session().linkedCursor(file->metadata.id);
      const double sampleError = static_cast<double>(std::abs(
          static_cast<long double>(pinned.sourceSample) - expectedSample));
      const double frequencyError = std::abs(pinned.frequencyHz - expectedFrequency);
      const double pixelError = psd
          ? frequencyError / (view.frequency.upperHz - view.frequency.lowerHz) * auxiliaryRect.width()
          : sampleError / (view.time.end - view.time.begin) * auxiliaryRect.width();
      points.append(QJsonObject{{"plot", auxiliary->objectName()}, {"click", pointJson(point)},
          {"physicalXAxis", psd ? "frequency" : "time"},
          {"actualSample", QString::number(pinned.sourceSample)},
          {"actualFrequencyHz", pinned.frequencyHz},
          {"expectedXAxisValue", psd ? expectedFrequency : static_cast<double>(expectedSample)},
          {"physicalXAxisError", psd ? frequencyError : sampleError},
          {"logicalPixelError", pixelError}, {"pinned", pinned.pinned}});
      require(pinned.pinned && pixelError <= 1.1 &&
                  (psd ? frequencyError < 1e-4 : sampleError <= 1.01) && file->view == view,
              "UX-R3 auxiliary physical X coordinate or click isolation failed");
    };

    const auto gestureProbe = [&](QJsonArray& entries) {
      // Wheel events are real Qt wheel events delivered to the visible chart,
      // as in existing native runners. Mouse drags use native QWindow routing.
      for (auto* plot : {main, auxiliary})
        for (bool yAxis : {false, true}) {
          const auto original = window.session().snapshot();
          const auto rect = plot->plotRect();
          const QPoint point = yAxis ? QPoint(qRound(rect.left() - 10), qRound(rect.center().y()))
              : QPoint(qRound(rect.center().x()), qRound(rect.bottom() + 8));
          QTest::mouseMove(window.windowHandle(), nativePoint(*plot, point), 15);
          const auto oldView = window.session().activeFile()->view;
          const auto& display = window.session().activeFile()->display;
          const double oldLow = display.auxiliaryMin, oldHigh = display.auxiliaryMax;
          const bool auxiliaryY = plot == auxiliary && yAxis;
          const bool frequency = !auxiliaryY && (plot == auxiliary
              ? display.auxiliaryMode == AuxiliaryMode::Psd
              : display.mainMode == MainMode::Waterfall ? !yAxis : yAxis);
          QWheelEvent wheel(point, plot->mapToGlobal(point), {}, QPoint(0, 120),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
          QCoreApplication::sendEvent(plot, &wheel);
          auto& after = *window.session().activeFile();
          QJsonObject entry{{"plot", plot->objectName()}, {"axis", yAxis ? "Y" : "X"},
              {"before", snapshotJson(original)}, {"afterWheel", physicalSnapshot()},
              {"wheelAccepted", wheel.isAccepted()}, {"wheelDelivery", "QWheelEvent to visible Qt widget"},
              {"dragDelivery", "QTest QWindow native hit testing"}};
          entries.append(entry); // Retain partial evidence before any assertion.
          require(wheel.isAccepted(), "UX-R3 native axis wheel was rejected");
          if (auxiliaryY)
            require(after.view == oldView && after.display.auxiliaryMax - after.display.auxiliaryMin < oldHigh - oldLow,
                    "UX-R3 auxiliary Y wheel did not isolate amplitude/power zoom");
          else if (frequency)
            require(after.view.time == oldView.time &&
                        after.view.frequency.upperHz - after.view.frequency.lowerHz <
                            oldView.frequency.upperHz - oldView.frequency.lowerHz,
                    "UX-R3 frequency-axis wheel changed the wrong physical axis");
          else
            require(after.view.frequency == oldView.frequency &&
                        after.view.time.end - after.view.time.begin < oldView.time.end - oldView.time.begin,
                    "UX-R3 time-axis wheel changed the wrong physical axis");
          require(waitUntil([&] { return !plot->hasPendingInteraction(); }, 3000),
                  "UX-R3 wheel batch did not commit");
          require(settled(), "UX-R3 wheel result did not render before the native drag");
          const auto zoomed = window.session().snapshot();
          const auto dragRect = plot->plotRect();
          const QPoint begin = yAxis ? QPoint(qRound(dragRect.left() - 10), qRound(dragRect.center().y()))
              : QPoint(qRound(dragRect.center().x()), qRound(dragRect.bottom() + 8));
          const QPoint end = begin + (yAxis ? QPoint(0, 15) : QPoint(15, 0));
          nativePoint(*plot, begin);
          nativePoint(*plot, end);
          uxR3Drag(window, *plot, begin, end);
          const auto dragged = window.session().snapshot();
          entry["afterDrag"] = snapshotJson(dragged);
          entries[entries.size() - 1] = entry;
          if (auxiliaryY)
            require(dragged.view == zoomed.view &&
                        (dragged.waveformMin != zoomed.waveformMin || dragged.psdMin != zoomed.psdMin),
                    "UX-R3 auxiliary Y native drag did not pan its own range");
          else if (frequency)
            require(dragged.view.time == zoomed.view.time && dragged.view.frequency != zoomed.view.frequency,
                    "UX-R3 native frequency-axis drag did not pan frequency only");
          else
            require(dragged.view.frequency == zoomed.view.frequency && dragged.view.time != zoomed.view.time,
                    "UX-R3 native time-axis drag did not pan time only");
          const bool sameTimeSpan = dragged.view.time.end - dragged.view.time.begin ==
                                    zoomed.view.time.end - zoomed.view.time.begin;
          const bool sameFrequencySpan = std::abs(
              (dragged.view.frequency.upperHz - dragged.view.frequency.lowerHz) -
              (zoomed.view.frequency.upperHz - zoomed.view.frequency.lowerHz)) < 1e-3;
          const bool sameAuxiliarySpan = std::abs(
              (dragged.waveformMax - dragged.waveformMin) -
              (zoomed.waveformMax - zoomed.waveformMin)) < 1e-6 && std::abs(
              (dragged.psdMax - dragged.psdMin) - (zoomed.psdMax - zoomed.psdMin)) < 1e-6;
          require(sameTimeSpan && sameFrequencySpan && sameAuxiliarySpan,
                  "UX-R3 native axis pan resized a range instead of translating it");
          require(window.session().restoreSnapshot(original), "UX-R3 could not restore the gesture baseline");
          window.refresh();
          entry["restored"] = physicalSnapshot();
          entry["pass"] = true;
          entries[entries.size() - 1] = entry;
          require(settled(), "UX-R3 restored gesture baseline did not settle");
        }
      sameImported();
    };

    const auto roiAndHandleProbe = [&](QJsonObject& evidence) {
      auto& session = window.session();
      const auto view = session.activeFile()->view;
      const auto rect = main->plotRect();
      const QPoint begin = QPointF(rect.left() + rect.width() * .24,
                                    rect.top() + rect.height() * .32).toPoint();
      const QPoint end = QPointF(rect.left() + rect.width() * .64,
                                  rect.top() + rect.height() * .69).toPoint();
      nativePoint(*main, begin); nativePoint(*main, end);
      main->beginDetectionRoi();
      uxR3Drag(window, *main, begin, end);
      const auto preview = main->detectionRoiPreview();
      require(preview.has_value(), "UX-R3 actual ROI drag produced no physical preview");
      evidence["roiPreview"] = QJsonObject{
          {"beginSample", QString::number(preview->time.begin)},
          {"endSample", QString::number(preview->time.end)},
          {"lowerHz", preview->frequency.lowerHz}, {"upperHz", preview->frequency.upperHz}};
      const auto a = main->toPixel(preview->time.begin, preview->frequency.lowerHz);
      const auto b = main->toPixel(preview->time.end, preview->frequency.upperHz);
      const auto actual = QRectF(a, b).normalized();
      const auto intended = QRectF(begin, end).normalized();
      const double error = std::max(QLineF(actual.topLeft(), intended.topLeft()).length(),
                                   QLineF(actual.bottomRight(), intended.bottomRight()).length());
      evidence["roiMaximumLogicalPixelError"] = error;
      QTest::keyClick(window.windowHandle(), Qt::Key_Escape);
      require(error <= 1.1 && !main->hasPendingInteraction() && session.activeFile()->view == view,
              "UX-R3 ROI bounds or Escape rollback failed");
      sameImported();
      const auto span = view.time.end - view.time.begin;
      const double frequencySpan = view.frequency.upperHz - view.frequency.lowerHz;
      const ViewRange fixture{{view.time.begin + span * 3 / 8, view.time.begin + span * 5 / 8},
          {view.frequency.lowerHz + frequencySpan * .37, view.frequency.lowerHz + frequencySpan * .63}};
      const auto id = session.addMark(fixture);
      require(!id.empty(), "UX-R3 manual handle fixture was rejected");
      window.refresh();
      auto* mark = findMark(*session.activeFile(), id);
      require(mark, "UX-R3 manual handle fixture disappeared");
      const auto original = mark->range;
      const auto revision = mark->revisionId;
      evidence["manualHandleBefore"] = QJsonObject{{"beginSample", QString::number(original.time.begin)},
          {"endSample", QString::number(original.time.end)},
          {"lowerHz", original.frequency.lowerHz}, {"upperHz", original.frequency.upperHz}};
      const auto corner = main->toPixel(original.time.end, original.frequency.upperHz).toPoint();
      const auto target = corner + QPoint(14, -10);
      nativePoint(*main, corner); nativePoint(*main, target);
      uxR3Drag(window, *main, corner, target);
      mark = findMark(*session.activeFile(), id);
      evidence["manualFixtureId"] = QString::fromStdString(id);
      require(mark && mark->range != original && mark->revisionId != revision,
              "UX-R3 actual manual corner drag did not revise geometry");
      require(mark->range.time.begin == original.time.begin &&
                  mark->range.frequency.lowerHz == original.frequency.lowerHz,
              "UX-R3 corner drag moved the whole mark instead of its time/frequency endpoint");
      const double handleError = QLineF(main->toPixel(mark->range.time.end,
          mark->range.frequency.upperHz), QPointF(target)).length();
      evidence["manualHandleLogicalPixelError"] = handleError;
      require(handleError <= 2.1, "UX-R3 manual corner did not track its actual native pixel target");
      evidence["manualHandleAfter"] = encodeAnnotation(*mark);
      require(session.annotations().undo(*session.activeFile()).ok,
              "UX-R3 manual handle undo failed");
      require(findMark(*session.activeFile(), id) && findMark(*session.activeFile(), id)->range == original,
              "UX-R3 manual handle undo did not restore original geometry");
      require(session.annotations().undo(*session.activeFile()).ok,
              "UX-R3 manual fixture creation undo failed");
      window.refresh();
      sameImported();
      evidence["roiCancelAndHandleUndoPassed"] = true;
    };

    for (int mode = 0; mode < 3; ++mode)
      for (bool psd : {false, true})
        for (bool waterfall : {false, true})
          for (bool longTicks : {false, true}) {
            const QString name = QString("%1-%2-%3-%4")
                .arg(QString(QChar('A' + mode)), psd ? "psd" : "waveform",
                     waterfall ? "waterfall" : "tf", longTicks ? "long" : "short");
            stage = name;
            QJsonObject entry{{"scene", name}, {"pass", false},
                {"axisMode", QString(QChar('A' + mode))}, {"longTicks", longTicks},
                {"auxiliaryMode", psd ? "psd" : "waveform"},
                {"mainMode", waterfall ? "waterfall" : "time-frequency"}};
            QJsonArray gestures, coordinates;
            QJsonObject roi;
            QString failure;
            try {
              requireTarget();
              window.cancelInteractions(true);
              for (auto* menu : window.findChildren<QMenu*>()) if (menu->isVisible()) menu->close();
              widebandNativeClick(child<QPushButton>(window, psd ? "psdMode" : "waveMode"));
              widebandNativeClick(child<QPushButton>(window, waterfall ? "waterfallMode" : "timeFrequencyMode"));
              main->setAxisTitleMode(static_cast<PlotWidget::AxisTitleMode>(mode));
              auto* file = window.session().activeFile();
              require(file && !file->metadata.demo &&
                          file->display.auxiliaryMode == (psd ? AuxiliaryMode::Psd : AuxiliaryMode::Waveform) &&
                          file->display.mainMode == (waterfall ? MainMode::Waterfall : MainMode::TimeFrequency) &&
                          main->axisTitleMode() == static_cast<PlotWidget::AxisTitleMode>(mode) &&
                          auxiliary->axisTitleMode() == main->axisTitleMode(),
                      "UX-R3 native mode controls did not apply the requested matrix combination");
              window.session().clearCursor(file->metadata.id);
              file->display.stftSize = 2048;
              file->display.psdSize = 4096;
              file->display.psd.scope = PsdScope::Visible;
              file->display.psdFromSelection = false;
              auto view = fullRange(file->metadata);
              view.time = longTicks ? TimeRange{12345, 210987} : TimeRange{0, 200000};
              if (longTicks) {
                const double center = file->metadata.centerFrequencyHz;
                view.frequency = {center - 1.734567e6, center + 2.586433e6};
              }
              window.session().setView(view, false);
              window.session().setAuxiliaryRange(
                  psd ? (longTicks ? -9999 : -120) : (longTicks ? -65536 : -16),
                  psd ? (longTicks ? 101 : 0) : (longTicks ? 65536 : 16), false);
              file->display.absoluteFrequency = true;
              std::array<quint64, 3> beforeFrames{};
              for (std::size_t i = 0; i < capture.plots.size(); ++i)
                beforeFrames[i] = capture.plots[i]->completedFrameCount();
              window.refresh();
              require(settled(), "UX-R3 real IQ charts did not settle for " + name);
              if (!software)
                require(waitUntil([&] {
                  for (std::size_t i = 0; i < capture.plots.size(); ++i) {
                    auto* plot = capture.plots[i];
                    if (!plot->gpuReady() || plot->completedFrameCount() <= beforeFrames[i] ||
                        plot->renderStatistics()["gpuDataDrawCalls"].toInteger() == 0) return false;
                  }
                  // Curves/navigation use real GPU vertices; only the main
                  // heatmap requires a scalar data texture upload.
                  return main->textureUploadCount() > 0;
                }, 10000), "UX-R3 explicit GPU mode lacks fresh QRhi frames/texture uploads");
              const auto mainAxes = axisMeasure(*main), auxAxes = axisMeasure(*auxiliary);
              entry["mainAxes"] = mainAxes;
              entry["auxiliaryAxes"] = auxAxes;
              const double alignmentError = std::abs(mainAxes["plotInWindow"].toObject()["x"].toDouble() -
                  auxAxes["plotInWindow"].toObject()["x"].toDouble());
              entry["plotLeftAlignmentErrorLogicalPx"] = alignmentError;
              entry["mainWidthGainFromFixed88"] = mainAxes["leftGainFrom88"];
              entry["auxiliaryWidthGainFromFixed88"] = auxAxes["leftGainFrom88"];
              require(alignmentError <= .5, "UX-R3 main and auxiliary axes are not aligned in the window");
              for (const auto axes : {mainAxes, auxAxes})
                require(axes["tickBudgetFits"].toBool() && axes["xBandsSeparate"].toBool() &&
                            axes["bottomTextInsideWidget"].toBool() && axes["topTextSeparate"].toBool(),
                        "UX-R3 measured text exceeds its geometry budget");
              for (const auto& axes : {mainAxes, auxAxes})
                require(axes["xTitleFits"].toBool() && axes["yTitleFits"].toBool() &&
                            axes["xTickInkInsideWidget"].toBool(),
                        "UX-R3 axis title or X tick ink is outside its allocated text band");
              for (auto* plot : capture.plots) {
                const auto backend = plot->renderingBackend();
                const auto stats = plot->renderStatistics();
                if (software)
                  require(!plot->gpuReady() && !stats["gpuDataDrawCalls"].toInteger() &&
                              stats["gpuDataRenderer"].toString() == "QPainter software fallback",
                          "UX-R3 software run unexpectedly used hardware rendering");
                else
                  require(backend.contains("D3D11", Qt::CaseInsensitive) &&
                              !backend.contains("CPU", Qt::CaseInsensitive) &&
                              !backend.contains("WARP", Qt::CaseInsensitive) &&
                              stats["gpuDataDrawCalls"].toInteger() > 0,
                          "UX-R3 explicit GPU run lacks non-CPU D3D11 data drawing");
              }
              entry["renderBeforeGestures"] = capture.evidence();
              requireTarget();
              const auto image = window.grab();
              entry["png"] = run.filePath(name + ".png");
              entry["capturedPixelSize"] = sizeJson(image.size());
              entry["capturedDpr"] = image.devicePixelRatioF();
              entry["capturedLogicalWidgetSize"] = sizeJson(window.size());
              entry["capturedScreen"] = uxR3ScreenIdentity(window.screen());
              const bool saved = image.save(entry["png"].toString());
              entry["pngSaved"] = saved;
              require(saved && image.size() == QSize(3840, 2160) &&
                          std::abs(image.devicePixelRatioF() - 1.5) < .001 &&
                          window.screen() == screen && window.isFullScreen() &&
                          window.size() == QSize(2560, 1440),
                      "UX-R3 scene screenshot is not actual Redmi 3840x2160/150% fullscreen");
              coordinateProbe(coordinates);
              // Cursor clicks deliberately exercise real frame-PSD binding.
              // Clear it before axis pans, otherwise a legitimate stale frame
              // can mask the display result of the independent axis probe.
              window.session().clearCursor(file->metadata.id);
              window.refresh();
              require(settled(), "UX-R3 display did not recover from coordinate frame probes");
              gestureProbe(gestures);
              roiAndHandleProbe(roi);
              entry["pass"] = true;
            } catch (const std::exception& exception) {
              failure = QString::fromUtf8(exception.what());
              entry["error"] = failure;
              // Preserve a native screenshot and actual dimensions even if
              // settling/geometry/gestures failed before the regular capture.
              const auto image = window.grab();
              const QString path = run.filePath(name + "-failed.png");
              entry["failurePngSaved"] = image.save(path);
              entry["failurePng"] = path;
              entry["failureCapturedPixelSize"] = sizeJson(image.size());
              entry["failureCapturedDpr"] = image.devicePixelRatioF();
              entry["failureScreen"] = uxR3ScreenIdentity(window.screen());
              window.cancelInteractions(true);
            }
            entry["gestures"] = gestures;
            entry["coordinateProbes"] = coordinates;
            entry["roiAndHandle"] = roi;
            entry["finalEvidence"] = capture.evidence();
            scenes.append(entry);
            capture.scenes.append(entry);
            for (std::size_t i = 0; i < capture.plots.size(); ++i) {
              capture.previousFrames[i] = capture.plots[i]->completedFrameCount();
              capture.previousUploads[i] = capture.plots[i]->textureUploadCount();
              capture.previousGenerations[i] = capture.plots[i]->powerGenerationCount();
            }
            report["lastScene"] = name;
            require(write(), "UX-R3 scene report checkpoint failed");
            QTextStream(stdout) << name << ": " << (failure.isEmpty() ? "PASS" : failure) << '\n';
          }
    int failures = 0;
    for (const auto& value : scenes) if (!value.toObject()["pass"].toBool()) ++failures;
    report["failedScenes"] = failures;
    report["completedScenes"] = scenes.size();
    QJsonArray comparisons;
    for (bool psd : {false, true})
      for (bool waterfall : {false, true})
        for (bool longTicks : {false, true}) {
          QJsonObject comparison{{"auxiliaryMode", psd ? "psd" : "waveform"},
              {"mainMode", waterfall ? "waterfall" : "time-frequency"}, {"longTicks", longTicks}};
          for (const auto& value : scenes) {
            const auto scene = value.toObject();
            if (scene["auxiliaryMode"] != comparison["auxiliaryMode"] ||
                scene["mainMode"] != comparison["mainMode"] ||
                scene["longTicks"] != comparison["longTicks"]) continue;
            comparison[scene["axisMode"].toString()] = QJsonObject{
                {"pass", scene["pass"]}, {"mainAxes", scene["mainAxes"]},
                {"auxiliaryAxes", scene["auxiliaryAxes"]}, {"png", scene["png"]}};
          }
          const auto a = comparison["A"].toObject(), c = comparison["C"].toObject();
          if (!a["mainAxes"].toObject().isEmpty() && !c["mainAxes"].toObject().isEmpty()) {
            comparison["mainWidthGainAoverC"] = c["mainAxes"].toObject()["sharedLeft"].toDouble() -
                a["mainAxes"].toObject()["sharedLeft"].toDouble();
            comparison["auxiliaryWidthGainAoverC"] = c["auxiliaryAxes"].toObject()["sharedLeft"].toDouble() -
                a["auxiliaryAxes"].toObject()["sharedLeft"].toDouble();
          }
          comparisons.append(comparison);
        }
    report["axisComparisons"] = comparisons;
    report["pass"] = failures == 0 && scenes.size() == 24;
    exitCode = report["pass"].toBool() ? 0 : 1;
  } catch (const std::exception& exception) {
    report["error"] = QString::fromUtf8(exception.what());
    report["failedStage"] = stage;
    if (owner && owner->isVisible()) {
      if (const auto* file = owner->session().activeFile())
        report["failureSourceCounts"] = widebandSourceCounts(*file);
      const auto image = owner->grab();
      report["setupFailurePng"] = run.filePath("setup-failed.png");
      report["setupFailurePngSaved"] = image.save(report["setupFailurePng"].toString());
      report["setupFailurePixelSize"] = sizeJson(image.size());
      report["setupFailureDpr"] = image.devicePixelRatioF();
      report["setupFailureScreen"] = uxR3ScreenIdentity(owner->screen());
    }
  }
  report["exitCode"] = exitCode;
  report["status"] = exitCode == 3 ? "blocked" : exitCode == 0 ? "passed" : "failed";
  report["finishedUtc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  if (!write()) {
    QTextStream(stderr) << "UX-R3 report write failed: " << reportPath << '\n';
    return 2;
  }
  QTextStream(stdout) << "UX-R3 report: " << reportPath << '\n';
  return exitCode;
}
