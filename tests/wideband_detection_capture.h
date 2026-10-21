#pragma once

// Included inside ui_capture.cpp's acceptance namespace, after Capture and its
// native helpers. Business/Qt headers must be included globally by
// ui_capture.cpp.

QJsonObject widebandScreenJson(QScreen* screen) {
  auto result = screenJson(screen);
  result["devicePath"] = displayDeviceName(screen);
#ifdef Q_OS_WIN
  UINT32 pathCount = 0, modeCount = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount,
                                  &modeCount) != ERROR_SUCCESS)
    return result;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                         &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
    return result;
  for (UINT32 i = 0; i < pathCount; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = static_cast<UINT32>(sizeof(source));
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
        QString::fromWCharArray(source.viewGdiDeviceName)
                .compare(displayDeviceName(screen), Qt::CaseInsensitive) != 0)
      continue;
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = static_cast<UINT32>(sizeof(target));
    target.header.adapterId = paths[i].targetInfo.adapterId;
    target.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
      result["friendlyName"] =
          QString::fromWCharArray(target.monitorFriendlyDeviceName);
      result["monitorDevicePath"] =
          QString::fromWCharArray(target.monitorDevicePath);
      result["identityMethod"] = "Windows active DisplayConfig source/target";
      break;
    }
  }
#endif
  return result;
}

QString widebandFileHash(const QString& path) {
  QFile input(path);
  require(input.open(QIODevice::ReadOnly),
          "Cannot read actual capture asset: " + path);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  require(hash.addData(&input),
          "Could not hash the complete actual capture asset");
  return QString::fromLatin1(hash.result().toHex());
}

QString widebandRepresentativeDataPath() {
  const auto local = QDir(QCoreApplication::applicationDirPath())
                         .filePath(
                             "../../resources/sigmf_data_demo/"
                             "0000000.sigmf-data");
  if (QFileInfo::exists(local)) return QFileInfo(local).canonicalFilePath();
  return "W:/wbSig53_rfbench20m/train/baseline/0000000.sigmf-data";
}

QJsonObject widebandSourceCounts(const FileState& file) {
  int imported = 0, manual = 0, algorithm = 0;
  for (const auto& annotation : file.marks) {
    if (annotation.deleted) continue;
    switch (annotation.sourceKind) {
      case AnnotationSource::ImportedLabel:
        ++imported;
        break;
      case AnnotationSource::Manual:
        ++manual;
        break;
      case AnnotationSource::Algorithm:
        ++algorithm;
        break;
    }
  }
  return {{"importedLabels", imported},
          {"manual", manual},
          {"algorithm", algorithm},
          {"total", imported + manual + algorithm}};
}

bool widebandDisplayReady(MainWindow& window) {
  return waitUntil(
      [&] {
        const auto* file = window.session().activeFile();
        if (!file || file->metadata.demo ||
            file->metadata.availability.status != LoadStatus::Ready ||
            !window.session().spectrogram(file->metadata.id))
          return false;
        for (auto* plot : window.findChildren<PlotWidget*>())
          if (plot->isVisible() &&
              (!plot->isDisplaySettled() || !plot->gpuReady() ||
               !plot->completedFrameCount()))
            return false;
        return true;
      },
      60'000);
}

QRect widebandVisibleRect(QWidget* widget, QWidget& window);

void widebandScrollTo(QWidget* widget) {
  QScrollArea* scroll = nullptr;
  for (auto* owner = widget->parentWidget(); owner;
       owner = owner->parentWidget())
    if (auto* area = qobject_cast<QScrollArea*>(owner)) {
      scroll = area;
      break;
    }
  const bool exposed = waitUntil([widget, scroll] {
            // Wrapped source fields can change the layout after selection.
            // Re-evaluate scrolling against the current native geometry.
            if (scroll) {
              const auto topLeft = widget->mapTo(scroll->widget(), QPoint());
              // Ask for the complete control rectangle; a spin box's input
              // cursor rectangle can be smaller than its painted frame.
              scroll->ensureVisible(topLeft.x() + widget->width() / 2,
                                    topLeft.y() + widget->height() / 2,
                                    widget->width() / 2 + 12,
                                    widget->height() / 2 + 12);
            }
            const auto visible = widebandVisibleRect(widget, *widget->window());
            const QRect bounds(widget->mapTo(widget->window(), QPoint()), widget->size());
            const bool fits = scroll && widget->width() <= scroll->viewport()->width() &&
                              widget->height() <= scroll->viewport()->height();
            return widget->isVisible() && (fits ? visible == bounds : !visible.isEmpty());
          });
  require(exposed,
          QString("Native scroll did not expose widget %1: visible=%2, "
                  "geometry=%3,%4 %5x%6, scroll=%7/%8")
              .arg(widget->objectName())
              .arg(widget->isVisible())
              .arg(widget->x())
              .arg(widget->y())
              .arg(widget->width())
              .arg(widget->height())
              .arg(scroll ? scroll->verticalScrollBar()->value() : -1)
              .arg(scroll ? scroll->verticalScrollBar()->maximum() : -1));
}

// Shared by the historical R/U runners. Never send input to an editor until
// its actual page/fold has been exposed and native hit testing can reach it.
void widebandNativeReachable(QWidget* widget, bool requireEnabled = true) {
  require(widget && widget->isVisible() && !widget->size().isEmpty() &&
              (!requireEnabled || widget->isEnabled()),
          "Native control is unavailable: " +
              (widget ? widget->objectName() : QString("<null>")));
  auto* root = widget->window();
  const QRect bounds(widget->mapTo(root, QPoint()), widget->size());
  require(widebandVisibleRect(widget, *root) == bounds,
          "Native control is clipped: " + widget->objectName());
  auto* hit = root->childAt(bounds.center());
  require(hit && (hit == widget || widget->isAncestorOf(hit)),
          "Native control is obstructed: " + widget->objectName());
  require(root->windowHandle(), "Native control has no exposed window");
}

void widebandNativeClick(QWidget* widget) {
  widebandNativeReachable(widget);
  QPoint point = widget->rect().center();
  if (auto* checkbox = qobject_cast<QCheckBox*>(widget)) {
    QStyleOptionButton option;
    option.initFrom(checkbox);
    option.text = checkbox->text();
    point = checkbox->style()->subElementRect(QStyle::SE_CheckBoxIndicator,
                                              &option, checkbox).center();
    require(widget->rect().contains(point), "Native checkbox indicator is clipped");
  }
  auto* root = widget->window();
  QTest::mouseClick(root->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                    root->mapFromGlobal(widget->mapToGlobal(point)));
  QCoreApplication::processEvents();
}

void widebandNativeSelect(QComboBox* combo, int index,
                           const std::function<void()>& beforeActivation = {}) {
  widebandScrollTo(combo);
  widebandNativeReachable(combo);
  require(index >= 0 && index < combo->count(),
          "Native combo choice is absent: " + combo->objectName());
  auto* root = combo->window();
  const QPoint arrow(combo->width() - 8, combo->height() / 2);
  QTest::mouseClick(root->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                    root->mapFromGlobal(combo->mapToGlobal(arrow)));
  auto* view = combo->view();
  require(waitUntil([&] { return view->isVisible(); }),
          "Native dropdown did not open: " + combo->objectName());
  QTest::qWait(QApplication::doubleClickInterval() + 50);
  const auto item = combo->model()->index(index, combo->modelColumn(),
                                         combo->rootModelIndex());
  require(combo->model()->flags(item).testFlag(Qt::ItemIsEnabled),
          "Native dropdown choice is disabled");
  view->scrollTo(item);
  QCoreApplication::processEvents();
  QRect rect;
  const bool rowReady = waitUntil([&] {
    view->scrollTo(item);
    rect = view->visualRect(item);
    return !rect.isEmpty() && view->viewport()->rect().contains(rect);
  }, 1500);
  require(rowReady,
          QString("Native dropdown choice is clipped: %1 row=%2,%3 %4x%5 viewport=%6x%7")
              .arg(combo->objectName()).arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height())
              .arg(view->viewport()->width()).arg(view->viewport()->height()));
  auto* popup = view->window();
  const auto point = popup->mapFromGlobal(
      view->viewport()->mapToGlobal(rect.center()));
  auto* hit = popup->childAt(point);
  require(popup->windowHandle() && hit &&
              (hit == view->viewport() || view->viewport()->isAncestorOf(hit)),
          "Native dropdown row is obstructed");
  QTest::mouseMove(popup->windowHandle(), point, 20);
  QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
  QTest::qWait(60);
  if (beforeActivation) beforeActivation();
  QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
  require(waitUntil([&] {
            return !view->isVisible() && combo->currentIndex() == index;
          }),
          "Native dropdown did not apply the chosen row: " + combo->objectName());
}

void widebandShowInspector(MainWindow& window, const char* tabName) {
  auto* rail = child<QPushButton>(window, "propRailOpen");
  if (rail->isVisible()) widebandNativeClick(rail);
  auto* tab = child<QPushButton>(window, tabName);
  if (!tab->isChecked()) widebandNativeClick(tab);
  require(waitUntil([&] { return tab->isVisible() && tab->isChecked(); }),
          "Native inspector page did not open: " + QString::fromLatin1(tabName));
}

void widebandDetectionFold(MainWindow& window, const char* name, bool open = true) {
  widebandShowInspector(window, "contextDetection");
  auto* toggle = child<QToolButton>(window, name);
  widebandScrollTo(toggle);
  if (toggle->isChecked() != open) widebandNativeClick(toggle);
  require(waitUntil([&] { return toggle->isChecked() == open; }),
          "Native detection fold did not change: " + QString::fromLatin1(name));
}

QWidget* widebandDetectionControl(MainWindow& window, const char* name) {
  widebandShowInspector(window, "contextDetection");
  auto* widget = child<QWidget>(window, name);
  for (auto* owner = widget->parentWidget(); owner; owner = owner->parentWidget()) {
    const auto group = owner->objectName();
    if (group == "detectionAnalysisParameters" ||
        group == "detectionPostprocessParameters" ||
        group == "detectionResourceParameters" ||
        group == "detectionCalculationDetails" ||
        group == "detectionDiagnosticsSection") {
      const auto toggle = group == "detectionDiagnosticsSection"
                              ? QString("detectionDiagnosticsToggle")
                              : group + "Toggle";
      widebandDetectionFold(window, qPrintable(toggle));
      break;
    }
  }
  widebandScrollTo(widget);
  widebandNativeReachable(widget);
  return widget;
}

// The old profile key encoded both values. Keep that mapping explicit while
// selecting both new controls through their real native popup rows.
void widebandSelectModelSpectrum(MainWindow& window, const QString& key) {
  widebandShowInspector(window, "contextDetection");
  const auto separator = key.lastIndexOf(':');
  const auto modelId = separator < 0 ? key : key.left(separator);
  bool valid = true;
  const auto spectrum = separator < 0 ? 0 : key.mid(separator + 1).toInt(&valid);
  require(valid && spectrum >= 0 && spectrum <= 4, "Invalid model/spectrum key");
  auto* model = child<QComboBox>(window, "detectionModel");
  widebandNativeSelect(model, model->findData(modelId));
  auto* mode = child<QComboBox>(window, "detectionSpectrumMode");
  if (mode->isEnabled()) widebandNativeSelect(mode, mode->findData(spectrum));
  else require(mode->isVisible() && spectrum == 0 && mode->count() == 1 &&
                   mode->currentData().toInt() == 0,
               "The explicitly selected fixture/experimental model must retain its sole default spectrum");
  const auto request = child<DetectionCard>(window, "detectionCard")->request();
  require(request.profile.id == modelId.toStdString() &&
              static_cast<int>(request.spectrumMode) == spectrum,
          "Visible model/spectrum selection differs from the frozen request");
}

void widebandMenuAction(MainWindow& window, const char* name) {
  auto* action = child<QAction>(window, name);
  require(action->isEnabled(), "Native menu action is disabled");
  auto* bar = window.menuBar();
  QAction* heading = nullptr;
  for (auto* candidate : bar->actions())
    if (candidate->menu() && candidate->menu()->actions().contains(action))
      heading = candidate;
  require(heading && bar->isVisible(), "Native menu heading is unavailable");
  const auto headingRect = bar->actionGeometry(heading);
  require(!headingRect.isEmpty() && bar->rect().contains(headingRect),
          "Native menu heading is clipped");
  QTest::mouseClick(window.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                    window.mapFromGlobal(bar->mapToGlobal(headingRect.center())));
  auto* menu = heading->menu();
  require(waitUntil([&] { return menu->isVisible(); }),
          "Native menu did not open");
  QTest::qWait(QApplication::doubleClickInterval() + 50);
  const auto row = menu->actionGeometry(action);
  require(!row.isEmpty() && menu->rect().contains(row),
          "Native menu action is clipped");
  QTest::mouseMove(menu, row.center(), 20);
  QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, row.center());
  require(waitUntil([&] { return !menu->isVisible(); }),
          "Native menu action did not close its menu");
}

void widebandChartMenuAction(PlotWidget* plot, const char* name) {
  widebandNativeReachable(plot);
  const auto point = plot->plotRect().center().toPoint();
  bool handled = false;
  QString failure;
  QTimer::singleShot(100, plot, [&] {
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    try {
      require(menu && menu->isVisible(), "Native chart context menu did not open");
      auto* action = child<QAction>(*menu, name);
      require(action->isEnabled(), "Native chart context action is unavailable");
      QTest::qWait(QApplication::doubleClickInterval() + 50);
      const auto row = menu->actionGeometry(action);
      require(!row.isEmpty() && menu->rect().contains(row),
              "Native chart context action is clipped");
      QTest::mouseMove(menu, row.center(), 20);
      QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, row.center());
    } catch (const std::exception& error) {
      failure = QString::fromUtf8(error.what());
      if (menu) menu->close();
    }
    handled = true;
  });
  // QtTest mouse buttons do not synthesize a context-menu event. Send the
  // public mouse-context event to the visible plot, then click its real menu.
  QContextMenuEvent event(QContextMenuEvent::Mouse, point, plot->mapToGlobal(point));
  QCoreApplication::sendEvent(plot, &event);
  require(waitUntil([&] { return handled; }) && failure.isEmpty(), failure);
}

void widebandOpenChartSettings(MainWindow& window, const char* name) {
  auto* entry = child<QWidget>(window, name);
  const auto frameName = QString::fromLatin1(name) + "Popover";
  auto* frame = child<QWidget>(window, qPrintable(frameName));
  if (frame->isVisible()) return;
  if (entry->isVisible()) widebandNativeClick(entry);
  else {
    const auto moreName = entry->parentWidget()->objectName() + "Overflow";
    widebandNativeClick(child<QWidget>(window, qPrintable(moreName)));
    const auto alternate = QString::fromLatin1(name) + "OverflowAction";
    widebandNativeClick(child<QWidget>(window, qPrintable(alternate)));
  }
  require(waitUntil([&] { return frame->isVisible(); }),
          "Native chart settings did not open: " + frameName);
}

QJsonObject widebandCurrentFileProperties(MainWindow& window) {
  widebandShowInspector(window, "contextDisplay");
  auto* entry = child<QPushButton>(window, "fileProperties");
  widebandScrollTo(entry);
  widebandNativeClick(entry);
  auto* frame = child<QWidget>(window, "filePropertiesPopover");
  require(waitUntil([&] { return frame->isVisible(); }),
          "The current file properties did not open through their visible entry");
  QJsonObject result;
  for (const auto* name : {"fileName", "sampleRate", "centerFreq", "fullRange"}) {
    auto* value = child<QLabel>(window, name);
    widebandScrollTo(value);
    widebandNativeReachable(value, false);
    require(!value->text().isEmpty(), "Current file property has no readable value");
    result[name] = value->text();
  }
  const auto* file = window.session().activeFile();
  require(file && result["fileName"].toString() ==
                      QString::fromStdString(file->metadata.name),
          "Visible file properties belong to a different source");
  auto* bandwidth = child<QDoubleSpinBox>(window, "effectiveBandwidthMHz");
  widebandScrollTo(bandwidth);
  widebandNativeReachable(bandwidth);
  result["effectiveBandwidthMHz"] = bandwidth->value();
  result["fileId"] = QString::fromStdString(file->metadata.id);
  QTest::keyClick(window.windowHandle(), Qt::Key_Escape);
  require(waitUntil([&] { return !frame->isVisible(); }),
          "The file properties did not close through Escape");
  return result;
}

void widebandScene(Capture& capture, const QString& name, QScreen* screen,
                   QWidget* target = nullptr,
                   bool requiresRenderedData = true) {
  capture.scene(name, target, {}, requiresRenderedData);
  auto record = capture.scenes.last().toObject();
  record["screen"] = widebandScreenJson(screen);
  record["captureKind"] = target ? (target->isWindow() ? "native-dialog-crop"
                                                       : "native-widget-crop")
                                 : "native-main-window";
  record["sourceCounts"] =
      widebandSourceCounts(*capture.window.session().activeFile());
  QJsonArray layers;
  for (int i = 0; i < 3; ++i) {
    auto* layer = child<QCheckBox>(
        capture.window, qPrintable(QString("annotationLayer%1").arg(i)));
    require(layer->isChecked(),
            "The native scene must retain all three annotation layers");
    layers.append(QJsonObject{{"index", i},
                              {"checked", layer->isChecked()},
                              {"label", layer->text()}});
  }
  record["annotationLayers"] = layers;
  capture.scenes[capture.scenes.size() - 1] = record;
  require(record["pass"].toBool(), record["error"].toString());
  require((target ? target->screen() : capture.window.screen()) == screen,
          "Native scene moved away from the selected native 4K monitor");
}

// Exercise the actual optional quality menu rather than triggering an action
// directly. The timer also supports QPushButton::showMenu's nested event loop.
void widebandChooseQuality(MainWindow& window, const char* actionName,
                           Capture* capture = nullptr,
                           QScreen* screen = nullptr,
                           const QString& sceneName = {}) {
  widebandShowInspector(window, "contextAnnotation");
  auto* button = child<QPushButton>(window, "annotationQualityMenu");
  widebandScrollTo(button);
  widebandNativeReachable(button);
  bool handled = false;
  QString failure;
  QTimer::singleShot(100, &window, [&] {
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    try {
      require(menu && menu->objectName() == "annotationQualityActions",
              "The optional quality menu did not open");
      QAction* action = nullptr;
      for (auto* candidate : menu->actions())
        if (candidate->objectName() == actionName) action = candidate;
      require(action && action->isEnabled(),
              "The quality action is unavailable");
      if (capture && screen && !sceneName.isEmpty())
        widebandScene(*capture, sceneName, screen, menu);
      QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                        menu->actionGeometry(action).center());
    } catch (const std::exception& error) {
      failure = QString::fromUtf8(error.what());
      if (menu) menu->close();
    }
    handled = true;
  });
  QTest::mouseClick(button, Qt::LeftButton);
  require(waitUntil([&] { return handled; }) && failure.isEmpty(), failure);
}

QRect widebandVisibleRect(QWidget* widget, QWidget& window) {
  if (!widget->isVisible()) return {};
  auto visible = QRect(widget->mapTo(&window, QPoint()), widget->size());
  for (auto* owner = widget->parentWidget(); owner;
       owner = owner->parentWidget()) {
    visible = visible.intersected(
        QRect(owner->mapTo(&window, QPoint()), owner->size()));
    if (owner == &window) break;
  }
  return visible.intersected(window.rect());
}

QJsonArray widebandAnnotations(const FileState& file) {
  QJsonArray result;
  for (const auto& annotation : file.marks)
    result.append(encodeAnnotation(annotation));
  return result;
}

class WidebandFeedbackPaint final : public QObject {
 public:
  WidebandFeedbackPaint(QWidget& widget, std::function<bool()> ready)
      : widget_(&widget), ready_(std::move(ready)) {
    widget.installEventFilter(this);
  }
  ~WidebandFeedbackPaint() override {
    if (widget_) widget_->removeEventFilter(this);
  }
  int count() const { return count_; }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == widget_ && event->type() == QEvent::Paint &&
        widget_->isVisible() &&
        !widebandVisibleRect(widget_, *widget_->window()).isEmpty() && ready_())
      ++count_;
    return false;
  }

 private:
  QPointer<QWidget> widget_;
  std::function<bool()> ready_;
  int count_ = 0;
};

QJsonArray widebandSources(const FileState& file) {
  QJsonArray result;
  for (const auto& source : file.sourceSets)
    result.append(encodeSourceSet(source));
  return result;
}

void widebandRequireVisibleActions(MainWindow& window) {
  // W4 deliberately shows these fixed controls only on the Detection page.
  // Check that page, then restore the visible annotation/evidence scene.
  QString previous;
  for (const auto* page : {"contextDisplay", "contextDetection",
                            "contextAnnotation", "contextChannel"})
    if (child<QPushButton>(window, page)->isChecked()) previous = page;
  widebandShowInspector(window, "contextDetection");
  for (const auto* name : {"runWidebandDetection", "cancelDetectionCardRun"}) {
    auto* button = child<QPushButton>(window, name);
    const auto bounds = QRect(button->mapTo(&window, QPoint()), button->size());
    require(
        button->isVisible() && bounds.width() > 40 && bounds.height() > 16 &&
            widebandVisibleRect(button, window) == bounds,
        QString("Fixed action is clipped or outside the native viewport: ") +
            name);
    widebandNativeReachable(button, false);
  }
  if (!previous.isEmpty() && previous != "contextDetection")
    widebandShowInspector(window, qPrintable(previous));
}

void widebandResidentCardAtTop(MainWindow& window) {
  // File facts now live on Display/file properties; the resident detection
  // page must expose the basic configuration immediately, with fixed actions.
  widebandShowInspector(window, "contextDetection");
  auto* scroll = child<QScrollArea>(window, "propScroll");
  auto* card = child<QWidget>(window, "detectionCard");
  auto* model = child<QComboBox>(window, "detectionModel");
  scroll->verticalScrollBar()->setValue(0);
  require(waitUntil([&] {
            const auto cardRect = widebandVisibleRect(card, window);
            return scroll->verticalScrollBar()->value() == 0 &&
                   cardRect.height() >= 200 &&
                   widebandVisibleRect(model, window) ==
                       QRect(model->mapTo(&window, QPoint()), model->size());
          }),
          "The first native scene must show the resident detection page at "
          "the top with its model and basic configuration visible");
  widebandNativeReachable(model);
  widebandRequireVisibleActions(window);
}

void widebandCaptureVisibleCard(Capture& capture, QScreen* screen,
                                QWidget* card) {
  const auto visible = widebandVisibleRect(card, capture.window);
  require(visible.width() >= 150 && visible.height() >= 200,
          "DetectionCard does not occupy a real visible native viewport");
  widebandScene(capture, "02-config-card-visible-crop", screen);
  auto record = capture.scenes.last().toObject();
  const auto originalPixels = record["capturedPixelSize"];
  const auto pixmap = capture.window.grab(visible);
  require(
      !pixmap.isNull() && pixmap.save(record["png"].toString()),
      "Could not crop the actual visible DetectionCard from the main window");
  record["captureKind"] = "native-card-visible-crop";
  record["widgetObjectName"] = card->objectName();
  record["mainWindowPixelSize"] = originalPixels;
  record["capturedPixelSize"] = sizeJson(pixmap.size());
  record["capturedLogicalWidgetSize"] = sizeJson(visible.size());
  record["visibleCropLogicalRect"] = QJsonObject{{"x", visible.x()},
                                                 {"y", visible.y()},
                                                 {"width", visible.width()},
                                                 {"height", visible.height()}};
  capture.scenes[capture.scenes.size() - 1] = record;
}

QPlainTextEdit* widebandOpenOriginalFields(MainWindow& window) {
  widebandShowInspector(window, "contextAnnotation");
  auto* evidence = child<QPlainTextEdit>(window, "annotationEvidence");
  if (!evidence->isVisible()) {
    auto* toggle =
        child<QAbstractButton>(window, "annotationOriginalFieldsToggle");
    widebandScrollTo(toggle);
    require(toggle->isVisible() && toggle->isEnabled(),
            "Original evidence fold has no visible enabled native control");
    QTest::mouseClick(toggle, Qt::LeftButton);
    require(waitUntil([&] { return evidence->isVisible(); }, 2000),
            QString("Original evidence fold did not open: checked=%1; "
                    "toggle=%2,%3 %4x%5; evidence=%6,%7 %8x%9")
                .arg(toggle->isChecked())
                .arg(toggle->x())
                .arg(toggle->y())
                .arg(toggle->width())
                .arg(toggle->height())
                .arg(evidence->x())
                .arg(evidence->y())
                .arg(evidence->width())
                .arg(evidence->height()));
  }
  widebandScrollTo(evidence);
  require(waitUntil([&] {
            for (auto* owner = evidence->parentWidget(); owner;
                 owner = owner->parentWidget()) {
              auto* area = qobject_cast<QScrollArea*>(owner);
              if (!area) continue;
              const auto center =
                  evidence->mapTo(area->widget(), evidence->rect().center());
              // Scroll the editor rectangle, independently of its text cursor.
              area->ensureVisible(center.x(), center.y(), 0,
                                  evidence->height() / 2 + 8);
              break;
            }
            const auto visible = widebandVisibleRect(evidence, window);
            return visible.height() >= 95 && visible.width() >= 150;
          }),
          QString("Expanded original evidence lacks a readable viewport: "
                  "editor=%1x%2 visible=%3x%4")
              .arg(evidence->width())
              .arg(evidence->height())
              .arg(widebandVisibleRect(evidence, window).width())
              .arg(widebandVisibleRect(evidence, window).height()));
  require(
      evidence->isVisible() && !widebandVisibleRect(evidence, window).isEmpty(),
      "Immutable original fields are hidden; hidden text is not visual proof");
  return evidence;
}

void widebandSelectAnnotation(
    MainWindow& window, const std::string& id,
    const QString& expectedFormat = "torchsig-rfbench-sigmf-v1") {
  click(window, "resultTab");
  auto* table = child<QTableView>(window, "annotationTable");
  QModelIndex selected;
  for (int row = 0; row < table->model()->rowCount(); ++row) {
    const auto index = table->model()->index(row, 1);
    if (index.data(Qt::UserRole).toString().toStdString() == id)
      selected = index;
  }
  require(selected.isValid(),
          "The actual annotation is absent from the native table");
  table->scrollTo(selected);
  QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                    table->visualRect(selected).center());
  require(
      waitUntil(
          [&] { return window.session().activeFile()->activeMarkId == id; }),
      "Native annotation table selection did not reach the application state");
  widebandShowInspector(window, "contextAnnotation");
  auto* evidence = widebandOpenOriginalFields(window);
  require(evidence->toPlainText().contains(expectedFormat),
          "Selected annotation details do not show their actual source format");
}

QJsonObject widebandImportActualSource(MainWindow& window, const QString& data,
                                       const QString& meta, QScreen* screen) {
  const auto suggested = suggestSourceMetadata(meta);
  const auto declared = readSigmfMetadata(meta);
  require(suggested.error.isEmpty() && declared.error.isEmpty(),
          suggested.error + declared.error);
  require(suggested.format && suggested.sampleRateHz &&
              suggested.centerFrequencyHz && declared.format &&
              declared.sampleRateHz && declared.centerFrequencyHz,
          "Actual SigMF metadata must supply datatype, Fs and capture Fc");
  require(*suggested.format == *declared.format &&
              *suggested.sampleRateHz == *declared.sampleRateHz &&
              *suggested.centerFrequencyHz == *declared.centerFrequencyHz,
          "Metadata suggestion differs from the actual SigMF declaration");
  FileMetadata described;
  QString error;
  const bool valid = describeRawFile(
      data, *declared.format, *declared.sampleRateHz,
      *declared.centerFrequencyHz, *declared.sampleRateHz, described, error);
  require(valid, error);
  require(described.sampleRateHz == 20000000 &&
              described.centerFrequencyHz == 2500000000 &&
              described.sampleCount == 400000 &&
              formatId(described.sampleFormat) == "CF32",
          "Capture input differs from the frozen actual TorchSig 20M/400000 "
          "source");
  SignalImportDialog dialog("宽带检测原生验收", &window);
  dialog.commitSource = [&](FileState file, QString& message) {
    return window.addImportedSource(std::move(file), message);
  };
  dialog.setScreen(screen);
  dialog.show();
  dialog.move(screen->geometry().center() - dialog.rect().center());
  require(dialog.addPath(meta),
          "The actual SigMF source could not enter the import workbench");
  dialog.startImport();
  auto* finish = child<QPushButton>(dialog, "importProgressFinish");
  require(waitUntil([&] { return finish->isEnabled(); }, 60'000),
          "The actual SigMF complete-frame import did not finish");
  const auto& row = dialog.controller().rows().front();
  require(row.status == ImportStatus::Ready &&
              row.load.loaded == described.sampleCount &&
              row.metadata.sampleFormat == described.sampleFormat &&
              row.metadata.sampleRateHz == described.sampleRateHz &&
              row.metadata.centerFrequencyHz == described.centerFrequencyHz,
          "Imported actual frames or RF metadata differ from the independent "
          "description");
  const auto importedFrames = row.load.loaded;
  const auto envelopePoints = row.load.envelope.size();
  QTest::mouseClick(finish, Qt::LeftButton);
  require(window.session().activeFile() &&
              !window.session().activeFile()->metadata.demo,
          "The import did not publish a real source into the native project");
  return {{"sampleRateHz", described.sampleRateHz},
          {"centerFrequencyHz", described.centerFrequencyHz},
          {"physicalSamples", QString::number(described.sampleCount)},
          {"decodedCompleteFrames", QString::number(importedFrames)},
          {"navigationEnvelopePoints", int(envelopePoints)},
          {"format", QString::fromStdString(formatId(described.sampleFormat))},
          {"formatKey",
           QString::fromStdString(sampleFormatKey(described.sampleFormat))},
          {"metadataProvenance", declared.provenance}};
}

QJsonArray widebandCheckConfigurationEntrances(Capture& capture) {
  auto& window = capture.window;
  auto* card = child<QWidget>(window, "detectionCard");
  const auto fileId = window.session().activeFile()->metadata.id;
  auto* tab = child<QPushButton>(window, "contextDetection");
  QJsonArray entries;
  for (const auto* entry : {"widebandDetectionAction", "widebandDetect",
                            "contextWidebandDetectionAction"}) {
    widebandShowInspector(window, "contextDisplay");
    require(
        !card->isVisible(),
        "Detection page did not hide before independent entry check");
    if (QString::fromLatin1(entry) == "widebandDetectionAction") {
      widebandMenuAction(window, entry);
    } else if (QString::fromLatin1(entry) == "widebandDetect") {
      widebandNativeClick(child<QPushButton>(window, entry));
    } else {
      widebandChartMenuAction(capture.plots[2], entry);
    }
    require(waitUntil([&] { return card->isVisible() && tab->isChecked(); }),
            "Detection entry did not show the resident card page");
    require(!QApplication::activeModalWidget() &&
                child<QWidget>(window, "detectionCard") == card &&
                window.findChildren<QWidget*>("detectionCard").size() == 1 &&
                window.session().activeFile()->metadata.id == fileId &&
                window.session().activeFile()->detectionRuns.empty(),
            "Detection entry opened a modal, changed source, duplicated card, "
            "or started a task");
    widebandScrollTo(child<QComboBox>(*card, "detectionModel"));
    require(!widebandVisibleRect(card, window).isEmpty(),
            "Expanded DetectionCard is outside the native viewport");
    entries.append(QJsonObject{{"entry", entry},
                               {"sameResidentCard", true},
                               {"activeModal", false},
                               {"pass", true},
                               {"fileId", QString::fromStdString(fileId)}});
  }
  return entries;
}

QJsonObject widebandConfigureAndStart(Capture& capture, QScreen* screen) {
  auto& window = capture.window;
  auto* card = child<QWidget>(window, "detectionCard");
  require(card->isVisible() && !QApplication::activeModalWidget(),
          "Configuration must use the visible resident DetectionCard");
  auto* model = child<QComboBox>(*card, "detectionModel");
  const int fixture = model->findData("pipeline-fixture-v1");
  require(fixture >= 0,
          "Fixture must be explicitly enabled before constructing MainWindow");
  widebandSelectModelSpectrum(window, "pipeline-fixture-v1");
  auto* origin = child<QComboBox>(*card, "detectionRangeOrigin");
  require(origin->itemData(1).toString() == "available-file",
          "Native range index 1 must mean the available file");
  widebandNativeSelect(origin, 1);
  widebandScrollTo(model);
  auto* range = child<QLabel>(*card, "detectionRequestedRange");
  auto* times = child<QLabel>(*card, "detectionPhysicalTimes");
  require(range->isVisible() && !range->text().isEmpty() &&
              times->isVisible() && !times->text().isEmpty(),
          "Resident configuration lacks readable frozen physical facts");
  auto* calculations = qobject_cast<QLabel*>(
      widebandDetectionControl(window, "detectionCalculationDetailsText"));
  require(calculations && calculations->text().contains("FFT 窗") &&
              calculations->text().contains("谱步长") &&
              calculations->text().contains("模型上下文（请求）") &&
              calculations->text().contains("窗口滑动") &&
              calculations->text().contains("实际上下文样本") &&
              calculations->text().contains("边缘补零"),
          "Expanded calculation details lack the full physical timing basis");
  const auto timingDetails = calculations->text();
  widebandDetectionFold(window, "detectionCalculationDetailsToggle", false);
  widebandResidentCardAtTop(window);
  auto* run = child<QPushButton>(window, "runWidebandDetection");
  require(run->isVisible() && run->isEnabled() &&
              !widebandVisibleRect(run, window).isEmpty(),
          "The fixed real-file run button is unavailable");
  widebandScene(capture, "02-config-main-window", screen);
  widebandCaptureVisibleCard(capture, screen, card);
  widebandNativeClick(run);
  require(waitUntil(
              [&] {
                return window.session().activeFile()->detectionRuns.size() == 1;
              },
              5000),
          "The fixed card run button did not submit a native task");
  require(!QApplication::activeModalWidget(),
          "Running from the resident card unexpectedly opened a modal");
  const auto request =
      window.session().activeFile()->detectionRuns.front().request;
  require(request.profile.id == "pipeline-fixture-v1" &&
              request.rangeOrigin == "available-file" &&
              request.requestedRange.time.begin == 0 &&
              request.requestedRange.time.end ==
                  request.sourceIdentity.availableSamples,
          "The actual submitted task did not freeze the available-file range");
  auto configuration = encodeDetectionRequest(request);
  configuration["evidenceKind"] =
      "Native resident-card submission; pipeline fixture is not a model";
  configuration["visiblePhysicalRangeText"] = range->text();
  configuration["visibleTimingText"] = times->text();
  configuration["expandedCalculationDetailsText"] = timingDetails;
  return configuration;
}

DetectionRun widebandWaitForRun(MainWindow& window) {
  require(
      waitUntil(
          [&] {
            const auto* file = window.session().activeFile();
            return file && file->detectionRuns.size() == 1 &&
                   detectionIsTerminal(file->detectionRuns.front().lifecycle);
          },
          60'000),
      "The actual filesystem task did not reach a terminal state");
  const auto run = window.session().activeFile()->detectionRuns.front();
  require(
      run.lifecycle == DetectionLifecycle::Completed,
      "Actual filesystem task failed: " + QString::fromStdString(run.error));
  require(
      run.request.profile.id == "pipeline-fixture-v1" &&
          run.request.profile.backend == DetectionBackendKind::Fixture &&
          run.eligibleWindows == 3 && run.completedWindows == 3,
      "The actual 20-slot source did not complete its three real 10/5 windows");
  require(
      waitUntil([&] {
        return widebandSourceCounts(*window.session().activeFile()) ==
               QJsonObject{{"importedLabels", 4},
                           {"manual", 1},
                           {"algorithm", 3},
                           {"total", 8}};
      }),
      "Completed task did not publish its three actual fixture observations");
  return run;
}

std::uint64_t widebandRawCandidateCount(const QString& projectPath,
                                        const DetectionRun& run) {
  QJsonArray chunks;
  QString error;
  const bool loaded = readDetectionChunks(QFileInfo(projectPath).absolutePath(),
                                          run, chunks, error);
  require(loaded, error);
  std::uint64_t count = 0;
  for (const auto chunk : chunks)
    for (const auto value : chunk.toObject()["results"].toArray()) {
      require(value.isObject() && value.toObject()["candidates"].isArray(),
              "The persisted worker result lacks its actual candidate array");
      count += static_cast<std::uint64_t>(
          value.toObject()["candidates"].toArray().size());
    }
  return count;
}

void widebandExpandTaskArea(MainWindow& window) {
  auto* handle = child<QWidget>(window, "bottomPanelResize");
  auto* bottom = child<QWidget>(window, "bottom");
  const int before = bottom->height();
  require(handle->isVisible(), "Native task-area resize handle is hidden");
  const auto start = handle->rect().center();
  QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
  for (int step = 1; step <= 8; ++step)
    QTest::mouseMove(handle, start - QPoint(0, step * 30), 10);
  QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier,
                      start - QPoint(0, 240));
  require(waitUntil([&] {
            return bottom->height() > before &&
                   QWidget::mouseGrabber() != handle;
          }),
          "Actual task-area drag did not expand the panel and release capture");
}

void widebandCaptureTaskDetails(Capture& capture, QScreen* screen) {
  auto& window = capture.window;
  auto* list = child<QAbstractItemView>(window, "detectionRunList");
  require(list->inherits("QListView") && list->model()->rowCount() == 1,
          "The completed run is absent from the native task-card list");
  const auto index = list->model()->index(0, 0);
  const auto text = index.data(Qt::DisplayRole).toString();
  const auto& run = window.session().activeFile()->detectionRuns.front();
  require(
      text.contains("完成") && text.contains("窗口 3 / 3") &&
          text.contains(QString("已读谱帧 %1").arg(run.readFrames)) &&
          text.contains(QString("结果 %1").arg(run.resultCount)) &&
          text.contains("阶段："),
      "Actual compact task row lacks lifecycle, phase, read/window/result counts");
  list->scrollTo(index);
  const auto visible =
      list->visualRect(index).intersected(list->viewport()->rect());
  require(!visible.isEmpty(), "The actual task card is outside its viewport");
  QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                    visible.center());
  auto* details = child<QPushButton>(window, "detectionRunDetails");
  require(details && details->isEnabled(),
          "Native task coverage details button is unavailable");
  bool handled = false;
  QString error;
  QTimer::singleShot(100, &window, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    try {
      require(dialog, "Actual task snapshot/coverage details did not open");
      dialog->setScreen(screen);
      dialog->resize(1100, 1050);
      dialog->move(screen->geometry().center() - dialog->rect().center());
      auto* facts = child<QWidget>(*dialog, "detectionRunFacts");
      const auto* label = qobject_cast<QLabel*>(facts);
      const auto readable =
          label ? label->text() : facts->property("plainText").toString();
      require(
          facts->isVisible() &&
              !widebandVisibleRect(facts, *dialog).isEmpty() &&
              readable.contains("冻结请求") && readable.contains("RF") &&
              readable.contains("completed windows 3") &&
              readable.contains("eligible 3") &&
              readable.contains("冻结上下文") &&
              readable.contains("已覆盖") && readable.contains("已读：") &&
              readable.contains("未读：") && readable.contains("排除：") &&
              readable.contains("未读或排除区域没有检测结论"),
          "Visible readable task facts lack actual frozen ranges or coverage");
      require(!child<QPlainTextEdit>(*dialog, "detectionRunTechnicalJson")
                   ->isVisible(),
              "Task details must start with readable facts, not raw JSON");
      widebandScene(capture, "04-task-readable-details", screen, dialog);
      auto record = capture.scenes.last().toObject();
      record["readableFactsWidgetClass"] = facts->metaObject()->className();
      record["visibleReadableFacts"] = readable;
      record["technicalJsonInitiallyCollapsed"] = true;
      capture.scenes[capture.scenes.size() - 1] = record;
      auto* buttons = dialog->findChild<QDialogButtonBox*>();
      require(buttons && buttons->button(QDialogButtonBox::Close),
              "Task details have no visible Close button");
      widebandNativeClick(buttons->button(QDialogButtonBox::Close));
    } catch (const std::exception& failure) {
      error = QString::fromUtf8(failure.what());
      if (dialog) dialog->reject();
    }
    handled = true;
  });
  widebandNativeClick(details);
  require(waitUntil([&] { return handled; }, 5000),
          "Coverage dialog callback did not complete");
  require(error.isEmpty(), error);
}

QJsonObject widebandReviseImported(Capture& capture, QScreen* screen,
                                   const std::string& id) {
  auto& window = capture.window;
  widebandSelectAnnotation(window, id);
  widebandChooseQuality(window, "acceptAnnotations");
  auto* accepted = findAnnotation(*window.session().activeFile(), id);
  require(accepted && accepted->reviewStatus == ReviewStatus::Accepted,
          "Native review did not accept the selected original label");
  const auto previousRevision = accepted->revisionId;
  const auto originalEvidence = accepted->originEvidence->originalFieldsJson;
  const auto* segment = activeSegment(*accepted);
  require(segment && segment->range.time.end > segment->range.time.begin + 1,
          "Actual label has no editable nonempty active interval");
  const auto newEnd = segment->range.time.end - 1;
  auto* end = child<QLineEdit>(window, "annotationEndMilliseconds");
  widebandScrollTo(end);
  require(end->isEnabled(),
          "Native imported-label geometry is not directly editable");
  end->setFocus();
  end->selectAll();
  const double sampleRate =
      window.session().activeFile()->metadata.sampleRateHz;
  const auto endMilliseconds =
      QString::number(static_cast<double>(newEnd) / sampleRate * 1000, 'g', 17);
  QTest::keyClicks(end, endMilliseconds);
  QTest::keyClick(end, Qt::Key_Return);
  auto* revised = findAnnotation(*window.session().activeFile(), id);
  require(revised && revised->revisionId != previousRevision &&
              revised->reviewStatus == ReviewStatus::Pending &&
              revised->reviewRevisionId == revised->revisionId &&
              revised->sourceKind == AnnotationSource::ImportedLabel &&
              revised->originEvidence->originalFieldsJson == originalEvidence &&
              activeSegment(*revised) &&
              activeSegment(*revised)->range.time.end == newEnd,
          "Geometry revision did not become pending while retaining immutable "
          "original evidence");
  widebandOpenOriginalFields(window);
  widebandScene(capture, "05-three-layers-revision-pending", screen);
  return {{"annotationId", QString::fromStdString(id)},
          {"previousRevisionId", QString::fromStdString(previousRevision)},
          {"currentRevisionId", QString::fromStdString(revised->revisionId)},
          {"reviewStatus", "Pending"},
          {"committedEndMilliseconds", endMilliseconds},
          {"exactExpectedEndSample", QString::number(newEnd)},
          {"reviewBindsCurrentRevision", true},
          {"originalEvidenceUnchanged", true},
          {"revisionCount", int(revised->revisions.size())},
          {"reviewDecisionCount", int(revised->reviewHistory.size())}};
}

QJsonObject widebandCaptureRoi(Capture& capture, QScreen* screen) {
  auto& window = capture.window;
  auto* plot = capture.plots[2];
  const auto* file = window.session().activeFile();
  const auto view = file->view;
  QJsonArray annotations;
  for (const auto& annotation : file->marks)
    annotations.append(encodeAnnotation(annotation));
  const auto runBefore = encodeDetectionRun(file->detectionRuns.front());
  const auto verifyNoMutation = [&] {
    const auto* current = window.session().activeFile();
    QJsonArray currentAnnotations;
    for (const auto& annotation : current->marks)
      currentAnnotations.append(encodeAnnotation(annotation));
    require(current->view == view && currentAnnotations == annotations &&
                current->detectionRuns.size() == 1 &&
                encodeDetectionRun(current->detectionRuns.front()) == runBefore,
            "ROI draft changed view, marks/revisions or the completed task");
  };
  startCreating(plot);
  widebandMenuAction(window, "widebandDetectionRoiAction");
  require(waitUntil([&] {
            return plot->isSelectingDetectionRoi() && !plot->isCreating();
          }),
          "ROI must take priority over continuous manual marking");
  createMark(plot, view, .22, .65, .20, .75);
  require(
      plot->hasDetectionRoiPreview() && plot->hasPendingInteraction() &&
          child<QWidget>(window, "detectionRoiConfirmation")->isVisible() &&
          QWidget::mouseGrabber() != plot,
      "Native ROI drag did not release capture and retain a visible preview");
  auto* roiConfirmation = child<QWidget>(window, "detectionRoiConfirmation");
  auto* roiRangeText = child<QLabel>(window, "detectionRoiRange");
  const auto preview = plot->detectionRoiPreview();
  require(preview &&
              roiRangeText->text().contains(QString::number(
                  preview->time.begin / file->metadata.sampleRateHz * 1000, 'f',
                  4)) &&
              roiRangeText->text().contains(
                  QString::number(preview->frequency.upperHz / 1e6, 'f', 6)) &&
              std::abs(roiConfirmation->geometry().center().x() -
                       plot->plotRect().center().x()) <= 1 &&
              plot->plotRect().contains(roiConfirmation->geometry()),
          "ROI confirmation must display its actual ms/RF range and remain "
          "centered inside the plot, as approved in M0-P");
  verifyNoMutation();
  widebandScene(capture, "07-roi-preview-before-esc", screen);
  QTest::keyClick(plot, Qt::Key_Escape);
  require(!plot->isSelectingDetectionRoi() && !plot->hasDetectionRoiPreview() &&
              !plot->hasPendingInteraction() && plot->isCreating() &&
              QWidget::mouseGrabber() != plot,
          "Esc did not discard ROI and restore the previous continuous tool");
  verifyNoMutation();
  widebandScene(capture, "08-roi-esc-manual-restored", screen);
  QTest::keyClick(plot, Qt::Key_Escape);
  require(!plot->isCreating(), "Second Esc did not leave manual marking");

  QObject receiver;
  bool confirmed = false;
  ViewRange confirmedRange;
  QObject::connect(plot, &PlotWidget::detectionRoiConfirmed, &receiver,
                   [&](const ViewRange& range) {
                     confirmedRange = range;
                     confirmed = true;
                   });
  widebandMenuAction(window, "widebandDetectionRoiAction");
  require(waitUntil([&] { return plot->isSelectingDetectionRoi(); }),
          "Second visible ROI menu entry did not activate the tool");
  createMark(plot, view, .22, .65, .20, .75);
  require(plot->hasDetectionRoiPreview(),
          "Second native ROI drag did not retain its confirmation draft");
  widebandScene(capture, "09-roi-preview-before-enter", screen);
  QTest::keyClick(plot, Qt::Key_Return);
  const auto bounds = fullRange(window.session().activeFile()->metadata);
  require(
      confirmed && !plot->isSelectingDetectionRoi() &&
          !plot->hasDetectionRoiPreview() && !plot->isCreating() &&
          confirmedRange.time.begin < confirmedRange.time.end &&
          confirmedRange.time.end <= bounds.time.end &&
          std::isfinite(confirmedRange.frequency.lowerHz) &&
          std::isfinite(confirmedRange.frequency.upperHz) &&
          confirmedRange.frequency.lowerHz < confirmedRange.frequency.upperHz &&
          confirmedRange.frequency.lowerHz >= bounds.frequency.lowerHz &&
          confirmedRange.frequency.upperHz <= bounds.frequency.upperHz &&
          child<QComboBox>(window, "detectionRangeOrigin")
                  ->currentData()
                  .toString() == "custom",
      "Enter did not commit a legal physical half-open ROI to the resident "
      "card");
  verifyNoMutation();
  widebandScrollTo(child<QComboBox>(window, "detectionRangeOrigin"));
  widebandScene(capture, "10-roi-enter-confirmed", screen);
  return {{"pass", true},
          {"escRestoredContinuousMarking", true},
          {"viewAnnotationsAndRunUnchanged", true},
          {"enterConfirmedResidentCard", true},
          {"startSample", QString::number(confirmedRange.time.begin)},
          {"endSample", QString::number(confirmedRange.time.end)},
          {"frequencyLowerHz", confirmedRange.frequency.lowerHz},
          {"frequencyUpperHz", confirmedRange.frequency.upperHz}};
}

QJsonObject widebandResizePanel(MainWindow& window, QWidget* handle,
                                QWidget* panel, Qt::Orientation orientation,
                                QPoint delta, bool cancel) {
  auto* native = window.windowHandle();
  require(native && handle->isVisible() && panel->isVisible() &&
              !widebandVisibleRect(handle, window).isEmpty(),
          "Native panel resizing requires a visible handle and native window");
  window.raise();
  window.activateWindow();
  require(QTest::qWaitForWindowActive(&window, 5000),
          "Native panel resizing requires the acceptance window to be active");
  const auto measure = [&] {
    return orientation == Qt::Horizontal ? panel->width() : panel->height();
  };
  const int before = measure();
  const int direction = handle->objectName() == "leftPanelResize" ? 1 : -1;
  const int movement = orientation == Qt::Horizontal ? delta.x() : delta.y();
  const int expected = before + direction * movement;
  const auto startGlobal = handle->mapToGlobal(handle->rect().center());
  const auto targetGlobal = startGlobal + delta;
  MouseMoveEvidence moves(handle);
  QTest::mousePress(native, Qt::LeftButton, Qt::NoModifier,
                    native->mapFromGlobal(startGlobal), 0);
  const bool captured = waitUntil(
      [&] { return QWidget::mouseGrabber() == handle && handle->hasFocus(); });
  require(
      captured,
      QString("Native panel press did not capture/focus %1; "
              "active=%2 grabber=%3 focus=%4 hit=%5")
          .arg(handle->objectName())
          .arg(window.isActiveWindow())
          .arg(QWidget::mouseGrabber() ? QWidget::mouseGrabber()->objectName()
                                       : QString("none"),
               QApplication::focusWidget()
                   ? QApplication::focusWidget()->objectName()
                   : QString("none"),
               window.childAt(window.mapFromGlobal(startGlobal))
                   ? window.childAt(window.mapFromGlobal(startGlobal))
                         ->objectName()
                   : QString("none")));
  for (int step = 1; step <= 8; ++step)
    QTest::mouseMove(native,
                     native->mapFromGlobal(startGlobal + delta * step / 8), 0);
  require(waitUntil([&] { return measure() == expected && moves.count() > 0; }),
          QString("Native panel did not follow cumulative logical delta: ") +
              handle->objectName() +
              QString("; before=%1 expected=%2 actual=%3")
                  .arg(before)
                  .arg(expected)
                  .arg(measure()));
  const int preview = measure();
  if (cancel) {
    QTest::keyClick(native, Qt::Key_Escape, Qt::NoModifier, 0);
    require(waitUntil([&] {
              return measure() == before && QWidget::mouseGrabber() != handle;
            }),
            "Esc did not restore the panel size and release mouse capture");
  }
  QTest::mouseRelease(native, Qt::LeftButton, Qt::NoModifier,
                      native->mapFromGlobal(targetGlobal), 0);
  require(waitUntil([&] {
            return QWidget::mouseGrabber() != handle &&
                   measure() == (cancel ? before : expected);
          }),
          "Panel release changed the committed or Esc-restored size");
  return {{"handle", handle->objectName()},
          {"panel", panel->objectName()},
          {"orientation",
           orientation == Qt::Horizontal ? "horizontal" : "vertical"},
          {"inputMethod", "QTest QWindow QPA mouse and key events"},
          {"coordinateUnits", "Qt global logical pixels"},
          {"nativeDevicePixelRatio", window.devicePixelRatioF()},
          {"startGlobal", pointJson(startGlobal)},
          {"targetGlobal", pointJson(targetGlobal)},
          {"logicalDelta", pointJson(delta)},
          {"before", before},
          {"preview", preview},
          {"after", measure()},
          {"escRestored", cancel},
          {"mouseCaptureReleased", true},
          {"actualMouseMoveEvents", moves.events()},
          {"pass", true}};
}

QJsonObject widebandCapturePanelLayouts(Capture& capture, QScreen* screen) {
  auto& window = capture.window;
  const auto* file = window.session().activeFile();
  const auto annotations = widebandAnnotations(*file);
  const auto sources = widebandSources(*file);
  const auto view = file->view;
  const auto run = encodeDetectionRun(file->detectionRuns.front());
  const auto verify = [&] {
    const auto* current = window.session().activeFile();
    require(current->view == view &&
                widebandAnnotations(*current) == annotations &&
                widebandSources(*current) == sources &&
                current->detectionRuns.size() == 1 &&
                encodeDetectionRun(current->detectionRuns.front()) == run &&
                window.isFullScreen() && window.screen() == screen &&
                window.size() == QSize(2560, 1440),
            "Panel resizing changed source geometry, revisions, evidence, "
            "task snapshot or required native screen");
    const auto mainRect = capture.plots[2]->plotRect();
    require(mainRect.width() >= 300 && mainRect.height() >= 160,
            "Resized native layout left the main chart unusable");
    widebandRequireVisibleActions(window);
  };
  struct ResizeCase {
    const char* handle;
    const char* panel;
    Qt::Orientation orientation;
    QPoint commitDelta;
    QPoint cancelDelta;
    const char* committedScene;
    const char* restoredScene;
  };
  const std::array<ResizeCase, 3> cases{{{"leftPanelResize",
                                          "resources",
                                          Qt::Horizontal,
                                          {40, 0},
                                          {24, 0},
                                          "11-layout-left-committed",
                                          "12-layout-left-esc-restored"},
                                         {"rightPanelResize",
                                          "properties",
                                          Qt::Horizontal,
                                          {-40, 0},
                                          {-24, 0},
                                          "13-layout-right-committed",
                                          "14-layout-right-esc-restored"},
                                         {"bottomPanelResize",
                                          "bottom",
                                          Qt::Vertical,
                                          {0, 80},
                                          {0, 40},
                                          "15-layout-bottom-committed",
                                          "16-layout-bottom-esc-restored"}}};
  QJsonArray operations;
  for (const auto& item : cases) {
    auto* handle = child<QWidget>(window, item.handle);
    auto* panel = child<QWidget>(window, item.panel);
    auto committed = widebandResizePanel(
        window, handle, panel, item.orientation, item.commitDelta, false);
    require(widebandDisplayReady(window),
            "Actual IQ hardware display did not settle after panel resize");
    widebandResidentCardAtTop(window);
    verify();
    widebandScene(capture, item.committedScene, screen);
    committed["scene"] = item.committedScene;
    auto restored = widebandResizePanel(window, handle, panel, item.orientation,
                                        item.cancelDelta, true);
    require(widebandDisplayReady(window),
            "Actual IQ hardware display did not settle after Esc rollback");
    widebandResidentCardAtTop(window);
    verify();
    widebandScene(capture, item.restoredScene, screen);
    restored["scene"] = item.restoredScene;
    operations.append(QJsonObject{{"commit", committed}, {"cancel", restored}});
  }
  return {{"pass", true},
          {"operations", operations},
          {"geometryRevisionsSourcesAndRunUnchanged", true},
          {"mainChartRemainedUsable", true},
          {"fixedRunAndCancelActionsFullyVisible", true}};
}

QJsonObject widebandCaptureAuxiliaryEvidence(Capture& capture, QScreen* screen,
                                             const std::string& id) {
  auto& window = capture.window;
  widebandSelectAnnotation(window, id);
  const auto* file = window.session().activeFile();
  const auto annotations = widebandAnnotations(*file);
  const auto sources = widebandSources(*file);
  const auto view = file->view;
  const auto run = encodeDetectionRun(file->detectionRuns.front());
  const auto* annotation = findAnnotation(*file, id);
  const auto* source = findSourceSet(*file, annotation->sourceSetId);
  require(source && !source->sourceHash.empty(),
          "Auxiliary original evidence lacks its saved source hash");
  const auto sourceHash = QString::fromStdString(source->sourceHash);
  const auto verify = [&] {
    const auto* current = window.session().activeFile();
    require(current->view == view &&
                widebandAnnotations(*current) == annotations &&
                widebandSources(*current) == sources &&
                current->detectionRuns.size() == 1 &&
                encodeDetectionRun(current->detectionRuns.front()) == run,
            "Auxiliary evidence browsing changed geometry, revisions, source "
            "snapshots or the completed task");
    widebandRequireVisibleActions(window);
  };
  auto* mode = child<QPushButton>(window, "detectionEvidenceMode");
  require(mode->isVisible() && !widebandVisibleRect(mode, window).isEmpty(),
          "Auxiliary detection-evidence mode has no visible native button");
  QTest::mouseClick(mode, Qt::LeftButton);
  auto* auxiliary = child<QWidget>(window, "auxiliaryDetectionEvidence");
  require(waitUntil([&] {
            return mode->isChecked() && auxiliary->isVisible() &&
                   !capture.plots[1]->isVisible();
          }),
          "Native auxiliary mode did not replace the plot with evidence");
  auto* title = child<QLabel>(*auxiliary, "annotationEvidenceTitle");
  widebandScrollTo(title);
  require(title->isVisible() && !title->text().isEmpty() &&
              widebandVisibleRect(title, window).height() > 10,
          "Auxiliary evidence title is not actually inside its viewport");
  const auto timeSummary =
      child<QWidget>(*auxiliary, "annotationAuxiliaryTimeSummary");
  const auto scoreSummary =
      child<QWidget>(*auxiliary, "annotationAuxiliaryScoreSummary");
  const auto diagnosticSummary =
      child<QWidget>(*auxiliary, "annotationAuxiliaryDiagnosticSummary");
  require(timeSummary->x() < scoreSummary->x() &&
              scoreSummary->x() < diagnosticSummary->x() &&
              widebandVisibleRect(timeSummary, window).height() >= 40 &&
              widebandVisibleRect(scoreSummary, window).height() >= 40 &&
              widebandVisibleRect(diagnosticSummary, window).height() >= 40,
          "The approved auxiliary time/score/noise summaries must be visible "
          "as three distinct native columns");
  verify();
  widebandScene(capture, "17-auxiliary-evidence-facts", screen);
  auto* toggle =
      child<QToolButton>(*auxiliary, "auxiliaryOriginalFieldsToggle");
  auto* raw = child<QPlainTextEdit>(*auxiliary, "auxiliaryAnnotationEvidence");
  if (toggle->isChecked()) {
    widebandScrollTo(toggle);
    QTest::mouseClick(toggle, Qt::LeftButton);
  }
  require(waitUntil([&] { return !toggle->isChecked() && !raw->isVisible(); }),
          "Auxiliary original evidence did not collapse through its control");
  widebandScrollTo(toggle);
  require(toggle->isVisible() && toggle->isEnabled() &&
              widebandVisibleRect(toggle, window).height() == toggle->height(),
          "Collapsed auxiliary evidence toggle is outside the actual viewport");
  verify();
  widebandScene(capture, "18-auxiliary-original-fields-collapsed", screen);
  QTest::mouseClick(toggle, Qt::LeftButton);
  require(waitUntil([&] { return toggle->isChecked() && raw->isVisible(); }),
          "Actual auxiliary original-field toggle did not open the editor");
  // ensureWidgetVisible() follows a text editor's input-method cursor rect.
  // Expose the editor viewport itself instead of only its first text line.
  auto* rawScroll = raw->parentWidget();
  while (rawScroll && !qobject_cast<QScrollArea*>(rawScroll))
    rawScroll = rawScroll->parentWidget();
  auto* rawArea = qobject_cast<QScrollArea*>(rawScroll);
  require(rawArea, "Auxiliary evidence has no native scroll viewport");
  const bool rawExposed = waitUntil([&] {
    const auto center = raw->mapTo(rawArea->widget(), raw->rect().center());
    const auto margin =
        std::min(raw->height() / 2, rawArea->viewport()->height() / 2 - 12);
    rawArea->ensureVisible(center.x(), center.y(), 12, std::max(0, margin));
    const auto visible = widebandVisibleRect(raw, window);
    return visible.width() >= 150 && visible.height() >= 60;
  });
  require(rawExposed,
          QString("Expanded auxiliary evidence viewport did not settle: "
                  "raw=%1x%2; visible=%3x%4")
              .arg(raw->width())
              .arg(raw->height())
              .arg(widebandVisibleRect(raw, window).width())
              .arg(widebandVisibleRect(raw, window).height()));
  const auto rawRect = widebandVisibleRect(raw, window);
  require(rawRect.width() >= 150 && rawRect.height() >= 60 &&
              raw->toPlainText().contains("torchsig-rfbench-sigmf-v1") &&
              raw->toPlainText().contains("formatId:") &&
              raw->toPlainText().contains("originRef:") &&
              raw->toPlainText().contains(sourceHash),
          "Expanded auxiliary original evidence is hidden or lost format, "
          "origin reference or saved source hash");
  verify();
  widebandScene(capture, "19-auxiliary-original-fields-visible", screen);
  QTest::mouseClick(child<QPushButton>(window, "psdMode"), Qt::LeftButton);
  require(waitUntil([&] {
            return !mode->isChecked() && !auxiliary->isVisible() &&
                   capture.plots[1]->isVisible();
          }) &&
              widebandDisplayReady(window),
          "Native PSD mode did not restore the actual IQ auxiliary display");
  verify();
  return {{"pass", true},
          {"annotationId", QString::fromStdString(id)},
          {"modeButtonVisible", true},
          {"originalFieldsFoldOperatedVisibly", true},
          {"visibleRawRect", rectJson(rawRect)},
          {"sourceHash", sourceHash},
          {"geometryRevisionsSourcesAndRunUnchanged", true},
          {"actualPsdRestored", true},
          {"modelInferenceVerified", false}};
}

QJsonObject widebandCapturePendingSegmentChannel(
    Capture& capture, QScreen* screen, const std::string& id,
    const QString& projectPath, const QString& dataPath,
    const QString& expectedIqHash) {
  auto& window = capture.window;
  widebandSelectAnnotation(window, id);
  const auto* file = window.session().activeFile();
  const auto* annotation = findAnnotation(*file, id);
  require(
      annotation && annotation->sourceKind == AnnotationSource::ImportedLabel &&
          annotation->reviewStatus == ReviewStatus::Pending &&
          activeSegment(*annotation) && !activeSegment(*annotation)->confirmed,
      "The source must be an actual pending, unconfirmed original label");
  const Annotation before = *annotation;
  const auto segmentId = activeSegment(before)->id;
  const auto annotationsBefore = widebandAnnotations(*file);
  const auto sourcesBefore = widebandSources(*file);
  const auto viewBefore = file->view;
  const auto runBefore = encodeDetectionRun(file->detectionRuns.front());
  const auto channelsBefore = file->channels.size();
  const auto* source = findSourceSet(*file, annotation->sourceSetId);
  require(source && !source->sourceHash.empty(),
          "The pending source lacks its immutable identity");
  const auto sourceHash = QString::fromStdString(source->sourceHash);
  const auto verify = [&] {
    const auto* current = window.session().activeFile();
    const auto* selected = findAnnotation(*current, id);
    require(
        selected && selected->reviewStatus == ReviewStatus::Pending &&
            selected->revisionId == before.revisionId &&
            findSegment(*selected, segmentId) &&
            !findSegment(*selected, segmentId)->confirmed &&
            widebandAnnotations(*current) == annotationsBefore &&
            widebandSources(*current) == sourcesBefore &&
            current->view == viewBefore &&
            current->channels.size() == channelsBefore &&
            current->detectionRuns.size() == 1 &&
            encodeDetectionRun(current->detectionRuns.front()) == runBefore &&
            widebandSourceCounts(*current) == QJsonObject{{"importedLabels", 4},
                                                          {"manual", 1},
                                                          {"algorithm", 3},
                                                          {"total", 8}},
        "Channel preview changed original objects, quality, sources or task");
  };
  verify();
  widebandRequireVisibleActions(window);
  require(!window.findChild<QPushButton*>("confirmAnnotationSegment"),
          "The obsolete activity confirmation prerequisite remains visible");
  widebandScene(capture, "20-original-pending-segment-direct-channel", screen);
  auto* extract = child<QPushButton>(window, "extract");
  require(extract->isVisible() && extract->isEnabled() &&
              !widebandVisibleRect(extract, window).isEmpty(),
          "Pending unconfirmed activity cannot open channel configuration");
  QTest::mouseClick(extract, Qt::LeftButton);
  QPointer<QDialog> dialog = window.findChild<QDialog*>("channelConfigDialog");
  require(dialog && waitUntil([&] {
            return dialog && dialog->isVisible() && dialog->windowHandle();
          }),
          "The pending activity did not open native channel configuration");
  dialog->windowHandle()->setScreen(screen);
  dialog->move(screen->geometry().center() - dialog->rect().center());
  require(waitUntil([&] {
            return dialog->screen() == screen &&
                   std::abs(dialog->devicePixelRatioF() - 1.5) < .001 &&
                   screen->geometry().contains(dialog->frameGeometry());
          }),
          "Channel configuration left selected 4K monitor or actual 150% DPI");
  require(
      !dialog->findChild<QCheckBox*>("channelConfirmedSegment") &&
          child<QPushButton>(*dialog, "confirmChannelConfig")->isEnabled() &&
          child<QLabel>(*dialog, "channelValidation")->text().isEmpty(),
      "Valid pending activity is still gated by confirmation or quality");
  verify();
  widebandScene(capture, "21-pending-unconfirmed-channel-configuration", screen,
                dialog.data());
  auto* buttons = dialog->findChild<QDialogButtonBox*>();
  require(buttons && buttons->button(QDialogButtonBox::Cancel) &&
              buttons->button(QDialogButtonBox::Cancel)->isVisible(),
          "Native channel preview has no visible cancel control");
  QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel), Qt::LeftButton);
  require(waitUntil([&] { return !dialog || !dialog->isVisible(); }),
          "Cancelling channel configuration did not close it");
  verify();
  require(window.saveProject(projectPath) && window.openProject(projectPath) &&
              widebandDisplayReady(window),
          "The unchanged pending source did not save and reopen on actual IQ");
  verify();
  require(widebandFileHash(dataPath) == expectedIqHash,
          "Channel configuration changed the original IQ data");
  widebandSelectAnnotation(window, id);
  widebandRequireVisibleActions(window);
  widebandScene(capture, "22-pending-source-unchanged-after-reopen", screen);
  return {{"pass", true},
          {"annotationId", QString::fromStdString(id)},
          {"segmentId", QString::fromStdString(segmentId)},
          {"reviewStatus", "Pending"},
          {"geometryRevisionId", QString::fromStdString(before.revisionId)},
          {"sourceHash", sourceHash},
          {"allEightObjectsUnchanged", true},
          {"sourceTextHashGeometryRevisionAndRunPreserved", true},
          {"noQualityOrConfirmationGate", true},
          {"channelDialogCancelledWithoutCreation", true},
          {"pendingUnconfirmedPreservedAfterSaveReopen", true},
          {"iqSha256Unchanged", true},
          {"ddcExecutionVerified", false},
          {"modelInferenceVerified", false}};
}

QJsonObject widebandCaptureProductionModel(Capture& capture, QScreen* screen,
                                           const QString& projectPath) {
  auto& window = capture.window;
  auto* card = child<QWidget>(window, "detectionCard");
  auto* origin = child<QComboBox>(*card, "detectionRangeOrigin");
  widebandMenuAction(window, "widebandDetectionAction");
  widebandSelectModelSpectrum(window, "ffscn-gui-iq-v1");
  widebandNativeSelect(origin, origin->findData("available-file"));
  window.refresh();
  auto* button = child<QPushButton>(window, "runWidebandDetection");
  require(button->isVisible() && button->isEnabled(),
          "Production asset preflight did not enable the approved resident run "
          "action");
  const auto previous = window.session().activeFile()->detectionRuns.size();
  auto* cancelFeedback = child<QPushButton>(window, "cancelDetectionCardRun");
  WidebandFeedbackPaint firstPaint(*cancelFeedback, [&] {
    const auto* file = window.session().activeFile();
    return file->detectionRuns.size() == previous + 1 &&
           cancelFeedback->isEnabled() && !button->isEnabled() &&
           cancelFeedback->toolTip().contains(QString::fromStdString(
               file->detectionRuns.back().request.runId));
  });
  QElapsedTimer startFeedback;
  startFeedback.start();
  widebandNativeClick(button);
  require(waitUntil(
              [&] {
                return window.session().activeFile()->detectionRuns.size() ==
                       previous + 1;
              },
              5000),
          "The resident action did not freeze the production model request");
  require(waitUntil([&] { return firstPaint.count() > 0; }),
          "The visible Run/Cancel state did not paint the submitted task");
  const auto startFeedbackMs = startFeedback.nsecsElapsed() / 1000000.0;
  widebandScene(capture, "23-production-model-running", screen);
  require(
      waitUntil(
          [&] {
            return detectionIsTerminal(
                window.session().activeFile()->detectionRuns.back().lifecycle);
          },
          120000),
      "Production model task did not finish");
  const auto run = window.session().activeFile()->detectionRuns.back();
  require(
      run.lifecycle == DetectionLifecycle::Completed &&
          run.completedWindows > 0 &&
          run.completedWindows == run.eligibleWindows &&
          !run.request.modelSha256.empty() &&
          run.device.find("RTX 5060 Laptop") != std::string::npos,
      "Actual production model failed: " + QString::fromStdString(run.error));
  std::vector<std::string> ids;
  require(
      waitUntil(
          [&] {
            ids.clear();
            for (const auto& annotation : window.session().activeFile()->marks)
              if (annotation.originEvidence &&
                  annotation.originEvidence->runId == run.request.runId)
                ids.push_back(annotation.id);
            return !ids.empty();
          },
          5000),
      "Production model results were not published into the shared annotation "
      "table");
  for (const auto& id : ids) {
    const auto* annotation = findAnnotation(*window.session().activeFile(), id);
    require(
        annotation && annotation->sourceKind == AnnotationSource::Algorithm &&
            !annotation->classLabel && annotation->originEvidence &&
            annotation->observationTimeQuality ==
                TimeQuality::WindowObservation,
        "Production publication lost observation evidence or invented a class");
  }
  std::string selectedId;
  for (const auto& id : ids) {
    const auto* annotation = findAnnotation(*window.session().activeFile(), id);
    if (!annotation->segments.empty() &&
        annotation->originEvidence->refineMethod == RefineMethod::Sample &&
        annotation->originEvidence->refineStatus == RefineStatus::Applied) {
      selectedId = id;
      break;
    }
  }
  require(!selectedId.empty(),
          "Actual IQ produced no applied sample time event");
  widebandSelectAnnotation(window, selectedId, "detection-v1");
  const auto* selected =
      findAnnotation(*window.session().activeFile(), selectedId);
  const auto originalFields = selected->originEvidence->originalFieldsJson;
  const auto segmentId = selected->activeSegmentId;
  require(
      selected->segments == selected->originEvidence->originalSegments &&
          activeSegment(*selected)->timeQuality ==
              TimeQuality::AlgorithmEstimated &&
          !activeSegment(*selected)->confirmed,
      "Time publication must preserve measured origin and pending activity");
  auto* measured = child<QWidget>(window, "auxiliaryDetectionEvidence")
                       ->findChild<QWidget*>("annotationMeasuredEvidenceTrace");
  auto* evidenceMode = child<QPushButton>(window, "detectionEvidenceMode");
  if (!evidenceMode->isChecked())
    QTest::mouseClick(evidenceMode, Qt::LeftButton);
  require(
      waitUntil([&] { return measured && measured->isVisible(); }),
      "Actual saved sample trace did not enter the auxiliary evidence mode");
  widebandScrollTo(measured);
  require(measured && measured->isVisible() &&
              widebandVisibleRect(measured, window).height() >= 40 &&
              measured->property("measuredPointCount").toULongLong() >= 2,
          "Saved sample evidence lacks its measured, bounded energy trace");
  widebandScene(capture, "24-production-model-time-refinement", screen);
  const auto countBeforeReview = window.session().activeFile()->marks.size();
  widebandChooseQuality(window, "acceptAnnotations");
  widebandChooseQuality(window, "clearAnnotationReview");
  selected = findAnnotation(*window.session().activeFile(), selectedId);
  require(
      selected->reviewStatus == ReviewStatus::Pending &&
          !activeSegment(*selected)->confirmed &&
          window.session().activeFile()->marks.size() == countBeforeReview &&
          selected->originEvidence->originalFieldsJson == originalFields,
      "Optional quality changed measured activity, origin or object count");
  widebandScene(capture, "25-real-algorithm-quality-is-optional", screen);
  auto* extract = child<QPushButton>(window, "extract");
  require(extract->isEnabled(),
          "Pending algorithm activity cannot create a channel");
  QTest::mouseClick(extract, Qt::LeftButton);
  auto* dialog = window.findChild<QDialog*>("channelConfigDialog");
  require(dialog && dialog->isVisible(),
          "Algorithm channel dialog did not open");
  dialog->windowHandle()->setScreen(screen);
  dialog->move(screen->geometry().center() - dialog->rect().center());
  require(!dialog->findChild<QCheckBox*>("channelConfirmedSegment") &&
              child<QPushButton>(*dialog, "confirmChannelConfig")->isEnabled(),
          "Actual channel parameters rejected pending unconfirmed activity");
  widebandScene(capture, "26-real-algorithm-channel-parameters", screen,
                dialog);
  const auto channelCount = window.session().activeFile()->channels.size();
  click(*dialog, "confirmChannelConfig");
  require(waitUntil([&] {
            return window.session().activeFile()->channels.size() ==
                   channelCount + 1;
          }),
          "Pending algorithm channel was not created");
  const auto channel = window.session().activeFile()->channels.back();
  require(
      channel.sourceSnapshot &&
          channel.sourceSnapshot->annotationId == selectedId &&
          channel.sourceSnapshot->segmentId == segmentId &&
          channel.sourceSnapshot->sourceKind == AnnotationSource::Algorithm &&
          channel.sourceRelation == ChannelSourceRelation::Current,
      "Channel lost actual algorithm/revision/source identity");
  ChannelDspPlan dsp;
  ChannelSampleData samples;
  QString error;
  const auto metadata = window.session().activeFile()->metadata;
  require(makeChannelDspPlan(metadata, channel, dsp, error), error);
  const auto outputCount = static_cast<SampleIndex>(
      (channel.sourceTime.end - channel.sourceTime.begin) *
      static_cast<long double>(channel.outputSampleRateHz) /
      metadata.sampleRateHz);
  require(outputCount > 0 &&
              processChannelSamples(
                  metadata, channel, dsp,
                  {0, std::min<SampleIndex>(16384, outputCount)}, samples),
          "CPU DDC did not produce actual samples from model activity");
  double energy = 0;
  for (const auto value : samples.samples) {
    require(std::isfinite(value.real()) && std::isfinite(value.imag()),
            "CPU DDC output is not finite");
    energy += std::norm(value);
  }
  require(energy > 0, "CPU DDC output has no source signal energy");
  const QByteArray outputBytes(
      reinterpret_cast<const char*>(samples.samples.data()),
      static_cast<qsizetype>(samples.samples.size() *
                             sizeof(std::complex<float>)));
  const auto outputHash = QString::fromLatin1(
      QCryptographicHash::hash(outputBytes, QCryptographicHash::Sha256)
          .toHex());
  auto* narrow = window.findChild<NarrowbandWorkspace*>();
  require(narrow && waitUntil(
                        [&] {
                          return narrow->isVisible() &&
                                 narrow->visibleChartsSettled() &&
                                 narrow->visibleChartsGpuReady() &&
                                 narrow->visibleTextureUploads() > 0 &&
                                 narrow->visibleCompletedFrames() > 0 &&
                                 narrow->dataStatusText().contains("真实 DDC");
                        },
                        60000),
          "Native narrowband did not display the actual CPU DDC");
  const auto narrowRendering = narrow->renderStatistics();
  widebandScene(capture, "27-real-algorithm-cpu-ddc", screen);
  click(window, "narrowbandClose");
  require(widebandDisplayReady(window),
          "Return from real DDC did not restore wideband");
  require(window.saveProject(projectPath) && window.openProject(projectPath) &&
              widebandDisplayReady(window),
          "Actual model/DDC project did not reopen");
  require(window.session().activeFile()->channels.back().sourceSnapshot &&
              *window.session().activeFile()->channels.back().sourceSnapshot ==
                  *channel.sourceSnapshot &&
              window.session().activeFile()->channels.back().sourceTime ==
                  channel.sourceTime,
          "Reopen silently changed actual channel provenance or timing");
  widebandSelectAnnotation(window, selectedId, "detection-v1");
  widebandScene(capture, "28-real-algorithm-ddc-saved-reopened", screen);
  selected = findAnnotation(*window.session().activeFile(), selectedId);
  const auto newEnd = activeSegment(*selected)->range.time.end - 1;
  auto* endEdit = child<QLineEdit>(window, "annotationEndMilliseconds");
  widebandScrollTo(endEdit);
  endEdit->setFocus();
  endEdit->selectAll();
  QTest::keyClicks(
      endEdit, QString::number(newEnd / metadata.sampleRateHz * 1000, 'g', 17));
  QTest::keyClick(endEdit, Qt::Key_Return);
  selected = findAnnotation(*window.session().activeFile(), selectedId);
  require(selected->sourceKind == AnnotationSource::Algorithm &&
              selected->originEvidence->originalFieldsJson == originalFields &&
              window.session().activeFile()->channels.back().sourceSnapshot &&
              *window.session().activeFile()->channels.back().sourceSnapshot ==
                  *channel.sourceSnapshot &&
              window.session().activeFile()->channels.back().sourceRelation ==
                  ChannelSourceRelation::Changed,
          "Manual revision rewrote model evidence or silently recomputed "
          "channel source");
  widebandScene(capture, "29-real-algorithm-revision-preserves-ddc", screen);
  require(window.saveProject(projectPath) && window.openProject(projectPath) &&
              widebandDisplayReady(window),
          "Changed model source relation did not reopen");
  require(window.session().activeFile()->channels.back().sourceRelation ==
                  ChannelSourceRelation::Changed &&
              window.session().activeFile()->channels.back().sourceSnapshot &&
              *window.session().activeFile()->channels.back().sourceSnapshot ==
                  *channel.sourceSnapshot,
          "Changed source relation or old DDC snapshot was lost");
  widebandSelectAnnotation(window, selectedId, "detection-v1");
  widebandScene(capture, "30-real-algorithm-changed-source-reopened", screen);
  widebandMenuAction(window, "widebandDetectionAction");
  auto* roiButton = child<QPushButton>(window, "detectionStrategyRoi");
  widebandScrollTo(roiButton);
  widebandNativeClick(roiButton);
  widebandDetectionFold(window, "detectionPostprocessParametersToggle");
  auto* split = child<QCheckBox>(window, "detectionSplitBursts");
  widebandScrollTo(split);
  if (split->isChecked())
    widebandNativeClick(split);
  require(waitUntil([&] { return !split->isChecked(); }),
          "Actual no-split checkbox indicator did not toggle");
  const auto setParameter = [&](const char* name, const char* value) {
    auto* edit = qobject_cast<QLineEdit*>(widebandDetectionControl(window, name));
    require(edit, "Approved ROI parameter is not an editor");
    require(edit->isEnabled(), "Approved ROI parameter is disabled");
    edit->setFocus();
    edit->selectAll();
    QTest::keyClicks(edit, value);
    QTest::keyClick(edit, Qt::Key_Return);
  };
  setParameter("detectionSampleEdgeRatio", "0.75");
  setParameter("detectionSmoothMilliseconds", "2");
  require(
      !child<QLineEdit>(window, "detectionMinOnMilliseconds")->isEnabled() &&
          !child<QLineEdit>(window, "detectionMinOffMilliseconds")->isEnabled(),
      "Approved no-split min on/off matrix changed");
  widebandScene(capture, "31-real-roi-approved-parameter-matrix", screen);
  const auto beforeRoi = window.session().activeFile()->detectionRuns.size();
  button = child<QPushButton>(window, "runWidebandDetection");
  widebandScrollTo(button);
  widebandNativeClick(button);
  require(waitUntil([&] {
            return window.session().activeFile()->detectionRuns.size() ==
                   beforeRoi + 1;
          }),
          "ROI request was not frozen");
  require(
      waitUntil(
          [&] {
            return detectionIsTerminal(
                window.session().activeFile()->detectionRuns.back().lifecycle);
          },
          120000),
      "Actual ROI model task did not finish");
  const auto roiRun = window.session().activeFile()->detectionRuns.back();
  require(
      roiRun.lifecycle == DetectionLifecycle::Completed &&
          roiRun.request.parameters.timeStrategy ==
              DetectionTimeStrategy::Roi &&
          !roiRun.request.parameters.splitBursts,
      "Actual ROI model task failed: " + QString::fromStdString(roiRun.error));
  std::string roiId;
  QJsonObject roiTime;
  for (const auto& annotation : window.session().activeFile()->marks) {
    if (!annotation.originEvidence ||
        annotation.originEvidence->runId != roiRun.request.runId ||
        annotation.originEvidence->refineStatus != RefineStatus::Applied)
      continue;
    const auto fields = QJsonDocument::fromJson(
                            QByteArray::fromStdString(
                                annotation.originEvidence->originalFieldsJson))
                            .object();
    const auto time = fields["timeAnalysis"].toObject();
    if (time["roiSmoothingCells"].toInt() == 11 &&
        time["durationParametersApplied"] == false &&
        time["effectiveEdgeRatio"].toDouble() == .75 &&
        time["roiThresholdDomain"] == "positive-db-residual-mean-v1" &&
        time["roiTimeThreshold"].isDouble() &&
        time["roiFrequencyThreshold"].isDouble()) {
      roiId = annotation.id;
      roiTime = time;
      break;
    }
  }
  require(!roiId.empty(),
          "Actual ROI evidence did not apply approved v2 parameters");
  widebandSelectAnnotation(window, roiId, "detection-v1");
  widebandScene(capture, "32-real-roi-time-result", screen);
  require(window.saveProject(projectPath) && window.openProject(projectPath) &&
              widebandDisplayReady(window),
          "ROI result did not save/reopen");
  widebandSelectAnnotation(window, roiId, "detection-v1");
  widebandScene(capture, "33-real-roi-saved-reopened", screen);
  return {
      {"pass", true},
      {"startFeedbackMilliseconds", startFeedbackMs},
      {"startFeedbackPaints", firstPaint.count()},
      {"startFeedbackTarget", "visible fixed Run/Cancel enabled state"},
      {"run", encodeDetectionRun(run)},
      {"annotationCount", static_cast<int>(ids.size())},
      {"sourceCounts", widebandSourceCounts(*window.session().activeFile())},
      {"modelInferenceVerified", true},
      {"timeEventsVerified", true},
      {"ddcExecutionVerified", true},
      {"ddcOutputSha256", outputHash},
      {"ddcSamples", static_cast<int>(samples.samples.size())},
      {"ddcEnergy", energy},
      {"roiRun", encodeDetectionRun(roiRun)},
      {"roiActualTimeEvidence", roiTime},
      {"roiV2NoSplitParametersVerified", true},
      {"narrowbandRenderEvidence", narrowRendering},
      {"immutableOriginPreserved", true},
      {"changedChannelSnapshotPreserved", true}};
}

#include "tests/wideband_adaptation_capture.h"
#include "tests/wideband_final_capture.h"

int runWidebandDetectionCapture(const QString& outputDirectory,
                                bool productionModel = false,
                                bool finalAcceptance = false) {
  QDir output(outputDirectory);
  if (!QFileInfo(outputDirectory).isAbsolute() ||
      !QDir().mkpath(output.absolutePath()))
    return 2;
  qApp->setProperty("uiStatePersistenceEnabled", false);
  qApp->setProperty("enableDetectionTestFixture", true);
  QJsonObject report{
      {"pass", false},
      {"mode", "wideband-detection"},
      {"platform", QGuiApplication::platformName()},
      {"screens", screensJson()},
      {"startedUtc",
       QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {"evidenceKind",
       "Native Qt Widgets, actual cf32 IQ, pipeline fixture (not a model)"},
      {"fixtureExplicitlyEnabledBeforeMainWindow", true},
      {"modelInferenceVerified", false},
      {"productionModelRequested", productionModel},
      {"finalAcceptanceRequested", finalAcceptance},
      {"a16AcceptanceVerified", false}};
  MainWindow window;
  Capture capture{window,
                  output,
                  {child<PlotWidget>(window, "navigationPlot"),
                   child<PlotWidget>(window, "auxPlot"),
                   child<PlotWidget>(window, "mainPlot")}};
  QScreen* screen = nativeAcceptanceScreen();
  try {
    require(QGuiApplication::platformName() == "windows",
            "Native Windows capture is required");
    require(screen && displayPixelSize(screen) == QSize(3840, 2160) &&
                screen->geometry().size() == QSize(2560, 1440) &&
                std::abs(screen->devicePixelRatio() - 1.5) < .001,
            "Connected selected 4K monitor must use its actual 4K / 150% "
            "system scale");
    report["screen"] = widebandScreenJson(screen);
    require(
        !report["screen"].toObject()["friendlyName"].toString().isEmpty() &&
            !displayDeviceName(screen).isEmpty(),
        "Native acceptance must record the current physical monitor identity");
    for (const auto* name :
         {"QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_FONT_DPI",
          "QT_DEVICE_PIXEL_RATIO", "QT_AUTO_SCREEN_SCALE_FACTOR",
          "QT_ENABLE_HIGHDPI_SCALING", "QT_SCALE_FACTOR_ROUNDING_POLICY",
          "QT_USE_PHYSICAL_DPI"})
      require(qgetenv(name).isEmpty(),
              QString("DPI override must be absent: %1").arg(name));
    showFullScreenOnScreen(window, screen);
    require(
        QTest::qWaitForWindowExposed(&window, 5000) &&
            waitUntil(
                [&] {
                  return window.isFullScreen() && window.screen() == screen &&
                         window.size() == QSize(2560, 1440);
                },
                5000),
        "The native main window did not occupy the selected native 4K monitor "
        "fullscreen");
    if (finalAcceptance) {
      require(productionModel,
              "Final acceptance requires the real model journey");
      capture.scene(
          "00-no-project-workflow", nullptr,
          [&] {
            require(!window.session().activeFile(),
                    "No-project entry unexpectedly contains a source file");
          },
          false);
      auto scene = capture.scenes.last().toObject();
      scene["captureKind"] = "native-main-window";
      scene["screen"] = widebandScreenJson(screen);
      capture.scenes[0] = scene;
      require(scene["pass"].toBool(), scene["error"].toString());
    }
    const auto projectName = "wideband-project-" +
                             QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString error;
    const bool created =
        window.createProject(output.absolutePath(), projectName, &error);
    require(created, error);
    const auto project = output.filePath(projectName + "/project.json");
    report["projectPath"] = project;
    const QString data = widebandRepresentativeDataPath();
    const auto meta = data.chopped(11) + ".sigmf-meta";
    const auto iqHash = widebandFileHash(data);
    require(
        iqHash ==
            "3a47e55b5f02851d702d5c5782998bf3e374f67cde592c35689acf9ec5399254",
        "Actual IQ SHA-256 differs from the frozen M0 asset");
    auto source = widebandImportActualSource(window, data, meta, screen);
    source["dataPath"] = data;
    source["metaPath"] = meta;
    source["iqSha256"] = iqHash;
    source["metaSha256"] = widebandFileHash(meta);
    source["dataBytes"] = QString::number(QFileInfo(data).size());
    report["source"] = source;
    const bool imported = window.importLabels(meta, &error, false);
    require(imported, error);
    auto* file = window.session().activeFile();
    require(widebandSourceCounts(*file)["importedLabels"].toInt() == 4,
            "The actual paired metadata did not produce four original labels");
    const auto originalId = file->marks.front().id;
    window.refresh();
    click(window, "psdMode");
    file->display.mainMode = MainMode::TimeFrequency;
    file->display.stftSize = 2048;
    file->display.psdSize = 4096;
    file->display.grid = true;
    file->display.colorScale = true;
    window.refresh();
    require(widebandDisplayReady(window),
            "Actual IQ charts did not settle on hardware QRhi");
    startCreating(capture.plots[2]);
    createMark(capture.plots[2], file->view, .20, .45, .60, .75);
    QTest::keyClick(capture.plots[2], Qt::Key_Escape);
    require(widebandSourceCounts(*window.session().activeFile())["manual"]
                    .toInt() == 1,
            "A real native chart drag did not create the manual annotation");
    widebandSelectAnnotation(window, originalId);
    report["configurationEntrances"] =
        widebandCheckConfigurationEntrances(capture);
    report["currentFileProperties"] = widebandCurrentFileProperties(window);
    widebandResidentCardAtTop(window);
    widebandScene(capture, "01-imported-manual-resident-card", screen);
    report["residentCardPlacement"] =
        QJsonObject{{"pass", true},
                    {"propertyViewportAtTop", true},
                    {"currentFileAboveResidentCardVisible", false},
                    {"currentFileFactsVisibleViaProperties", true},
                    {"placementContract", "W4 contextual inspector; Display/file properties and Detection"},
                    {"modelAndBasicConfigurationVisible", true},
                    {"fixedRunAndCancelActionsFullyVisible", true}};
    report["configuration"] = widebandConfigureAndStart(capture, screen);
    const auto run = widebandWaitForRun(window);
    report["run"] = encodeDetectionRun(run);
    report["modelInferenceVerified"] = false;
    const auto rawCandidates = widebandRawCandidateCount(project, run);
    const auto algorithmObservations =
        widebandSourceCounts(*window.session().activeFile())["algorithm"]
            .toInt();
    report["windowRecordCount"] = QString::number(run.resultCount);
    report["rawCandidateCount"] = QString::number(rawCandidates);
    report["algorithmObservationCount"] = algorithmObservations;
    require(rawCandidates == 3 && algorithmObservations == 3,
            "The explicitly non-model fixture must publish three actual "
            "candidate/observation records");
    report["zeroCandidates"] = rawCandidates == 0;
    report["prototypeComparisonVerified"] = false;
    click(window, "taskTab");
    widebandExpandTaskArea(window);
    widebandScene(capture, "03-completed-task-cards", screen);
    widebandCaptureTaskDetails(capture, screen);
    widebandSelectAnnotation(window, originalId);
    report["sourceCounts"] =
        widebandSourceCounts(*window.session().activeFile());
    report["revision"] = widebandReviseImported(capture, screen, originalId);
    const auto before = encodeAnnotation(
        *findAnnotation(*window.session().activeFile(), originalId));
    QJsonArray originalSources;
    for (const auto& set : window.session().activeFile()->sourceSets)
      originalSources.append(encodeSourceSet(set));
    require(window.saveProject(project), "Native project save failed");
    require(window.openProject(project),
            "Native saved project could not reopen");
    require(widebandDisplayReady(window),
            "Reopened real source charts did not settle");
    const auto* reopenedAnnotation =
        findAnnotation(*window.session().activeFile(), originalId);
    require(
        reopenedAnnotation && encodeAnnotation(*reopenedAnnotation) == before,
        "Annotation, immutable origin or revision/review chain changed after "
        "save/reopen");
    QJsonArray reopenedSources;
    for (const auto& set : window.session().activeFile()->sourceSets)
      reopenedSources.append(encodeSourceSet(set));
    require(reopenedSources == originalSources,
            "Full original source text, hash or source-set snapshot changed "
            "after reopen");
    require(
        window.session().activeFile()->detectionRuns.size() == 1 &&
            encodeDetectionRun(
                window.session().activeFile()->detectionRuns.front()) ==
                encodeDetectionRun(run),
        "Completed task/coverage snapshot changed after native project reopen");
    require(widebandFileHash(data) == iqHash,
            "Actual IQ content changed during native acceptance");
    widebandSelectAnnotation(window, originalId);
    widebandScene(capture, "06-saved-reopened", screen);
    require(widebandSourceCounts(*window.session().activeFile()) ==
                QJsonObject{{"importedLabels", 4},
                            {"manual", 1},
                            {"algorithm", 3},
                            {"total", 8}},
            "Native save/reopen changed the four labels, one manual mark "
            "or three fixture observations");
    report["reopen"] = QJsonObject{
        {"pass", true},
        {"projectPath", project},
        {"sourceCounts", widebandSourceCounts(*window.session().activeFile())},
        {"annotationOriginRevisionReviewPreserved", true},
        {"fullSourceTextAndHashesPreserved", true},
        {"completedRunAndCoveragePreserved", true}};
    report["roiInteraction"] = widebandCaptureRoi(capture, screen);
    report["panelLayoutInteraction"] =
        widebandCapturePanelLayouts(capture, screen);
    report["auxiliaryEvidenceInteraction"] =
        widebandCaptureAuxiliaryEvidence(capture, screen, originalId);
    report["pendingSegmentChannel"] = widebandCapturePendingSegmentChannel(
        capture, screen, originalId, project, data, iqHash);
    if (productionModel) {
      report["productionModel"] =
          widebandCaptureProductionModel(capture, screen, project);
      report["modelInferenceVerified"] = true;
      report["timeEventsVerified"] = true;
      report["evidenceKind"] =
          "Native Qt Widgets, actual cf32 IQ, explicit fixture regression and "
          "real TensorRT FP32 inference, sample refinement and real CPU DDC";
      report["adaptationClosure"] =
          widebandAdaptationClosure(capture, screen, project);
      report["reviewClosure"] =
          widebandFinalAcceptance(capture, screen, project, finalAcceptance);
      if (finalAcceptance)
        report["realTaskFeedback"] =
            widebandCancellationFeedback(capture, screen, project);
    }
    report["renderEvidence"] = capture.evidence();
    report["fullScreen"] = window.isFullScreen();
    report["logicalWindowSize"] = sizeJson(window.size());
    const int kExpectedScenes = finalAcceptance   ? 46
                                : productionModel ? 43
                                                  : 23;
    QStringList uniqueScenes;
    for (const auto& value : capture.scenes) {
      const auto name = value.toObject()["scene"].toString();
      require(!name.isEmpty() && !uniqueScenes.contains(name),
              "Native acceptance contains an empty or duplicate scene name");
      uniqueScenes.append(name);
    }
    require(capture.scenes.size() == kExpectedScenes &&
                uniqueScenes.size() == kExpectedScenes,
            "Native wideband acceptance must contain all 23 unique scenes");
    report["expectedSceneCount"] = kExpectedScenes;
    report["uniqueSceneCount"] = uniqueScenes.size();
    report["pass"] = capture.passed;
  } catch (const std::exception& failure) {
    report["error"] = QString::fromUtf8(failure.what());
    const auto diagnostic = output.filePath("failed-native-diagnostic.png");
    if (window.grab().save(diagnostic))
      report["failureDiagnostic"] = diagnostic;
  }
  report["scenes"] = capture.scenes;
  report["sceneCount"] = capture.scenes.size();
  report["finishedUtc"] =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  QFile result(output.filePath("wideband-detection-report.json"));
  const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
  if (!result.open(QIODevice::WriteOnly) || result.write(bytes) != bytes.size())
    return 2;
  return report["pass"].toBool() ? 0 : 1;
}
