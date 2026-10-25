#pragma once

// Included inside ui_capture.cpp's anonymous namespace, after the wideband
// helpers and before ux_r3_capture.h. All dependencies are already global in
// ui_capture.cpp; do not include Qt/project headers inside this namespace.
// Call after the real SigMF source has automatically imported four labels.
// The caller must check the returned "pass" field.
QJsonObject uxR3ManualLabelEntryProbe(MainWindow& window,
                                    const QString& knownLabelPath,
                                    const QDir& run) {
  constexpr int scenarioTimeoutMs = 30000;
  const QString reportPath = run.filePath("manual-label-entry-report.json");
  QJsonArray scenarios;
  QJsonObject report{{"schemaVersion", 1}, {"pass", false},
      {"mode", "ux-r3-manual-label-entry-native"},
      {"platform", QGuiApplication::platformName()},
      {"qtVersion", QString::fromLatin1(qVersion())},
      {"binaryPath", QCoreApplication::applicationFilePath()},
      {"knownLabelPath", knownLabelPath}, {"reportPath", reportPath},
      {"startedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {"scenarioTimeoutMs", scenarioTimeoutMs}, {"expectedScenarios", 6},
      {"inputRoute", "QTest mouse input through actual QWindow hit testing"},
      {"filePickerEvidence", "visible Windows Qt QFileDialog; not Windows Shell picker"},
      {"windowsShellPickerTested", false},
      {"nativeQtWindowsVerified", false},
      {"status", "running"}};
  const bool previousDialogAttribute = qApp->testAttribute(Qt::AA_DontUseNativeDialogs);
  struct RestoreLabelDialogAttribute {
    bool previous;
    ~RestoreLabelDialogAttribute() {
      qApp->setAttribute(Qt::AA_DontUseNativeDialogs, previous);
    }
  } restore{previousDialogAttribute};
  const QPointer<QToolButton> initialBottomToggle = window.findChild<QToolButton*>("resultsToggle");
  const bool initialBottomExpanded = initialBottomToggle && initialBottomToggle->isChecked();
  QPointer<QPushButton> initialBottomTab;
  for (const auto* name : {"resultTab", "taskTab", "logTab"})
    if (auto* tab = window.findChild<QPushButton*>(name); tab && tab->isChecked())
      initialBottomTab = tab;
  report["bottomPanelBefore"] = QJsonObject{
      {"expanded", initialBottomExpanded},
      {"tab", initialBottomTab ? initialBottomTab->objectName() : QString()}};
  const auto write = [&] {
    report["scenarios"] = scenarios;
    report["completedScenarios"] = scenarios.size();
    QFile output(reportPath);
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.flush();
  };
  const auto snapshot = [&] {
    const auto& project = window.session().project();
    const auto* file = window.session().activeFile();
    require(file, "Manual label probe lost the active source");
    return QJsonObject{
        {"stateToken", QString::fromLatin1(ProjectStore::stateToken(project).toHex())},
        {"generation", QString::number(window.session().projectGeneration())},
        {"projectId", QString::fromStdString(project.projectId)},
        {"activeFileId", QString::fromStdString(project.activeFileId)},
        {"activeChannelId", QString::fromStdString(project.activeChannelId)},
        {"labels", widebandAnnotations(*file)}, {"sourceSets", widebandSources(*file)},
        {"counts", widebandSourceCounts(*file)}};
  };
  try {
    require(initialBottomToggle && initialBottomTab,
            "Cannot snapshot the initial bottom panel expansion and selected tab");
    require(run.isAbsolute() && run.exists(), "Manual label evidence requires an existing absolute run directory");
    require(write(), "Cannot checkpoint manual label probe report");
    auto* screen = window.screen();
    require(QGuiApplication::platformName() == "windows" && screen &&
                window.windowHandle() && window.isFullScreen() &&
                window.windowHandle()->isExposed() &&
                displayPixelSize(screen) == QSize(3840, 2160) &&
                window.size() == QSize(2560, 1440) &&
                std::abs(window.devicePixelRatioF() - 1.5) < .001,
            "Manual label probe requires the runner's actual native 4K/150% fullscreen window");
    report["screen"] = widebandScreenJson(screen);
    require(QFileInfo(knownLabelPath).isFile() &&
                knownLabelPath.endsWith(".sigmf-meta", Qt::CaseSensitive),
            "Manual label probe needs the actual paired SigMF meta");
    const auto* initialFile = window.session().activeFile();
    require(initialFile && !initialFile->metadata.demo &&
                initialFile->metadata.availability.status == LoadStatus::Ready &&
                widebandSourceCounts(*initialFile)["importedLabels"].toInt() == 4,
            "Manual label probe must follow automatic import of the four actual SigMF labels");
    const QString targetName = QString::fromStdString(initialFile->metadata.name);
    const QString targetPath = QString::fromStdString(initialFile->metadata.path);
    report["targetName"] = targetName;
    report["targetPath"] = targetPath;
    report["knownLabelSha256"] = widebandFileHash(knownLabelPath);

    auto* action = child<QAction>(window, "importLabelsAction");
    auto* bottom = child<QPushButton>(window, "importLabels");
    require(action->isEnabled() && action->isVisible() && action->text() == bottom->text(),
            "Menu and bottom import entries must expose the same enabled action wording");
    auto* resultsTab = child<QPushButton>(window, "resultTab");
    if (!bottom->isVisible()) widebandNativeClick(resultsTab);
    widebandScrollTo(bottom);
    widebandNativeReachable(bottom);
    const auto baseline = snapshot();
    report["before"] = baseline;

    QTemporaryDir fixtureDirectory(run.filePath("manual-label-input-XXXXXX"));
    require(fixtureDirectory.isValid(), "Cannot create the unknown TXT cancellation fixture");
    const QString unknownPath = fixtureDirectory.filePath("unknown-labels.txt");
    const QByteArray unknownText("QPSK 0 100 -100000 100000\n");
    {
      QFile fixture(unknownPath);
      require(fixture.open(QIODevice::WriteOnly) && fixture.write(unknownText) == unknownText.size(),
              "Cannot write the unknown TXT fixture");
    }
    report["unknownTxt"] = QJsonObject{{"path", unknownPath},
        {"text", QString::fromUtf8(unknownText)}, {"sha256", widebandFileHash(unknownPath)},
        {"purpose", "synthetic mapping/cancellation input; not RF evidence"},
        {"removedAfterProbe", true}};
    qApp->setAttribute(Qt::AA_DontUseNativeDialogs, true);

    // Each input click below reaches the real public entry, file chooser,
    // parser/mapping, preview and publication path. No business import is called.
    for (bool menuEntry : {true, false}) {
      for (const QString decision : {QString("known-dedup-ok"), QString("unknown-mapping-cancel"),
                                     QString("unknown-preview-cancel")}) {
        const bool known = decision == "known-dedup-ok";
        const bool mappingCancel = decision == "unknown-mapping-cancel";
        const QString selectedPath = known ? knownLabelPath : unknownPath;
        const QString name = (menuEntry ? "menu-" : "bottom-") + decision;
        QJsonObject scenario{{"name", name}, {"pass", false},
            {"entryObjectName", menuEntry ? "importLabelsAction" : "importLabels"},
            {"selectedPath", selectedPath}, {"decision", decision},
            {"failureCleanupUsed", false},
            {"expectedFinalState", "original four labels and all source records unchanged"}};
        QJsonArray captures, steps, progressDialogs;
        QElapsedTimer elapsed;
        elapsed.start();
        QString failure;
        bool timedOut = false, busy = false;
        int next = 0, entrySignals = 0;
        QObject lifetime;
        QTimer poll, watchdog;
        poll.setInterval(40);
        watchdog.setInterval(100);
        const auto closeForFailure = [&] {
          // Failure cleanup only. Never count this as a tested Cancel click.
          if (auto* popup = QApplication::activePopupWidget()) popup->close();
          if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            if (modal->inherits("QProgressDialog"))
              QMetaObject::invokeMethod(modal, "canceled", Qt::DirectConnection);
            modal->reject();
          }
          scenario["failureCleanupUsed"] = true;
        };
        const auto place = [&](QDialog& dialog) {
          require(dialog.isVisible() && dialog.windowHandle(), "Expected a visible native Qt dialog");
          dialog.windowHandle()->setScreen(screen);
          dialog.move(screen->geometry().center() - dialog.rect().center());
          dialog.raise();
          dialog.activateWindow();
          require(QTest::qWaitForWindowExposed(dialog.windowHandle(), 1500) && waitUntil([&] {
            return dialog.screen() == screen && dialog.windowHandle()->isExposed() &&
                   std::abs(dialog.devicePixelRatioF() - 1.5) < .001 &&
                   screen->geometry().contains(dialog.frameGeometry());
          }, 1500), "Label dialog did not settle on the runner's actual 4K/150% monitor");
        };
        const auto capture = [&](QDialog& dialog, const QString& stage) {
          place(dialog);
          QTest::qWait(100);
          require(snapshot() == baseline, "Label import mutated project/evidence before confirmation");
          const QString png = run.filePath("manual-label-" + name + "-" + stage + ".png");
          // Read the visible native window, rather than QWidget::render or a
          // hidden/offscreen dialog. This is Qt's picker, not the Shell picker.
          const auto image = screen->grabWindow(dialog.winId(), 0, 0, dialog.width(), dialog.height());
          const bool saved = !image.isNull() && image.save(png);
          captures.append(QJsonObject{{"stage", stage}, {"png", png}, {"pngSaved", saved},
              {"captureKind", "actual native Windows Qt window client area"},
              {"windowsShellPicker", false}, {"windowTitle", dialog.windowTitle()},
              {"objectName", dialog.objectName()}, {"nativeExposed", dialog.windowHandle()->isExposed()},
              {"logicalSize", sizeJson(dialog.size())}, {"pixelSize", sizeJson(image.size())},
              {"devicePixelRatio", image.devicePixelRatioF()},
              {"screen", widebandScreenJson(dialog.screen())}});
          require(saved && std::abs(image.devicePixelRatioF() - 1.5) < .001 &&
                      std::abs(image.width() - qRound(dialog.width() * 1.5)) <= 1 &&
                      std::abs(image.height() - qRound(dialog.height() * 1.5)) <= 1,
                  "Could not capture the actual label dialog at native monitor DPI");
        };
        const auto choose = [&](QDialog& dialog) {
          auto* chooser = qobject_cast<QFileDialog*>(&dialog);
          require(chooser && chooser->objectName() == "importLabelFileDialog" &&
                      chooser->windowTitle().contains(targetName) &&
                      chooser->fileMode() == QFileDialog::ExistingFile,
                  "Manual label chooser does not identify the active target source");
          place(dialog);
          chooser->setDirectory(QFileInfo(selectedPath).absolutePath());
          // QFileSystemModel loads asynchronously; selectFile() on a visible
          // picker can be cleared by its pending directory change. Enter the
          // absolute path through the actual filename editor instead.
          auto* filename = child<QLineEdit>(dialog, "fileNameEdit");
          require(waitUntil([&] { return filename->isVisible() && filename->isEnabled(); }, 2000),
                  "File-name editor did not become available");
          QTest::qWait(150);
          widebandNativeClick(filename);
          QTest::keyClick(dialog.windowHandle(), Qt::Key_A, Qt::ControlModifier);
          for (const auto character : QDir::toNativeSeparators(selectedPath).toLatin1())
            QTest::keyClick(dialog.windowHandle(), character);
          QTest::keyClick(dialog.windowHandle(), Qt::Key_Tab);
          auto* buttons = chooser->findChild<QDialogButtonBox*>();
          auto* open = buttons ? buttons->button(QDialogButtonBox::Open) : nullptr;
          require(open && waitUntil([&] {
            const auto selected = chooser->selectedFiles();
            return open->isVisible() && open->isEnabled() && selected.size() == 1 &&
                   QFileInfo(selected.front()).canonicalFilePath() == QFileInfo(selectedPath).canonicalFilePath();
          }, 2000), "The real file chooser did not select the requested existing label");
          scenario["chooserTitleShowsTarget"] = true;
          scenario["chooserSelectedPath"] = chooser->selectedFiles().front();
          capture(dialog, "file-picker");
          widebandNativeClick(open);
          require(!dialog.isVisible(), "Native Open click did not finish the file chooser");
          scenario["fileOpenNativeClick"] = true;
        };
        const auto mapping = [&](QDialog& dialog) {
          require(dialog.objectName() == "labelMappingDialog", "Unknown TXT did not open field mapping");
          auto* raw = child<QPlainTextEdit>(dialog, "labelMappingPreview");
          require(raw->toPlainText() == QString::fromUtf8(unknownText), "Mapping preview changed the unknown TXT");
          place(dialog);
          if (mappingCancel) {
            auto* delimiter = child<QComboBox>(dialog, "labelDelimiter");
            const int index = delimiter->findText("空白", Qt::MatchExactly);
            require(index >= 0, "Unknown TXT delimiter choice is unavailable");
            widebandNativeSelect(delimiter, index);
            require(delimiter->currentIndex() == index, "Partial mapping draft was not selected");
            scenario["cancelledPartialMappingDraft"] = true;
          }
          if (!mappingCancel) {
            const auto select = [&](const char* objectName, const QString& text) {
              auto* combo = child<QComboBox>(dialog, objectName);
              const int index = combo->findText(text, Qt::MatchExactly);
              require(index >= 0, "Mapping choice is unavailable: " + text);
              widebandNativeSelect(combo, index);
              require(combo->currentIndex() == index, "Native mapping choice did not apply");
            };
            const auto column = [&](const char* objectName, int value) {
              auto* spin = child<QWidget>(dialog, objectName);
              auto* editor = spin->findChild<QLineEdit*>();
              require(editor && value >= 1 && value <= 5, "Mapping column editor is unavailable");
              widebandScrollTo(spin);
              widebandNativeClick(editor);
              QTest::keyClick(dialog.windowHandle(), Qt::Key_A, Qt::ControlModifier);
              QTest::keyClick(dialog.windowHandle(), static_cast<Qt::Key>(Qt::Key_0 + value));
              QTest::keyClick(dialog.windowHandle(), Qt::Key_Tab);
              require(spin->property("value").toInt() == value, "Native mapping column did not commit");
            };
            select("labelDelimiter", "空白");
            column("labelClassColumn", 1);
            select("labelTimeUnit", "样本");
            column("labelStartColumn", 2);
            column("labelEndColumn", 3);
            select("labelInterval", "[起点, 终点)");
            select("labelIndexOrigin", "从 0 起（秒也选此项）");
            select("labelFrequencyLayout", "频率下界 / 上界");
            column("labelLowerColumn", 4);
            column("labelUpperColumn", 5);
            select("labelFrequencyUnit", "Hz");
            select("labelFrequencyReference", "基带偏移（加一次 Fc）");
            scenario["mappingConfiguredThroughVisibleControls"] = true;
          }
          capture(dialog, mappingCancel ? "mapping-cancel" : "mapping-confirm");
          auto* buttons = dialog.findChild<QDialogButtonBox*>();
          require(buttons, "Mapping decision button box is missing");
          auto* button = buttons->button(mappingCancel ? QDialogButtonBox::Cancel : QDialogButtonBox::Ok);
          require(button, "Mapping decision button is missing");
          widebandNativeClick(button);
          require(!dialog.isVisible(), "Native mapping decision did not finish the mapping dialog");
          scenario["mappingDecisionNativeClick"] = true;
        };
        const auto preview = [&](QDialog& dialog) {
          require(dialog.windowTitle() == "标签导入预览" && !qobject_cast<QMessageBox*>(&dialog),
                  "Expected the actual batch preview dialog");
          auto* target = child<QLabel>(dialog, "labelImportTarget");
          auto* records = child<QTableWidget>(dialog, "labelImportPreviewRecords");
          require(target->isVisible() && target->text().contains(targetName) &&
                      target->text().contains(targetPath), "Label preview lost its actual target binding");
          require(records->rowCount() == (known ? 4 : 1), "Label preview does not show the expected parsed objects");
          scenario["previewTarget"] = target->text();
          scenario["previewObjectCount"] = records->rowCount();
          capture(dialog, known ? "preview-ok" : "preview-cancel");
          auto* buttons = dialog.findChild<QDialogButtonBox*>();
          require(buttons, "Preview decision button box is missing");
          auto* button = buttons->button(known ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel);
          require(button, "Preview decision button is missing");
          widebandNativeClick(button);
          require(!dialog.isVisible(), "Native preview decision did not finish the preview dialog");
          scenario["previewDecisionNativeClick"] = true;
        };
        struct ModalStep {
          QString name;
          std::function<bool(QDialog*)> matches;
          std::function<void(QDialog&)> act;
        };
        std::vector<ModalStep> script;
        script.push_back({"file-picker", [](QDialog* dialog) {
          return dialog->objectName() == "importLabelFileDialog";
        }, choose});
        if (!known) script.push_back({"mapping", [](QDialog* dialog) {
          return dialog->objectName() == "labelMappingDialog";
        }, mapping});
        if (!mappingCancel) script.push_back({"preview", [](QDialog* dialog) {
          return dialog->windowTitle() == "标签导入预览";
        }, preview});
        QObject::connect(&watchdog, &QTimer::timeout, &lifetime, [&] {
          if (elapsed.elapsed() >= scenarioTimeoutMs && failure.isEmpty()) {
            timedOut = true;
            failure = "Native label scenario exceeded its modal deadline";
          }
          if (!failure.isEmpty()) closeForFailure();
        });
        QObject::connect(&poll, &QTimer::timeout, &lifetime, [&] {
          if (busy || !failure.isEmpty()) return;
          auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
          if (!dialog || !dialog->isVisible()) return;
          if (dialog->inherits("QProgressDialog")) {
            if (!progressDialogs.contains(dialog->windowTitle())) progressDialogs.append(dialog->windowTitle());
            return;
          }
          if (next >= static_cast<int>(script.size()) || !script[next].matches(dialog)) {
            failure = "Unexpected modal in native label scenario: " + dialog->windowTitle();
            closeForFailure();
            return;
          }
          busy = true;
          const int index = next++;
          try {
            script[index].act(*dialog);
            steps.append(QJsonObject{{"step", script[index].name}, {"pass", true},
                {"elapsedMs", elapsed.elapsed()}});
          } catch (const std::exception& error) {
            failure = QString::fromUtf8(error.what());
            steps.append(QJsonObject{{"step", script[index].name}, {"pass", false},
                {"error", failure}, {"elapsedMs", elapsed.elapsed()}});
            closeForFailure();
          }
          busy = false;
        });
        if (menuEntry)
          QObject::connect(action, &QAction::triggered, &lifetime, [&](bool) { ++entrySignals; });
        else
          QObject::connect(bottom, &QPushButton::clicked, &lifetime, [&](bool) { ++entrySignals; });
        poll.start();
        watchdog.start();
        try {
          require(snapshot() == baseline, "Project changed before the next manual label entry");
          if (menuEntry) {
            auto* bar = window.menuBar();
            QAction* menuAction = nullptr;
            for (auto* candidate : bar->actions())
              if (candidate->menu() && candidate->menu()->actions().contains(action)) {
                require(!menuAction, "Import action belongs to multiple top-level menus");
                menuAction = candidate;
              }
            require(menuAction && menuAction->isEnabled(), "File-menu import action is not reachable");
            auto* menu = menuAction->menu();
            widebandNativeReachable(bar);
            const auto barRect = bar->actionGeometry(menuAction);
            require(!barRect.isEmpty() && bar->rect().contains(barRect), "File-menu title is clipped");
            QTest::mouseClick(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                window.mapFromGlobal(bar->mapToGlobal(barRect.center())));
            require(waitUntil([&] { return menu->isVisible() && menu->windowHandle(); }, 2000),
                    "Native File-menu title click did not open the actual menu");
            const auto actionRect = menu->actionGeometry(action);
            require(!actionRect.isEmpty() && menu->rect().contains(actionRect) &&
                        menu->actionAt(actionRect.center()) == action,
                    "Native import menu row is clipped or targets another action");
            QTest::mouseClick(menu->windowHandle(), Qt::LeftButton, Qt::NoModifier, actionRect.center());
          } else {
            widebandScrollTo(bottom);
            widebandNativeClick(bottom);
          }
          require(waitUntil([&] {
            return !failure.isEmpty() || (next == static_cast<int>(script.size()) &&
                !QApplication::activeModalWidget() && !QApplication::activePopupWidget());
          }, scenarioTimeoutMs), "Manual label modal sequence never finished");
          require(failure.isEmpty(), failure);
          require(entrySignals == 1 && next == static_cast<int>(script.size()),
                  "Actual entry signal or expected modal steps were not delivered exactly once");
          // Let the public entry return and its refresh/queued work settle.
          QTest::qWait(150);
          const auto after = snapshot();
          scenario["after"] = after;
          scenario["stateTokenUnchanged"] = after["stateToken"] == baseline["stateToken"];
          scenario["labelsUnchanged"] = after["labels"] == baseline["labels"];
          scenario["sourceSetsUnchanged"] = after["sourceSets"] == baseline["sourceSets"];
          require(after == baseline, "Manual label confirmation/cancellation changed the original project or label evidence");
          scenario["pass"] = true;
        } catch (const std::exception& error) {
          if (failure.isEmpty()) failure = QString::fromUtf8(error.what());
          if (elapsed.elapsed() >= scenarioTimeoutMs) timedOut = true;
          closeForFailure();
          scenario["error"] = failure;
        }
        poll.stop();
        watchdog.stop();
        scenario["entrySignalCount"] = entrySignals;
        scenario["steps"] = steps;
        scenario["progressDialogTitles"] = progressDialogs;
        scenario["captures"] = captures;
        scenario["timedOut"] = timedOut;
        scenario["elapsedMs"] = elapsed.elapsed();
        scenarios.append(scenario);
        require(write(), "Could not checkpoint native label scenario evidence");
        require(scenario["pass"].toBool(), failure);
      }
    }
    require(snapshot() == baseline && scenarios.size() == 6,
            "Manual label probe did not preserve its original four-label source across all six scenarios");
    report["after"] = snapshot();
    report["nativeQtWindowsVerified"] = true;
    report["pass"] = true;
    report["status"] = "passed";
  } catch (const std::exception& error) {
    report["error"] = QString::fromUtf8(error.what());
    report["status"] = "failed";
  }
  // Restore UI layout before the caller captures its next chart scene. A tab
  // click expands the panel, so restore the tab first and expansion last.
  // This also runs after a failed scenario; cleanup failure must not pass.
  try {
    require(initialBottomToggle && initialBottomTab && !QApplication::activeModalWidget(),
            "Cannot restore bottom panel while its controls or modal context are unavailable");
    if (!initialBottomTab->isChecked()) widebandNativeClick(initialBottomTab.data());
    if (initialBottomToggle->isChecked() != initialBottomExpanded)
      widebandNativeClick(initialBottomToggle.data());
    auto* content = window.findChild<QWidget*>("bottomContent");
    require(content && waitUntil([&] {
      return initialBottomTab->isChecked() &&
             initialBottomToggle->isChecked() == initialBottomExpanded &&
             content->isVisible() == initialBottomExpanded;
    }, 2000), "Bottom panel did not return to its original expansion and tab");
    report["bottomPanelRestored"] = true;
    report["bottomPanelAfter"] = QJsonObject{
        {"expanded", initialBottomToggle->isChecked()}, {"tab", initialBottomTab->objectName()}};
  } catch (const std::exception& error) {
    report["bottomPanelRestored"] = false;
    report["bottomPanelRestoreError"] = QString::fromUtf8(error.what());
    report["pass"] = false;
    report["status"] = "failed";
  }
  report["finishedUtc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  if (!write()) {
    report["pass"] = false;
    report["status"] = "failed";
    report["reportWriteFailed"] = true;
  }
  return report;
}
