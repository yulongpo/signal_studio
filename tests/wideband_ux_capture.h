#pragma once

// Include inside ui_capture.cpp's anonymous namespace, AFTER the wideband and
// annotation capture sidecars. All Qt/standard headers belong at global scope.
// This runner owns only W2 chart scenes; it does not call historical R/U runners.

QJsonObject widebandUxReachable(QWidget* widget, MainWindow& window,
                              bool requireEnabled = true) {
  require(widget && widget->isVisible() && !widget->size().isEmpty(),
          "W2 control is missing, hidden or empty");
  require(!requireEnabled || widget->isEnabled(),
          "W2 control is disabled: " + widget->objectName());
  const QRect bounds(widget->mapTo(&window, QPoint()), widget->size());
  const auto visible = widebandVisibleRect(widget, window);
  require(visible == bounds,
          "W2 control is clipped by an ancestor: " + widget->objectName());
  auto* hit = window.childAt(bounds.center());
  require(hit && (hit == widget || widget->isAncestorOf(hit)),
          "W2 center hit is obstructed: " + widget->objectName() +
              " hit=" + (hit ? hit->objectName() : "<none>"));
  return {{"objectName", widget->objectName()},
          {"geometryInWindow", rectJson(bounds)},
          {"visibleRect", rectJson(visible)},
          {"enabled", widget->isEnabled()},
          {"centerHit", hit->objectName()},
          {"fullyContained", true}, {"centerReachable", true}};
}

void widebandUxClick(QWidget* widget, MainWindow& window) {
  widebandUxReachable(widget, window);
  auto* native = window.windowHandle();
  require(native, "W2 click needs the actual native window");
  // Route through the window so hit testing and application event filters run.
  QPoint point = widget->rect().center();
  if (auto* checkbox = qobject_cast<QCheckBox*>(widget)) {
    QStyleOptionButton option;
    option.initFrom(checkbox);
    option.text = checkbox->text();
    point = checkbox->style()->subElementRect(QStyle::SE_CheckBoxIndicator,
                                              &option, checkbox).center();
  }
  QTest::mouseClick(native, Qt::LeftButton, Qt::NoModifier,
                    window.mapFromGlobal(widget->mapToGlobal(point)));
  QCoreApplication::processEvents();
}

void widebandUxSelect(QComboBox* combo, int index, MainWindow& window) {
  widebandUxReachable(combo, window);
  require(index >= 0 && index < combo->count(), "W2 invalid combo choice");
  const QPoint arrow(combo->width() - 8, combo->height() / 2);
  QTest::mouseClick(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                    window.mapFromGlobal(combo->mapToGlobal(arrow)));
  auto* view = combo->view();
  require(waitUntil([&] { return view->isVisible(); }),
          "W2 dropdown did not open: " + combo->objectName());
  // Respect Qt's opening-release guard and allow the native popup layout to
  // settle before moving to and selecting a different row.
  QTest::qWait(QApplication::doubleClickInterval() + 50);
  const auto item = combo->model()->index(index, combo->modelColumn(),
                                         combo->rootModelIndex());
  require(combo->model()->flags(item).testFlag(Qt::ItemIsEnabled),
          "W2 dropdown item is disabled");
  view->scrollTo(item);
  QCoreApplication::processEvents();
  const auto rect = view->visualRect(item);
  require(!rect.isEmpty() && view->viewport()->rect().contains(rect),
          "W2 dropdown choice is clipped");
  auto* popup = view->window();
  auto* hit = popup->childAt(popup->mapFromGlobal(
      view->viewport()->mapToGlobal(rect.center())));
  require(hit == view->viewport() || (hit && view->viewport()->isAncestorOf(hit)),
          "W2 dropdown choice center is obstructed");
  require(popup->windowHandle(), "W2 dropdown lacks a native window");
  QTest::mouseMove(popup->windowHandle(),
                   popup->mapFromGlobal(view->viewport()->mapToGlobal(rect.center())), 20);
  QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
  QTest::qWait(60);
  QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
  require(waitUntil([&] { return !view->isVisible() && combo->currentIndex() == index; }),
          QString("W2 visible dropdown selection did not apply: %1 wanted=%2 current=%3 popup=%4 row=%5")
              .arg(combo->objectName()).arg(index).arg(combo->currentIndex())
              .arg(view->isVisible()).arg(view->currentIndex().row()));
}

QJsonArray widebandUxToolbarEvidence(MainWindow& window) {
  QJsonArray result;
  auto* maximized = window.findChild<QWidget*>("maximizeHost");
  for (const auto* name : {"auxChartHead", "specChartHead"}) {
    auto* head = child<QWidget>(window, name);
    if (!head->isVisible()) continue;
    if (maximized && maximized->isVisible() && !maximized->isAncestorOf(head))
      continue;
    std::vector<QRect> rectangles;
    for (auto* control : head->findChildren<QWidget*>()) {
      if (!control->isVisible() ||
          (!qobject_cast<QAbstractButton*>(control) &&
           !qobject_cast<QComboBox*>(control))) continue;
      bool nested = false;
      for (auto* owner = control->parentWidget(); owner && owner != head;
           owner = owner->parentWidget())
        nested |= qobject_cast<QAbstractButton*>(owner) || qobject_cast<QComboBox*>(owner);
      if (nested) continue;
      result.append(widebandUxReachable(control, window, false));
      const QRect rect(control->mapTo(&window, QPoint()), control->size());
      for (const auto& previous : rectangles)
        require(!rect.intersects(previous),
                "W2 parallel toolbar controls overlap: " + control->objectName());
      rectangles.push_back(rect);
    }
  }
  return result;
}

int runWidebandUxCapture(const QString& outputDirectory) {
  QDir output(outputDirectory);
  if (!QFileInfo(outputDirectory).isAbsolute() ||
      !QDir().mkpath(output.absolutePath())) return 2;
  // Do not replace an earlier run's report or images, including failed runs.
  if (QFileInfo::exists(output.filePath("wideband-ux-report.json"))) return 2;
  const auto runName = "wideband-ux-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto assetsPath = output.filePath(runName + "-scenes");
  if (QFileInfo::exists(assetsPath) || !output.mkdir(runName + "-scenes")) return 2;
  const auto previousPersistence = qApp->property("uiStatePersistenceEnabled");
  qApp->setProperty("uiStatePersistenceEnabled", false);
  struct RestorePersistence {
    QVariant value;
    ~RestorePersistence() { qApp->setProperty("uiStatePersistenceEnabled", value); }
  } restorePersistence{previousPersistence};
  QJsonObject report{{"mode", "wideband-ux"}, {"scope", "W2 chart controls"},
                     {"pass", false}, {"error", ""},
                     {"qtVersion", QString::fromLatin1(qVersion())},
                     {"platform", QGuiApplication::platformName()},
                     {"startedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                     {"modelInferenceVerified", false},
                     {"resizeReposition", QJsonObject{{"status", "deferred"},
                         {"reason", "W2 offscreen UX test; native fullscreen is preserved"}}}};
  MainWindow window;
  Capture capture{window, QDir(assetsPath),
                  {child<PlotWidget>(window, "navigationPlot"),
                   child<PlotWidget>(window, "auxPlot"),
                   child<PlotWidget>(window, "mainPlot")}};
  QScreen* screen = nativeAcceptanceScreen();
  QString currentScene = "setup";
  const auto control = [&](const char* name) { return child<QWidget>(window, name); };
  const auto frame = [&](const char* name) { return control(name); };
  const auto clickControl = [&](const char* name) { widebandUxClick(control(name), window); };
  const auto noPopovers = [&] {
    for (auto* widget : window.findChildren<QWidget*>())
      if (widget->objectName().endsWith("Popover") && widget->isVisible()) return false;
    return true;
  };
  const auto scene = [&](const QString& name, QWidget* target = nullptr) {
    currentScene = name;
    require(window.isFullScreen() && window.screen() == screen &&
                window.size() == QSize(2560, 1440),
            "W2 scene must retain the selected native 4K fullscreen");
    require(widebandDisplayReady(window), "W2 real IQ display did not settle");
    // Popovers may cover chart pixels, never toolbar control click targets.
    const auto toolbar = widebandUxToolbarEvidence(window);
    widebandScene(capture, name, screen, target);
    auto entry = capture.scenes.last().toObject();
    entry["toolbarControls"] = toolbar;
    entry["fullScreen"] = window.isFullScreen();
    QJsonArray popovers;
    for (auto* widget : window.findChildren<QWidget*>()) {
      if (!widget->objectName().endsWith("Popover") || !widget->isVisible()) continue;
      require(widget->inherits("QFrame") && widget->parentWidget() == &window,
              "W2 popover must be a direct root child");
      popovers.append(widebandUxReachable(widget, window));
    }
    require(popovers.size() <= 1, "W2 settings/overflow popovers are not exclusive");
    entry["popovers"] = popovers;
    capture.scenes[capture.scenes.size() - 1] = entry;
  };
  // Reach an overflow command by its visible alternate, never by the hidden
  // original widget. This works for settings relocated by a narrow strip.
  const auto openSetting = [&](const char* name) {
    auto* button = control(name);
    if (button->isVisible()) widebandUxClick(button, window);
    else {
      auto* strip = button->parentWidget();
      const auto moreName = strip->objectName() + "Overflow";
      widebandUxClick(child<QWidget>(window, qPrintable(moreName)), window);
      const auto alternate = QString::fromLatin1(name) + "OverflowAction";
      widebandUxClick(child<QWidget>(window, qPrintable(alternate)), window);
    }
    const auto nameOfFrame = QString::fromLatin1(name) + "Popover";
    require(waitUntil([&] { return child<QWidget>(window, qPrintable(nameOfFrame))->isVisible(); }),
            "W2 settings did not open: " + QString::fromLatin1(name));
  };
  const auto escape = [&] {
    QTest::keyClick(window.windowHandle(), Qt::Key_Escape);
    require(waitUntil(noPopovers), "W2 Escape did not close settings");
  };
  try {
    report["binaryPath"] = QCoreApplication::applicationFilePath();
    report["binarySha256"] = widebandFileHash(QCoreApplication::applicationFilePath());
    require(QGuiApplication::platformName() == "windows", "W2 requires native Windows");
    require(screen && displayPixelSize(screen) == QSize(3840, 2160) &&
                screen->geometry().size() == QSize(2560, 1440) &&
                std::abs(screen->devicePixelRatio() - 1.5) < .001,
            "W2 requires one actual connected 4K/150% acceptance monitor");
    report["screen"] = widebandScreenJson(screen);
    require(!report["screen"].toObject()["friendlyName"].toString().isEmpty() &&
                !report["screen"].toObject()["monitorDevicePath"].toString().isEmpty(),
            "W2 monitor friendly name/device path unavailable");
    for (const auto* name : {"QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_FONT_DPI",
                            "QT_DEVICE_PIXEL_RATIO", "QT_AUTO_SCREEN_SCALE_FACTOR",
                            "QT_ENABLE_HIGHDPI_SCALING", "QT_SCALE_FACTOR_ROUNDING_POLICY",
                            "QT_USE_PHYSICAL_DPI"})
      require(qgetenv(name).isEmpty(), QString("W2 DPI override must be absent: %1").arg(name));
    showFullScreenOnScreen(window, screen);
    require(QTest::qWaitForWindowExposed(&window, 5000) && waitUntil([&] {
      return window.isFullScreen() && window.screen() == screen &&
             window.size() == QSize(2560, 1440);
    }, 5000), "W2 native fullscreen did not settle");
    auto screenRecord = widebandScreenJson(screen);
    screenRecord["fullScreen"] = true;
    report["screen"] = screenRecord;
    QString error;
    require(!QFileInfo::exists(output.filePath(runName)), "W2 project already exists");
    require(window.createProject(output.absolutePath(), runName, &error), error);
    const auto projectPath = output.filePath(runName + "/project.json");
    require(QFileInfo::exists(projectPath), "W2 project was not persisted");
    report["projectPath"] = projectPath;
    const auto data = widebandRepresentativeDataPath();
    const auto meta = data.chopped(11) + ".sigmf-meta";
    auto source = widebandImportActualSource(window, data, meta, screen);
    source["dataPath"] = data;
    source["metaPath"] = meta;
    source["iqSha256"] = widebandFileHash(data);
    source["metaSha256"] = widebandFileHash(meta);
    source["dataBytes"] = QString::number(QFileInfo(data).size());
    report["source"] = source;
    require(widebandDisplayReady(window), "W2 actual IQ did not load");
    require(window.findChildren<QComboBox*>("colormap").size() == 1 &&
                window.findChildren<QComboBox*>("reference").size() == 1 &&
                window.findChildren<QComboBox*>("dynamic").size() == 1,
            "W2 must reuse the unique existing palette and power editors");
    // Verify the actual toolbar contract; do not address obsolete aliases.
    child<QComboBox>(window, "psdFft");
    child<QComboBox>(window, "stftFft");
    child<QComboBox>(window, "psdStatistic");
    child<QComboBox>(window, "psdSource");
    require(window.session().activeFile()->display.auxiliaryMode == AuxiliaryMode::Waveform,
            "W2 default auxiliary view is not waveform");
    scene("w2-default-waveform");
    for (int index = 0; index < 4; ++index) {
      currentScene = QString("w2-waveform-%1").arg(index);
      widebandUxSelect(child<QComboBox>(window, "waveformMode"), index, window);
      require(static_cast<int>(window.session().activeFile()->display.waveformMode) == index,
              "W2 waveform component did not reach application state");
      scene(currentScene);
    }
    currentScene = "w2-psd";
    clickControl("psdMode");
    require(window.session().activeFile()->display.auxiliaryMode == AuxiliaryMode::Psd,
            "W2 visible PSD segment did not switch mode");
    scene(currentScene);
    currentScene = "w2-psd-settings";
    openSetting("psdSettings");
    scene(currentScene);
    auto* removeMean = child<QCheckBox>(window, "psdRemoveMean");
    widebandScrollTo(removeMean);
    const bool beforeMean = window.session().activeFile()->display.psd.parameters.removeMean;
    widebandUxClick(removeMean, window);
    require(window.session().activeFile()->display.psd.parameters.removeMean != beforeMean,
            "W2 PSD settings switch did not apply");
    scene("w2-psd-settings-switch");
    widebandScrollTo(child<QComboBox>(window, "psdMethod"));
    widebandUxSelect(child<QComboBox>(window, "psdMethod"), 2, window);
    require(static_cast<int>(window.session().activeFile()->display.psd.parameters.method) == 2,
            "W2 PSD method did not apply");
    auto* method = child<QComboBox>(window, "psdWindow");
    widebandScrollTo(method);
    widebandUxReachable(method, window);
    QTest::mouseClick(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      window.mapFromGlobal(method->mapToGlobal(QPoint(method->width()-8, method->height()/2))));
    require(waitUntil([&] { return method->view()->isVisible(); }), "W2 PSD dropdown did not open");
    scene("w2-psd-dropdown-open", method->view()->window());
    // First Escape belongs to the dropdown; the host must remain visible.
    QTest::keyClick(method->view()->window()->windowHandle(), Qt::Key_Escape);
    require(waitUntil([&] { return !method->view()->isVisible(); }) &&
                frame("psdSettingsPopover")->isVisible(), "W2 dropdown Escape closed its host");
    scene("w2-psd-dropdown-escape-retains-host");
    escape();
    scene("w2-psd-settings-escape");
    currentScene = "w2-same-entrance-toggle";
    // Power is a high-priority resident entrance. Require an actual visible
    // entrance here rather than skipping a required scenario when PSD overflows.
    openSetting("powerSettings");
    clickControl("powerSettings");
    require(waitUntil(noPopovers), "W2 same entrance did not toggle closed");
    scene(currentScene);
    currentScene = "w2-stft-settings";
    openSetting("stftSettings");
    widebandUxSelect(child<QComboBox>(window, "stftWindow"), 2, window);
    require(static_cast<int>(window.session().activeFile()->display.spectrogram.parameters.window) == 2,
            "W2 STFT window did not apply");
    scene(currentScene);
    currentScene = "w2-mutually-exclusive-settings";
    openSetting("powerSettings");
    require(!frame("stftSettingsPopover")->isVisible(), "W2 previous settings stayed open");
    scene(currentScene);
    for (const auto* name : {"reference", "dynamic"}) {
      currentScene = QString("w2-power-custom-%1").arg(name);
      auto* combo = child<QComboBox>(window, name);
      auto* edit = combo->lineEdit();
      widebandScrollTo(edit);
      widebandUxClick(edit, window);
      require(edit->hasFocus(), "W2 power editor did not receive native click focus");
      const auto before = window.session().activeFile()->display;
      const bool reference = QString::fromLatin1(name) == "reference";
      const double value = reference ? (before.referenceLevelDb == -37 ? -38 : -37)
                                     : (before.dynamicRangeDb == 73 ? 74 : 73);
      QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier);
      QTest::keyClicks(edit, QString::number(value));
      const auto draft = window.session().activeFile()->display;
      require(reference ? draft.referenceLevelDb == before.referenceLevelDb
                        : draft.dynamicRangeDb == before.dynamicRangeDb,
              "W2 typing applied an uncommitted power draft");
      QTest::keyClick(edit, Qt::Key_Return);
      require(waitUntil([&] {
        const auto& display = window.session().activeFile()->display;
        return std::abs((reference ? display.referenceLevelDb : display.dynamicRangeDb) - value) < .001;
      }), "W2 Enter did not commit custom power value: " + edit->text() +
          " focus=" + QString::number(edit->hasFocus()));
      scene(currentScene);
    }
    currentScene = "w2-auto-fit";
    const auto beforeFit = window.session().activeFile()->display;
    clickControl("autoPowerFit");
    const auto afterFit = window.session().activeFile()->display;
    require(std::isfinite(afterFit.referenceLevelDb) && afterFit.dynamicRangeDb > 0 &&
                (afterFit.referenceLevelDb != beforeFit.referenceLevelDb ||
                 afterFit.dynamicRangeDb != beforeFit.dynamicRangeDb),
            "W2 auto fit did not change the custom power range");
    scene(currentScene);
    escape();
    currentScene = "w2-display-settings";
    openSetting("displaySettings");
    for (const auto* name : {"gridToggle", "colorbarToggle"}) {
      auto* box = child<QCheckBox>(window, name);
      widebandScrollTo(box);
      const bool before = box->isChecked();
      widebandUxClick(box, window);
      const auto& display = window.session().activeFile()->display;
      require(box->isChecked() != before &&
                  (QString::fromLatin1(name) == "gridToggle" ? display.grid : display.colorScale) == !before,
              "W2 display checkbox did not apply");
      scene(QString("w2-display-%1").arg(name));
    }
    auto* frequency = child<QComboBox>(window, "freqMode");
    widebandScrollTo(frequency);
    const int nextFrequency = 1 - frequency->currentIndex();
    widebandUxSelect(frequency, nextFrequency, window);
    require(window.session().activeFile()->display.absoluteFrequency == (nextFrequency == 0),
            "W2 frequency labels did not apply");
    scene("w2-display-frequency");
    // A real outside press must BOTH dismiss the host and run the mode button.
    currentScene = "w2-outside-click-executes";
    clickControl("waveMode");
    require(waitUntil(noPopovers) &&
                window.session().activeFile()->display.auxiliaryMode == AuxiliaryMode::Waveform,
            "W2 outside click was swallowed or failed to dismiss");
    scene(currentScene);
    for (const auto* name : {"waterfallMode", "timeFrequencyMode"}) {
      currentScene = QString("w2-%1").arg(name);
      clickControl(name);
      require((window.session().activeFile()->display.mainMode == MainMode::Waterfall) ==
                  (QString::fromLatin1(name) == "waterfallMode"), "W2 main mode did not apply");
      scene(currentScene);
    }
    currentScene = "w2-main-maximized";
    const auto previousHeight = capture.plots[2]->height();
    clickControl("specMaximize");
    require(waitUntil([&] { return control("maximizeHost")->isVisible() &&
                                  capture.plots[2]->height() > previousHeight; }),
            "W2 main plot did not maximize");
    scene(currentScene);
    clickControl("specMaximize");
    require(waitUntil([&] { return !control("maximizeHost")->isVisible() &&
                                  std::abs(capture.plots[2]->height() - previousHeight) <= 2; }),
            "W2 main plot did not restore");
    scene("w2-main-restored");
    currentScene = "w2-collapsed-sidebars-tools";
    if (control("resources")->width() > 38) clickControl("resourceToggle");
    if (control("properties")->width() > 38) clickControl("propClose");
    require(control("resources")->width() == 38 && control("properties")->width() == 38,
            "W2 sidebars did not collapse");
    widebandUxSelect(child<QComboBox>(window, "colormap"), 1, window);
    openSetting("powerSettings");
    scene(currentScene);
    escape();
    clickControl("psdMode");
    openSetting("psdSettings");
    scene("w2-collapsed-sidebars-psd-tools");
    escape();
    QJsonArray overflow;
    for (const auto* stripName : {"auxChartControls", "specChartControls"}) {
      const auto moreName = QString::fromLatin1(stripName) + "Overflow";
      auto* more = child<QWidget>(window, qPrintable(moreName));
      QJsonObject evidence{{"strip", stripName}, {"width", control(stripName)->width()},
                           {"overflowNeeded", more->isVisible()}};
      if (more->isVisible()) {
        widebandUxClick(more, window);
        const auto popoverName = moreName + "Popover";
        auto* popover = child<QWidget>(window, qPrintable(popoverName));
        require(waitUntil([&] { return popover->isVisible(); }), "W2 overflow did not open");
        QJsonArray actions;
        for (auto* action : popover->findChildren<QToolButton*>()) {
          if (!action->isVisible()) continue;
          widebandScrollTo(action);
          actions.append(widebandUxReachable(action, window, false));
        }
        require(!actions.isEmpty(), "W2 overflow contains no reachable commands");
        evidence["actions"] = actions;
        scene("w2-" + moreName);
        escape();
      } else evidence["status"] = "not needed at actual fullscreen strip width; no resize forced";
      overflow.append(evidence);
    }
    report["overflow"] = overflow;
    require(noPopovers(), "W2 final scene left a settings popover open");
    scene("w2-chart-tools-complete");
    report["pass"] = capture.passed;
  } catch (const std::exception& error) {
    report["pass"] = false;
    report["error"] = QString::fromUtf8(error.what());
    report["failedScene"] = currentScene;
    capture.scenes.append(QJsonObject{{"scene", currentScene}, {"pass", false},
                                     {"error", QString::fromUtf8(error.what())}});
  }
  report["scenes"] = capture.scenes;
  report["sceneCount"] = capture.scenes.size();
  report["logicalWindowSize"] = sizeJson(window.size());
  report["renderEvidence"] = capture.evidence();
  report["fullScreen"] = window.isFullScreen();
  report["finishedUtc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  QFile reportFile(output.filePath("wideband-ux-report.json"));
  const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
  if (!reportFile.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      reportFile.write(bytes) != bytes.size() || !reportFile.flush()) return 2;
  return report["pass"].toBool() ? 0 : 1;
}
