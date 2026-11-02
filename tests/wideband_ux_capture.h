#pragma once

// Include inside ui_capture.cpp's anonymous namespace, AFTER the wideband and
// annotation capture sidecars. All Qt/standard headers belong at global scope.
// This runner owns this optimization's scenes and counts. Shared native helpers
// do not modify archived R/U reports or their acceptance counts.

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

QJsonObject widebandUxOpenSavedProject(MainWindow& window, const QString& projectPath,
                                       const QDir& output) {
  const bool oldNativeDialogs = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
  bool accepted = false, typed = false;
  QString failure;
  QElapsedTimer elapsed;
  elapsed.start();
  QTimer picker;
  picker.setInterval(100);
  QJsonObject trace;
  QObject::connect(&picker, &QTimer::timeout, &window, [&] {
    if (auto* question = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        question && question->windowTitle() == "未保存的工程修改") {
      if (auto* discard = question->button(QMessageBox::Discard))
        QTest::mouseClick(discard, Qt::LeftButton);
      return;
    }
    auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
    if (!dialog) {
      if (accepted) return;
      if (elapsed.elapsed() < 15000) return;
      failure = "W6 visible project file picker did not appear";
      picker.stop();
      for (auto* candidate : window.findChildren<QFileDialog*>()) candidate->reject();
      if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) modal->reject();
      return;
    }
    try {
      auto* field = child<QLineEdit>(*dialog, "fileNameEdit");
      trace = {{"elapsedMs", elapsed.elapsed()}, {"directory", dialog->directory().absolutePath()},
               {"selectedFiles", QJsonArray::fromStringList(dialog->selectedFiles())},
               {"fileNameText", field->text()}, {"typed", typed}};
      QFile diagnostic(output.filePath("project-picker-trace.json"));
      if (diagnostic.open(QIODevice::WriteOnly)) diagnostic.write(QJsonDocument(trace).toJson());
      if (elapsed.elapsed() >= 15000) dialog->grab().save(output.filePath("project-picker-failed.png"));
      require(elapsed.elapsed() < 15000, "W6 project picker did not accept: " + field->text());
      if (!typed) {
        require(field->isVisible() && field->isEnabled(), "W6 project file name is not editable");
        QTest::mouseClick(field, Qt::LeftButton);
        QTest::keyClick(field, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(field, QDir::toNativeSeparators(projectPath));
        typed = true;
        return;
      }
      QTest::keyClick(field, Qt::Key_Return);
      if (!dialog->isVisible()) accepted = true;
    } catch (const std::exception& error) {
      failure = QString::fromUtf8(error.what());
      picker.stop(); dialog->reject();
    }
  });
  picker.start();
  widebandMenuAction(window, "openProjectJsonAction");
  picker.stop();
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, oldNativeDialogs);
  require(accepted && failure.isEmpty(), "W6 project picker reopen failed: " + failure);
  trace["pass"] = true;
  return trace;
}

int runWidebandUxReopenProbe(const QString& projectPath, const QString& outputDirectory) {
  const QDir output(outputDirectory);
  if (!QFileInfo(outputDirectory).isAbsolute() || !QDir().mkpath(outputDirectory)) return 2;
  qApp->setProperty("uiStatePersistenceEnabled", false);
  MainWindow window;
  QJsonObject result{{"pass", false}};
  try {
    showFullScreenOnScreen(window, nativeAcceptanceScreen());
    require(window.openProject(projectPath) && widebandDisplayReady(window), "Cannot prepare the saved-project probe");
    const auto generation = window.session().projectGeneration();
    result["picker"] = widebandUxOpenSavedProject(window, projectPath, output);
    require(window.session().projectGeneration() > generation && widebandDisplayReady(window), "Picker did not replace the project");
    result["pass"] = true;
  } catch (const std::exception& error) { result["error"] = QString::fromUtf8(error.what()); }
  QFile report(output.filePath("project-reopen-probe.json"));
  if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(result).toJson());
  return result["pass"].toBool() ? 0 : 1;
}

int runWidebandUxReviewProbe(const QString& projectPath, const QString& outputDirectory) {
  const QDir output(outputDirectory);
  if (!QFileInfo(outputDirectory).isAbsolute() || !QDir().mkpath(outputDirectory)) return 2;
  qApp->setProperty("uiStatePersistenceEnabled", false);
  MainWindow window;
  Capture capture{window, output, {child<PlotWidget>(window, "navigationPlot"),
      child<PlotWidget>(window, "auxPlot"), child<PlotWidget>(window, "mainPlot")}};
  QJsonObject result{{"pass", false}};
  try {
    auto* screen = nativeAcceptanceScreen();
    showFullScreenOnScreen(window, screen);
    require(window.openProject(projectPath), "Cannot prepare the saved-review probe");
    for (const auto& file : window.session().project().files)
      if (file.metadata.path.ends_with(".sigmf-data")) { window.session().activateFile(file.metadata.id); break; }
    window.refresh();
    require(widebandDisplayReady(window), "Actual review source did not settle");
    result["review"] = widebandFinalAcceptance(capture, screen, projectPath, false);
    result["pass"] = true;
  } catch (const std::exception& error) {
    result["error"] = QString::fromUtf8(error.what());
    if (auto* popup = QApplication::activePopupWidget()) popup->grab().save(output.filePath("failed-popup.png"));
    window.grab().save(output.filePath("failed-review.png"));
  }
  result["scenes"] = capture.scenes;
  QFile report(output.filePath("review-probe.json"));
  if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(result).toJson());
  return result["pass"].toBool() ? 0 : 1;
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
  QJsonObject report{{"mode", "wideband-ux"}, {"scope", "W0-W6 final UI journey"},
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
  const auto commit = [&](const char* name, const QString& value) {
    auto* widget = control(name);
    widebandScrollTo(widget);
    widebandUxClick(widget, window);
    auto* editor = qobject_cast<QLineEdit*>(widget);
    if (!editor) editor = widget->findChild<QLineEdit*>();
    require(editor && editor->isVisible(), "W6 parameter has no visible editor");
    QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(editor, value);
    QTest::keyClick(editor, Qt::Key_Return);
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
    require(!QFileInfo::exists(output.filePath(runName + ".ssproj")), "W2 project already exists");
    require(window.createProject(output.absolutePath(), runName, &error), error);
    const auto projectPath = output.filePath(runName + ".ssproj");
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
    // Inspect every method on the real monitor, including whole-row hiding
    // and return to a previously committed Welch/Kaiser configuration.
    currentScene = "w6-psd-welch-kaiser";
    auto* psdMethod = child<QComboBox>(window, "psdMethod");
    widebandScrollTo(psdMethod);
    widebandUxSelect(psdMethod, 2, window);
    commit("psdOverlap", "65");
    auto* psdWindow = child<QComboBox>(window, "psdWindow");
    widebandScrollTo(psdWindow);
    widebandUxSelect(psdWindow, 6, window);
    commit("psdKaiserBeta", "12.5");
    const auto welch = window.session().activeFile()->display.psd.parameters;
    scene("w6-psd-welch-kaiser");
    for (const auto index : {1, 3, 4, 2}) {
      widebandScrollTo(psdMethod);
      widebandUxSelect(psdMethod, index, window);
      if (index == 1)
        require(window.session().activeFile()->display.psd.parameters.overlap == 0 &&
                    !control("psdOverlap")->isVisible(), "W6 Bartlett overlap row is not hidden/zero");
      if (index == 3) {
        commit("psdTimeBandwidth", "4.5");
        commit("psdTapers", "7");
        require(!psdWindow->isVisible(), "W6 DPSS retained an irrelevant window row");
      }
      if (index == 4) {
        commit("psdBurgOrder", "27");
        require(!psdWindow->isVisible(), "W6 Burg retained an irrelevant window row");
      }
      scene(QString("w6-psd-method-%1").arg(index));
    }
    require(window.session().activeFile()->display.psd.parameters == welch,
            "W6 returning to Welch lost its committed legal configuration");
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
    currentScene = "w4-detection-basic";
    clickControl("propRailOpen");
    clickControl("contextDetection");
    auto* card = child<DetectionCard>(window, "detectionCard");
    auto* propScroll = child<QScrollArea>(window, "propScroll");
    propScroll->verticalScrollBar()->setValue(0);
    QCoreApplication::processEvents();
    QJsonArray basicControls;
    for (const auto* name : {"detectionModel", "detectionSpectrumMode", "detectionRangeOrigin",
                             "detectionScoreThreshold", "detectionTimeStrategy", "detectionPreflight"})
      basicControls.append(widebandUxReachable(control(name), window));
    basicControls.append(widebandUxReachable(control("runWidebandDetection"), window, false));
    require(!propScroll->isAncestorOf(card->actionsWidget()), "W4 run action is inside scrolling content");
    scene(currentScene);
    auto basicScene = capture.scenes.last().toObject();
    basicScene["firstScreenControls"] = basicControls;
    capture.scenes[capture.scenes.size() - 1] = basicScene;
    for (const auto* name : {"detectionAnalysisParametersToggle", "detectionPostprocessParametersToggle",
                             "detectionResourceParametersToggle", "detectionCalculationDetailsToggle",
                             "detectionDiagnosticsToggle"}) {
      auto* toggle = child<QAbstractButton>(window, name);
      widebandScrollTo(toggle);
      widebandUxClick(toggle, window);
      require(toggle->isChecked(), "W4 detection fold did not open");
      scene(QString("w4-%1").arg(name));
    }
    clickControl("contextDisplay");
    scene("w4-display-summary");
    clickControl("fileProperties");
    require(control("effectiveBandwidthMHz")->isVisible(), "W4 file bandwidth editor is unreachable");
    widebandUxReachable(control("effectiveBandwidthMHz"), window);
    scene("w4-file-properties");
    escape();
    clickControl("contextAnnotation");
    require(control("annotationEmptyGuide")->isVisible(), "W4 missing annotation guide");
    require(!control("contextChannel")->isEnabled(), "W4 channel page should require a channel");
    scene("w4-annotation-empty-guide");
    // A real menu close must not deactivate and cancel the newly selected
    // ROI tool. Escape restores the persistent tool captured by that menu.
    clickControl("contextDisplay");
    clickControl("manualTool");
    auto* mainPlot = capture.plots[2];
    require(mainPlot->isCreating(), "W4 persistent tool did not start");
    const auto markCount = window.session().activeFile()->marks.size();
    const auto runCount = window.session().activeFile()->detectionRuns.size();
    std::unique_ptr<QMenu> roiMenu(mainPlot->createContextMenu(mainPlot->plotRect().center().toPoint()));
    auto* roiAction = child<QAction>(*roiMenu, "contextDetectionRoiAction");
    roiMenu->popup(mainPlot->mapToGlobal(mainPlot->plotRect().center().toPoint()));
    require(waitUntil([&] { return roiMenu->isVisible(); }), "W4 ROI menu did not open");
    require(roiAction->isEnabled(), "W4 ROI menu action is disabled");
    QTest::mouseClick(roiMenu.get(), Qt::LeftButton, Qt::NoModifier,
                      roiMenu->actionGeometry(roiAction).center());
    require(waitUntil([&] { return mainPlot->isSelectingDetectionRoi() &&
                                  control("detectionPage")->isVisible(); }),
            "W4 actual menu selection did not retain its ROI tool");
    scene("w4-menu-roi-retained");
    QTest::keyClick(mainPlot, Qt::Key_Escape);
    require(mainPlot->isCreating() && !mainPlot->isSelectingDetectionRoi(),
            "W4 menu ROI Escape did not restore the previous persistent tool");
    require(window.session().activeFile()->marks.size() == markCount &&
                window.session().activeFile()->detectionRuns.size() == runCount,
            "W4 menu ROI created an annotation or a run");
    clickControl("manualTool");
    scene("w4-menu-roi-escape");
    clickControl("contextDisplay");
    QJsonArray productionRuns;
    for (const auto* modelId : {"ffscn-gui-iq-v1", "scn-max-psd-v1"}) {
      currentScene = QString("w6-%1-preflight").arg(modelId);
      clickControl("contextDetection");
      // First-screen configuration is sufficient for both formal models.
      for (const auto* fold : {"detectionAnalysisParametersToggle", "detectionPostprocessParametersToggle",
                                "detectionResourceParametersToggle", "detectionCalculationDetailsToggle",
                                "detectionDiagnosticsToggle"}) {
        auto* toggle = child<QToolButton>(window, fold);
        if (toggle->isChecked()) { widebandScrollTo(toggle); widebandUxClick(toggle, window); }
      }
      auto* model = child<QComboBox>(window, "detectionModel");
      widebandScrollTo(model);
      widebandUxSelect(model, model->findData(modelId), window);
      auto* spectrum = child<QComboBox>(window, "detectionSpectrumMode");
      widebandUxSelect(spectrum, spectrum->findData(0), window);
      auto* range = child<QComboBox>(window, "detectionRangeOrigin");
      widebandUxSelect(range, range->findData("available-file"), window);
      clickControl("detectionStrategyCoarse");
      auto* formalCard = child<DetectionCard>(window, "detectionCard");
      require(formalCard->validationError().isEmpty(), "W6 formal preflight: " + formalCard->validationError());
      auto* runButton = child<QPushButton>(window, "runWidebandDetection");
      widebandUxReachable(runButton, window);
      scene(currentScene);
      const auto beforeRuns = window.session().activeFile()->detectionRuns.size();
      widebandUxClick(runButton, window);
      require(waitUntil([&] { return window.session().activeFile()->detectionRuns.size() == beforeRuns + 1; }),
              "W6 visible run button did not create a frozen task");
      const auto frozen = encodeDetectionRequest(window.session().activeFile()->detectionRuns.back().request);
      const auto runId = window.session().activeFile()->detectionRuns.back().request.runId;
      scene(QString("w6-%1-submitted").arg(modelId));
      require(waitUntil([&] { return detectionIsTerminal(window.session().activeFile()->detectionRuns.back().lifecycle); }, 120000),
              "W6 formal model did not reach a terminal state");
      const auto completed = window.session().activeFile()->detectionRuns.back();
      require(completed.lifecycle == DetectionLifecycle::Completed &&
                  completed.completedWindows > 0 && completed.completedWindows == completed.eligibleWindows &&
                  completed.request.profile.id == modelId &&
                  completed.request.profile.backend != DetectionBackendKind::Fixture &&
                  !completed.request.modelSha256.empty() && !completed.device.empty() &&
                  completed.device.find("CPU spectrum") == std::string::npos && !completed.runtime.empty() &&
                  encodeDetectionRequest(completed.request) == frozen,
              "W6 actual model failed: " + QString::fromStdString(completed.error));
      DetectionRun durable;
      require(loadDetectionRun(QString::fromStdString(window.session().project().assetDirectory), QString::fromStdString(runId), durable, error), error);
      require(encodeDetectionRequest(durable.request) == frozen && durable.completedWindows == completed.completedWindows,
              "W6 formal frozen request/prefix changed on disk");
      productionRuns.append(encodeDetectionRun(completed));
      clickControl("taskTab");
      auto* list = child<QAbstractItemView>(window, "detectionRunList");
      QModelIndex selected;
      for (int row = 0; row < list->model()->rowCount(); ++row) {
        const auto index = list->model()->index(row, 0);
        if (index.data(Qt::UserRole).toString().toStdString() == runId) selected = index;
      }
      require(selected.isValid(), "W6 real model task is absent from its list");
      list->scrollTo(selected);
      const auto visible = list->visualRect(selected).intersected(list->viewport()->rect());
      require(!visible.isEmpty(), "W6 actual task row is outside its viewport");
      QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, visible.topLeft() + QPoint(18, 12));
      list->setFocus();
      require(list->hasFocus(), "W6 task keyboard focus was not acquired");
      QSignalSpy sourceFocus(child<DetectionPanel>(window, "detectionPanel"), &DetectionPanel::runFocused);
      QTest::keyClick(list, Qt::Key_Return);
      require(waitUntil([&] { return sourceFocus.count() == 1; }) &&
                  sourceFocus.first().first().toString().toStdString() == runId,
              "W6 task Return did not focus exactly its runId");
      bool detailsSeen = false;
      QString detailsError;
      QTimer::singleShot(60, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        try {
          require(dialog, "W6 task Alt+Return did not open details");
          auto* facts = child<QWidget>(*dialog, "detectionRunFacts");
          require(facts->property("plainText").toString().contains(QString::fromStdString(runId)),
                  "W6 actual task details show the wrong run");
          scene(QString("w6-%1-task-details-keyboard").arg(modelId), dialog);
          detailsSeen = true;
          QTest::keyClick(dialog, Qt::Key_Escape);
        } catch (const std::exception& failure) {
          detailsError = QString::fromUtf8(failure.what());
          if (dialog) dialog->reject();
        }
      });
      list->setFocus();
      QTest::keyClick(list, Qt::Key_Return, Qt::AltModifier);
      require(detailsSeen && detailsError.isEmpty(), "W6 task details: " + detailsError);
      window.activateWindow();
      require(waitUntil([&] { return window.isActiveWindow(); }), "W6 model details did not return window focus");
      scene(QString("w6-%1-completed").arg(modelId));
    }
    report["productionRuns"] = productionRuns;
    report["modelInferenceVerified"] = productionRuns.size() == 2;
    currentScene = "w6-real-task-cancel-prefix";
    clickControl("contextDetection");
    auto* cancelModel = child<QComboBox>(window, "detectionModel");
    widebandScrollTo(cancelModel);
    widebandUxSelect(cancelModel, cancelModel->findData("ffscn-gui-iq-v1"), window);
    auto* analysis = child<QToolButton>(window, "detectionAnalysisParametersToggle");
    widebandScrollTo(analysis); widebandUxClick(analysis, window);
    commit("detectionFftSize", "8192");
    commit("detectionModelFrames", "3");
    commit("detectionModelHopFrames", "1");
    commit("detectionHopMilliseconds", "0.01");
    const auto cancellationStart = window.session().activeFile()->detectionRuns.size();
    clickControl("runWidebandDetection");
    require(waitUntil([&] { return window.session().activeFile()->detectionRuns.size() == cancellationStart + 1; }),
            "W6 formal cancellation task was not submitted");
    require(waitUntil([&] { const auto& run = window.session().activeFile()->detectionRuns.back();
        return run.completedWindows > 0 && !detectionIsTerminal(run.lifecycle); }, 60000),
        "W6 formal cancellation did not expose a live persisted prefix");
    const auto beforeCancellation = window.session().activeFile()->detectionRuns.back();
    const auto immutableChunk = beforeCancellation.chunks.front();
    const auto chunkPath = QDir(QString::fromStdString(window.session().project().assetDirectory)).filePath(QString::fromStdString(immutableChunk.relativePath));
    const auto chunkHash = widebandFileHash(chunkPath);
    const auto displayBefore = window.session().activeFile()->display;
    for (const auto* name : {"stftFft", "psdFft", "colormap"}) {
      auto* combo = child<QComboBox>(window, name);
      widebandUxSelect(combo, (combo->currentIndex() + 1) % combo->count(), window);
    }
    openSetting("powerSettings");
    commit("dynamic", "72");
    escape();
    const auto& afterDisplayChange = window.session().activeFile()->detectionRuns.back();
    const auto& changedDisplay = window.session().activeFile()->display;
    require(!detectionIsTerminal(afterDisplayChange.lifecycle) &&
                encodeDetectionRequest(afterDisplayChange.request) == encodeDetectionRequest(beforeCancellation.request) &&
                displayBefore.stftSize != changedDisplay.stftSize && displayBefore.psdSize != changedDisplay.psdSize &&
                displayBefore.palette != changedDisplay.palette && changedDisplay.dynamicRangeDb == 72,
            "W6 display changes altered a live formal request or did not reach display state");
    clickControl("taskTab");
    auto* cancellationList = child<QAbstractItemView>(window, "detectionRunList");
    QModelIndex cancellationRow;
    for (int row = 0; row < cancellationList->model()->rowCount(); ++row) {
      const auto index = cancellationList->model()->index(row, 0);
      if (index.data(Qt::UserRole).toString().toStdString() == beforeCancellation.request.runId) cancellationRow = index;
    }
    require(cancellationRow.isValid(), "W6 live task row is missing");
    cancellationList->scrollTo(cancellationRow);
    QCoreApplication::processEvents();
    const auto cancelVisible = cancellationList->visualRect(cancellationRow).intersected(cancellationList->viewport()->rect());
    require(!cancelVisible.isEmpty(), "W6 live task row is clipped out");
    QTest::mouseClick(cancellationList->viewport(), Qt::LeftButton, Qt::NoModifier, cancelVisible.topLeft() + QPoint(18, 12));
    cancellationList->setFocus();
    require(cancellationList->hasFocus() && child<QAction>(window, "taskCancelAction")->isEnabled(),
            "W6 actual live task has no reachable keyboard cancel");
    QTest::keyClick(cancellationList, Qt::Key_X, Qt::ControlModifier | Qt::ShiftModifier);
    require(waitUntil([&] { return detectionIsTerminal(window.session().activeFile()->detectionRuns.back().lifecycle); }, 60000),
            "W6 keyboard cancellation did not finish");
    const auto cancelled = window.session().activeFile()->detectionRuns.back();
    require(cancelled.lifecycle == DetectionLifecycle::Cancelled &&
                cancelled.request.runId == beforeCancellation.request.runId &&
                cancelled.completedWindows > 0 && cancelled.completedWindows < cancelled.eligibleWindows &&
                encodeDetectionRequest(cancelled.request) == encodeDetectionRequest(beforeCancellation.request) &&
                widebandFileHash(chunkPath) == chunkHash,
            "W6 keyboard cancel changed target/request or lost its immutable prefix");
    report["nativeCancellation"] = QJsonObject{{"pass", true}, {"input", "Ctrl+Shift+X on the visible selected task row"},
        {"displayChangedDuringRun", true}, {"unchangedPrefixSha256", chunkHash}, {"run", encodeDetectionRun(cancelled)}};
    scene(currentScene);
    // Restore the chart budget after examining task rows.
    if (child<QToolButton>(window, "resultsToggle")->isChecked()) clickControl("resultsToggle");
    clickControl("timeFrequencyMode");
    clickControl("manualTool");
    const auto beforeManual = window.session().activeFile()->marks.size();
    createMark(mainPlot, window.session().activeFile()->view, .25, .55, .38, .52);
    require(window.session().activeFile()->marks.size() == beforeManual + 1,
            "W6 visible continuous tool did not create its manual annotation");
    const auto manualId = window.session().activeFile()->activeMarkId;
    clickControl("manualTool");
    clickControl("contextAnnotation");
    const auto* manual = findAnnotation(*window.session().activeFile(), manualId);
    require(manual && manual->sourceKind == AnnotationSource::Manual && activeSegment(*manual),
            "W6 created annotation is not a located manual source");
    const auto beforeRevision = manual->revisionId;
    const auto originalEvidence = *manual->originEvidence;
    const auto newEnd = activeSegment(*manual)->range.time.end - 1;
    commit("annotationEndMilliseconds", QString::number(newEnd / window.session().activeFile()->metadata.sampleRateHz * 1000, 'g', 17));
    manual = findAnnotation(*window.session().activeFile(), manualId);
    require(manual->revisionId != beforeRevision && *manual->originEvidence == originalEvidence &&
                activeSegment(*manual)->range.time.end == newEnd,
            "W6 visible geometry edit changed original evidence or lost its revision");
    scene("w6-manual-annotation-direct-edit");
    clickControl("extract");
    auto* channelDialog = channelsDialog(window, screen);
    scene("w6-channel-source-and-parameters", channelDialog);
    const auto channelCount = window.session().activeFile()->channels.size();
    click(*channelDialog, "confirmChannelConfig");
    require(waitUntil([&] { return window.session().activeFile()->channels.size() == channelCount + 1; }),
            "W6 visible channel confirmation did not create its source snapshot");
    const auto channel = window.session().activeFile()->channels.back();
    require(channel.sourceSnapshot && channel.sourceSnapshot->annotationId == manualId,
            "W6 channel snapshot is bound to a different annotation");
    const auto ddc = channelsDdc(window, channel);
    auto* narrow = window.findChild<NarrowbandWorkspace*>();
    require(narrow && waitUntil([&] { return narrow->isVisible() && narrow->visibleChartsSettled() &&
        narrow->visibleChartsGpuReady() && narrow->dataStatusText().contains("真实 DDC"); }, 60000),
        "W6 narrowband did not display real CPU DDC output");
    clickControl("contextChannel");
    scene("w6-channel-page-real-cpu-ddc");
    report["cpuDdc"] = ddc;
    report["cpuDdcVerified"] = true;
    clickControl("narrowbandClose");
    require(widebandDisplayReady(window), "W6 return from channel did not restore charts");
    // Save and reopen through the actual menu and visible .ssproj file picker.
    widebandMenuAction(window, "saveProjectAction");
    QFile saved(projectPath);
    require(saved.open(QIODevice::ReadOnly), "W6 menu Save did not write .ssproj");
    const auto savedObject = QJsonDocument::fromJson(saved.readAll()).object();
    saved.close();
    require(savedObject["magic"].toString() == "SignalStudioProject" &&
                savedObject["schemaVersion"].toInt() == 1 &&
                !savedObject["projectId"].toString().isEmpty(),
            "W6 .ssproj container identity/schema is invalid");
    require(savedObject["state"].toObject()["version"].toInt() == 6,
            "W6 native project state schema changed");
    const auto generation = window.session().projectGeneration();
    currentScene = "w6-visible-project-reopen";
    report["projectPicker"] = widebandUxOpenSavedProject(window, projectPath, output);
    require(window.session().projectGeneration() > generation && widebandDisplayReady(window),
            "W6 visible file picker did not reopen the project");
    const auto* reopened = findAnnotation(*window.session().activeFile(), manualId);
    require(reopened && *reopened->originEvidence == originalEvidence &&
                activeSegment(*reopened)->range.time.end == newEnd &&
                window.session().activeFile()->channels.back().sourceSnapshot &&
                *window.session().activeFile()->channels.back().sourceSnapshot == *channel.sourceSnapshot,
            "W6 visible save/reopen changed geometry, origin or channel snapshot");
    const auto reopenedDdc = channelsDdc(window, window.session().activeFile()->channels.back());
    require(reopenedDdc["outputSha256"] == ddc["outputSha256"], "W6 reopen changed actual DDC samples");
    report["v6ReopenVerified"] = true;
    clickControl("contextDisplay");
    scene("w6-saved-reopened-display-context");
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
    report["failedPng"] = output.filePath("failed-native-diagnostic.png");
    window.grab().save(report["failedPng"].toString());
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
