// Independent native UI acceptance diagnostics. This executable uses the real
// Windows QRhi backend and visible controls. The linked-cursor mode drives
// actual IQ/DSP through the application; original scene captures use demo data.
#include "app/main_window.h"
#include "ui/charts/plot_widget.h"
#include "ui/charts/accelerated_surface.h"
#include "ui/narrowband_workspace.h"
#include "ui/display_target.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPixmap>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QWindow>
#include <QWheelEvent>
#include <QtTest>

#include <array>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace signalstudio;

namespace {

void require(bool condition, const QString& message) {
    if (!condition) throw std::runtime_error(message.toStdString());
}

bool waitUntil(const std::function<bool()>& ready, int timeoutMs = 2000) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (ready()) return true;
        QTest::qWait(10);
    } while (timer.elapsed() < timeoutMs);
    return ready();
}

QJsonObject sizeJson(QSize size) {
    return {{"width", size.width()}, {"height", size.height()}};
}

QJsonObject rectJson(QRect rect) {
    return {{"x", rect.x()}, {"y", rect.y()},
            {"width", rect.width()}, {"height", rect.height()}};
}

QJsonObject pointJson(QPoint point) {
    return {{"x", point.x()}, {"y", point.y()}};
}

class MouseMoveEvidence final : public QObject {
public:
    explicit MouseMoveEvidence(QWidget* target) : target_(target) { target->installEventFilter(this); }
    ~MouseMoveEvidence() override { if (target_) target_->removeEventFilter(this); }
    int count() const { return static_cast<int>(events_.size()); }
    QPointF lastPosition() const { return lastPosition_; }
    QJsonArray events() const { return events_; }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == target_ && event->type() == QEvent::MouseMove) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            lastPosition_ = mouse->position();
            events_.append(QJsonObject{{"localX", mouse->position().x()}, {"localY", mouse->position().y()},
                {"globalX", mouse->globalPosition().x()}, {"globalY", mouse->globalPosition().y()},
                {"buttons", static_cast<int>(mouse->buttons())}, {"spontaneous", mouse->spontaneous()}});
        }
        return false;
    }
private:
    QPointer<QWidget> target_;
    QPointF lastPosition_;
    QJsonArray events_;
};

void moveThroughWindow(QWidget* plot, QPoint point) {
    auto* native = plot->window()->windowHandle();
    require(native != nullptr, "Hover injection requires a native window handle");
    // Unlike the QWidget overload's NoButton QCursor warp, this QTest overload
    // injects a QPA mouse event that is routed through the real widget hierarchy.
    QTest::mouseMove(native, native->mapFromGlobal(plot->mapToGlobal(point)), 10);
}

QJsonObject snapshotJson(const ViewSnapshot& snapshot) {
    return {{"fileId", QString::fromStdString(snapshot.fileId)},
        {"beginSample", QString::number(snapshot.view.time.begin)},
        {"endSample", QString::number(snapshot.view.time.end)},
        {"lowerHz", snapshot.view.frequency.lowerHz}, {"upperHz", snapshot.view.frequency.upperHz},
        {"waveformMin", snapshot.waveformMin}, {"waveformMax", snapshot.waveformMax},
        {"psdMin", snapshot.psdMin}, {"psdMax", snapshot.psdMax}};
}

QJsonObject screenJson(const QScreen* screen) {
    if (!screen) return {};
    const auto geometry = screen->geometry();
    const auto dpr = screen->devicePixelRatio();
    return {{"name", screen->name()}, {"deviceName", displayDeviceName(screen)},
        {"connectedIndex", QGuiApplication::screens().indexOf(const_cast<QScreen*>(screen)) + 1},
        {"logicalGeometry", rectJson(geometry)},
        {"availableLogicalGeometry", rectJson(screen->availableGeometry())},
        {"logicalDpi", screen->logicalDotsPerInch()}, {"physicalDpi", screen->physicalDotsPerInch()},
        {"devicePixelRatio", dpr},
        {"pixelSize", sizeJson(displayPixelSize(screen))},
        {"primary", screen == QGuiApplication::primaryScreen()}};
}

QJsonArray screensJson() {
    QJsonArray screens;
    for (const auto* screen : QGuiApplication::screens()) screens.append(screenJson(screen));
    return screens;
}

bool writeReport(const QDir& output, const QJsonObject& report) {
    QFile file(output.filePath("native-capture-report.json"));
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

template<class T> T* child(QObject& parent, const char* name) {
    auto* widget = parent.findChild<T*>(QString::fromLatin1(name));
    require(widget != nullptr, QString("Missing UI control: %1").arg(name));
    return widget;
}

void click(QObject& parent, const char* name) {
    auto* button = child<QAbstractButton>(parent, name);
    require(button->isVisible() && button->isEnabled(),
            QString("Control is not clickable: %1").arg(name));
    auto* checkbox = qobject_cast<QCheckBox*>(button);
    const bool previouslyChecked = checkbox && checkbox->isChecked();
    if (checkbox) {
        QStyleOptionButton option;
        option.initFrom(checkbox);
        option.text = checkbox->text();
        const auto indicator = checkbox->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, checkbox);
        require(indicator.isValid() && checkbox->rect().contains(indicator.center()),
                QString("Checkbox indicator is not visible: %1").arg(name));
        QTest::mouseClick(checkbox, Qt::LeftButton, Qt::NoModifier, indicator.center());
    } else QTest::mouseClick(button, Qt::LeftButton);
    QTest::qWait(80);
    if (checkbox) {
        require(checkbox->isChecked() != previouslyChecked,
                QString("Actual indicator click did not toggle checkbox: %1").arg(name));
        if (auto* window = qobject_cast<MainWindow*>(&parent)) {
            const auto* file = window->session().activeFile();
            require(file != nullptr, "Checkbox interaction requires an active file");
            if (QString::fromLatin1(name) == "colorbarToggle")
                require(file->display.colorScale == checkbox->isChecked(), "Colorbar checkbox and business state differ");
            if (QString::fromLatin1(name) == "gridToggle")
                require(file->display.grid == checkbox->isChecked(), "Grid checkbox and business state differ");
        }
    }
}

struct Capture {
    MainWindow& window;
    QDir output;
    std::array<PlotWidget*, 3> plots;
    QJsonArray scenes;
    bool passed = true;
    std::array<quint64, 3> previousUploads{}, previousFrames{}, previousGenerations{};

    void waitForSettledDisplay() {
        require(waitUntil([this] { return plots[2]->isDisplaySettled(); }),
                "Main chart did not settle on the latest requested display");
        QTest::qWait(80);
        require(plots[2]->isDisplaySettled(), "Main chart resumed preview before capture");
    }

    void waitForGpu() {
        require(waitUntil([this] {
            for (const auto* plot : plots)
                if (!plot->gpuReady() || plot->completedFrameCount() == 0) return false;
            return true;
        }), "All three charts must initialize hardware QRhi and submit frames");
    }

    void waitForMainFrame(quint64 before) {
        require(waitUntil([this, before] {
            return plots[2]->gpuReady() && plots[2]->completedFrameCount() > before;
        }), "Main chart did not submit a new hardware frame");
    }

    QJsonObject evidence() const {
        QJsonArray charts;
        for (std::size_t index = 0; index < plots.size(); ++index) {
            const auto* plot = plots[index];
            charts.append(QJsonObject{{"name", plot->objectName()},
                {"backend", plot->renderingBackend()}, {"gpuReady", plot->gpuReady()},
                {"completedFrames", static_cast<qint64>(plot->completedFrameCount())},
                {"textureUploads", static_cast<qint64>(plot->textureUploadCount())},
                {"powerGenerations", static_cast<qint64>(plot->powerGenerationCount())},
                {"renderStatistics", plot->renderStatistics()},
                {"newFramesSincePreviousScene", static_cast<qint64>(plot->completedFrameCount() - previousFrames[index])},
                {"newTextureUploadsSincePreviousScene", static_cast<qint64>(plot->textureUploadCount() - previousUploads[index])},
                {"heatmapTextureReusedFromPreviousScene", !scenes.isEmpty() && plot->textureUploadCount() == previousUploads[index]},
                {"powerCacheReusedFromPreviousScene", !scenes.isEmpty() && plot->powerGenerationCount() == previousGenerations[index]},
                {"logicalWidgetSize", sizeJson(plot->size())},
                {"plotRectHeight", plot->plotRect().height()},
                {"visible", plot->isVisible()}});
        }
        const auto* file = window.session().activeFile();
        QJsonArray marks;
        QJsonArray selected;
        if (file) {
            for (const auto& id : file->selectedMarkIds)
                selected.append(QString::fromStdString(id));
            for (const auto& mark : file->marks) {
                marks.append(QJsonObject{{"id", QString::fromStdString(mark.id)},
                    {"name", QString::fromStdString(mark.name)},
                    {"beginSample", QString::number(mark.range.time.begin)},
                    {"endSample", QString::number(mark.range.time.end)},
                    {"lowerHz", mark.range.frequency.lowerHz},
                    {"upperHz", mark.range.frequency.upperHz}});
            }
        }
        return {{"devicePixelRatio", window.devicePixelRatioF()},
            {"logicalWindowSize", sizeJson(window.size())},
            {"windowScreen", screenJson(window.screen())},
            {"windowFullScreen", window.isFullScreen()},
            {"mainWidgetHeight", plots[2]->height()},
            {"actualMainPlotHeight", plots[2]->plotRect().height()},
            {"files", static_cast<int>(window.session().project().files.size())},
            {"marks", marks}, {"selectedMarkIds", selected},
            {"creating", plots[2]->isCreating()},
            {"mainMode", file && file->display.mainMode == MainMode::Waterfall ? "waterfall" : "time-frequency"},
            {"auxiliaryMode", file && file->display.auxiliaryMode == AuxiliaryMode::Psd ? "psd" : "waveform"},
            {"colorScale", file && file->display.colorScale},
            {"absoluteFrequency", !file || file->display.absoluteFrequency},
            {"charts", charts}};
    }

    void scene(const QString& name, QWidget* target = nullptr,
               const std::function<void()>& verify = {}) {
        QTest::qWait(80);
        QString failure;
        try {
            waitForGpu();
            waitForSettledDisplay();
            if (verify) verify();
        } catch (const std::exception& error) {
            failure = QString::fromUtf8(error.what());
        }
        auto record = evidence();
        record["scene"] = name;
        record["parentDevicePixelRatio"] = window.devicePixelRatioF();
        QWidget* captured = target ? target : &window;
        const auto pixmap = captured->grab();
        record["capturedPixelSize"] = sizeJson(pixmap.size());
        record["capturedDevicePixelRatio"] = pixmap.devicePixelRatioF();
        record["capturedLogicalWidgetSize"] = sizeJson(captured->size());
        record["capturedScreen"] = screenJson(captured->screen());
        if (captured == &window && window.session().activeFile() &&
            (name == "01_default_4k_150" || name == "05_psd")) {
            const auto image = pixmap.toImage();
            const auto logicalRegion = plots[1]->plotRect().adjusted(2, 2, -2, -2)
                .translated(plots[1]->mapTo(&window, QPoint()));
            const auto dpr = pixmap.devicePixelRatioF();
            const QRect pixels(QPoint(static_cast<int>(std::floor(logicalRegion.left() * dpr)),
                                      static_cast<int>(std::floor(logicalRegion.top() * dpr))),
                               QPoint(static_cast<int>(std::ceil(logicalRegion.right() * dpr)),
                                      static_cast<int>(std::ceil(logicalRegion.bottom() * dpr))));
            const auto region = pixels.intersected(image.rect());
            const bool psd = window.session().activeFile()->display.auxiliaryMode == AuxiliaryMode::Psd;
            qint64 tracePixels = 0;
            for (int y = region.top(); y <= region.bottom(); ++y)
                for (int x = region.left(); x <= region.right(); ++x) {
                    const auto color = image.pixelColor(x, y);
                    const bool curve = psd ? color.blue() > color.red() + 40 &&
                        color.blue() > color.green() + 10 && color.blue() > 130 :
                        color.green() > color.red() + 40 && color.green() > color.blue() + 10 && color.green() > 130;
                    if (curve) ++tracePixels;
                }
            record["auxTracePixels"] = tracePixels;
            record["auxTracePixelRegion"] = rectJson(region);
            record["auxTraceColor"] = psd ? "dominant blue" : "dominant green";
            record["minimumAuxTracePixels"] = 100;
            if (tracePixels < 100) {
                if (!failure.isEmpty()) failure += "; ";
                failure += "Captured auxiliary curve has fewer than 100 visible trace pixels";
            }
        }
        const auto filename = name + ".png";
        if (pixmap.isNull() || !pixmap.save(output.filePath(filename))) {
            if (!failure.isEmpty()) failure += "; ";
            failure += "Could not save PNG screenshot";
        }
        if (captured == &window &&
            (!isDisplayNumber(window.screen(), 2) ||
             !window.isFullScreen() || window.size() != QSize(2560, 1440) || pixmap.size() != QSize(3840, 2160) ||
             std::abs(pixmap.devicePixelRatioF() - 1.5) > 0.001)) {
            if (!failure.isEmpty()) failure += "; ";
            failure += "Window capture must be logical 2560x1440, physical 3840x2160, DPR 1.5";
        }
        record["png"] = output.filePath(filename);
        record["pass"] = failure.isEmpty();
        if (!failure.isEmpty()) record["error"] = failure;
        scenes.append(record);
        for (std::size_t index = 0; index < plots.size(); ++index) {
            previousUploads[index] = plots[index]->textureUploadCount();
            previousFrames[index] = plots[index]->completedFrameCount();
            previousGenerations[index] = plots[index]->powerGenerationCount();
        }
        passed = passed && failure.isEmpty();
        QTextStream(stdout) << name << ": " << (failure.isEmpty() ? "PASS" : failure) << '\n';
    }
};

QJsonArray chartRenderStatistics(const Capture& capture) {
    QJsonArray charts;
    for (const auto* plot : capture.plots)
        charts.append(QJsonObject{{"name", plot->objectName()}, {"renderStatistics", plot->renderStatistics()}});
    return charts;
}

void createMark(PlotWidget* plot, const ViewRange& view,
                double t0, double t1, double f0, double f1) {
    const auto timeAt = [&view](double fraction) {
        return view.time.begin + static_cast<SampleIndex>(
            static_cast<long double>(view.time.end - view.time.begin) * fraction);
    };
    const auto frequencyAt = [&view](double fraction) {
        return view.frequency.lowerHz + (view.frequency.upperHz - view.frequency.lowerHz) * fraction;
    };
    const auto start = plot->toPixel(timeAt(t0), frequencyAt(f0)).toPoint();
    const auto end = plot->toPixel(timeAt(t1), frequencyAt(f1)).toPoint();
    require(plot->plotRect().contains(start) && plot->plotRect().contains(end),
            "Mark drag endpoints must lie inside the actual chart");
    QTest::mousePress(plot, Qt::LeftButton, Qt::NoModifier, start);
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(plot, start + (end - start) * step / 8, 10);
    QTest::mouseRelease(plot, Qt::LeftButton, Qt::NoModifier, end);
    QTest::qWait(80);
}

void startCreating(PlotWidget* plot) {
    const auto point = plot->plotRect().center().toPoint();
    std::unique_ptr<QMenu> menu(plot->createContextMenu(point));
    auto* action = child<QAction>(*menu, "contextMarkToggle");
    menu->popup(plot->mapToGlobal(point));
    require(waitUntil([&menu] { return menu->isVisible(); }), "Chart context menu did not open");
    QTest::mouseClick(menu.get(), Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(action).center());
    require(waitUntil([plot] { return plot->isCreating(); }), "Continuous mark mode did not activate");
}

QList<QTreeWidgetItem*> markItems(QTreeWidget& tree) {
    QList<QTreeWidgetItem*> result;
    for (QTreeWidgetItemIterator iterator(&tree); *iterator; ++iterator)
        if ((*iterator)->data(0, Qt::UserRole).toString() == "mark") result.append(*iterator);
    return result;
}

void chooseRelativeFrequency(MainWindow& window) {
    auto* combo = child<QComboBox>(window, "freqMode");
    child<QScrollArea>(window, "propScroll")->ensureWidgetVisible(combo);
    QTest::qWait(80);
    require(combo->isVisible(), "Frequency coordinate control is not visible");
    QTest::mouseClick(combo, Qt::LeftButton);
    require(waitUntil([combo] { return combo->view()->isVisible(); }), "Frequency coordinate popup did not open");
    // Use ordinary keyboard selection in the native popup. Direct test mouse
    // delivery to its viewport does not activate rows on the acceptance host.
    QTest::qWait(80);
    combo->view()->setFocus(Qt::OtherFocusReason);
    QTest::keyClick(combo->view(), Qt::Key_End);
    QTest::keyClick(combo->view(), Qt::Key_Return);
    QTest::qWait(80);
    require(waitUntil([combo, &window] { return combo->currentIndex() == 1 &&
                !combo->view()->isVisible() && window.session().activeFile() &&
                !window.session().activeFile()->display.absoluteFrequency; }),
            QString("Relative frequency UI selection failed: index=%1 popupVisible=%2")
                .arg(combo->currentIndex()).arg(combo->view()->isVisible()));
}

int clickChartContextAction(PlotWidget* plot, const char* name) {
    const auto point = plot->plotRect().center().toPoint();
    std::unique_ptr<QMenu> menu(plot->createContextMenu(point));
    auto* action = child<QAction>(*menu, name);
    QSignalSpy triggered(action, &QAction::triggered);
    require(action->isEnabled(), QString("Chart context action is disabled: %1").arg(name));
    menu->popup(plot->mapToGlobal(point));
    require(waitUntil([&menu] { return menu->isVisible(); }), "Chart context menu did not open");
    QTest::qWait(80);
    QTest::mouseClick(menu.get(), Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(action).center());
    require(waitUntil([&menu] { return !menu->isVisible(); }), "Chart context action did not close its menu");
    return triggered.count();
}

QJsonObject responseSummary(const std::vector<double>& measurements) {
    if (measurements.empty()) return {};
    auto sorted = measurements;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double quantile) {
        const auto index = static_cast<std::size_t>(std::ceil(quantile * sorted.size())) - 1;
        return sorted[std::min(index, sorted.size() - 1)];
    };
    return {{"unit", "ms"}, {"samples", static_cast<int>(sorted.size())},
        {"p50", percentile(.50)}, {"p95", percentile(.95)}, {"max", sorted.back()},
        {"measurement", "Synchronous QCoreApplication::sendEvent return time, including direct UI slots"}};
}

void diagnoseRapidWheel(Capture& capture, QJsonObject& diagnostic) {
    capture.waitForSettledDisplay();
    auto* plot = capture.plots[2];
    const auto original = capture.window.session().snapshot();
    const auto before = plot->renderStatistics();
    diagnostic = {{"pass", false}, {"before", before}, {"syntheticDataOnly", true},
        {"restoreAction", "contextBackAction"}, {"frameCountersAreNotFps", true},
        {"originalSnapshot", snapshotJson(original)}, {"eventPumpBetweenWheelEvents", false},
        {"wheelGroupTimeoutMs", 220}, {"canBackBeforeBurst", capture.window.session().canBack()}};
    constexpr std::array deltas{120, 120, -120, 240, -120, 120, 120, -120, 240, 120, -120, 120};
    std::vector<double> responseMs;
    QJsonArray events;
    qint64 maximumPreviewRequestedPixels = 0;
    bool acceptedPreviewObserved = false;
    QElapsedTimer burstTimer;
    burstTimer.start();
    double previousEventStartMs = 0, maximumIntervalMs = 0;
    const auto point = plot->plotRect().center();
    for (std::size_t index = 0; index < deltas.size(); ++index) {
        const double eventStartMs = burstTimer.nsecsElapsed() / 1e6;
        const double intervalMs = index ? eventStartMs - previousEventStartMs : 0;
        previousEventStartMs = eventStartMs;
        maximumIntervalMs = std::max(maximumIntervalMs, intervalMs);
        const auto previous = capture.window.session().activeFile()->view;
        QWheelEvent event(point, plot->mapToGlobal(point.toPoint()), QPoint(), QPoint(0, deltas[index]),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QElapsedTimer timer;
        timer.start();
        QCoreApplication::sendEvent(plot, &event);
        const auto elapsed = timer.nsecsElapsed() / 1e6;
        const auto immediate = plot->renderStatistics();
        if (immediate.value("quality").toString() == "preview")
            maximumPreviewRequestedPixels = std::max(maximumPreviewRequestedPixels,
                                                     immediate.value("requestedPixels").toInteger());
        acceptedPreviewObserved = acceptedPreviewObserved || immediate.value("acceptedHeatmapPreview").toBool();
        responseMs.push_back(elapsed);
        // Worker submission is synchronous, so it can supersede older work
        // without pumping native paint events or wheel-history timers here.
        events.append(QJsonObject{{"event", static_cast<int>(index + 1)}, {"angleDeltaY", deltas[index]},
            {"responseMs", elapsed}, {"accepted", event.isAccepted()},
            {"timeSincePreviousEventMs", intervalMs},
            {"immediateRenderStatistics", immediate},
            {"eventPumpPerformed", false}});
        diagnostic["events"] = events;
        diagnostic["eventResponse"] = responseSummary(responseMs);
        diagnostic["maximumPreviewRequestedPixels"] = maximumPreviewRequestedPixels;
        diagnostic["acceptedPreviewObserved"] = acceptedPreviewObserved;
        require(event.isAccepted() && capture.window.session().activeFile()->view != previous,
                "A rapid wheel event did not change the real main-chart view");
    }
    diagnostic["totalBurstMs"] = burstTimer.nsecsElapsed() / 1e6;
    diagnostic["maximumEventIntervalMs"] = maximumIntervalMs;
    require(maximumIntervalMs < 220 && diagnostic["totalBurstMs"].toDouble() < 220,
            "The intended rapid wheel burst exceeded the 220 ms grouping interval");
    diagnostic["afterLastEvent"] = plot->renderStatistics();
    QElapsedTimer firstPump;
    firstPump.start();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    diagnostic["firstPostBurstEventPumpMs"] = firstPump.nsecsElapsed() / 1e6;
    diagnostic["afterPostBurstEventPump"] = plot->renderStatistics();
    capture.waitForSettledDisplay();
    require(waitUntil([plot] { return !plot->hasPendingInteraction(); }),
            "Rapid wheel group did not commit its view history");
    const auto settled = plot->renderStatistics();
    diagnostic["settled"] = settled;
    diagnostic["lastWheelRequestedGeneration"] = diagnostic["afterLastEvent"].toObject().value("requestedGeneration");
    diagnostic["finalRequestedGeneration"] = settled.value("requestedGeneration");
    diagnostic["finalCommittedGeneration"] = settled.value("committedGeneration");
    diagnostic["settledRequestedPixels"] = settled.value("requestedPixels");
    diagnostic["settledAcceptedPixels"] = settled.value("heatmapWidth").toInteger() * settled.value("heatmapHeight").toInteger();
    require(maximumPreviewRequestedPixels > 0 && maximumPreviewRequestedPixels <= 54'000 &&
            maximumPreviewRequestedPixels < settled.value("requestedPixels").toInteger(),
            "Rapid wheel must request a nonzero preview within 54000 pixels and below settled size");
    const bool currentCommitted = settled.value("requestedGeneration").toInteger() > 0 &&
        settled.value("committedGeneration") == settled.value("requestedGeneration") &&
        settled.value("workerIdle").toBool() && !settled.value("acceptedHeatmapPreview").toBool() &&
        !settled.value("gpuUploadsPending").toBool();
    diagnostic["onlyCurrentGenerationCommittedAtSettle"] = currentCommitted;
    diagnostic["settledOnCurrentRequest"] = plot->isDisplaySettled();
    diagnostic["staleDropsDuringDiagnostic"] = settled.value("staleDrops").toInteger() - before.value("staleDrops").toInteger();
    diagnostic["heatmapCacheHitsDuringDiagnostic"] = settled.value("heatmapCacheHits").toInteger() - before.value("heatmapCacheHits").toInteger();
    require(plot->isDisplaySettled() && settled.value("settled").toBool() &&
            currentCommitted && settled.value("requestedGeneration").toInteger() >= diagnostic["lastWheelRequestedGeneration"].toInteger(),
            "Rapid wheel rendering did not settle on the latest request");
    diagnostic["beforeContextBackSnapshot"] = snapshotJson(capture.window.session().snapshot());
    diagnostic["canBackBeforeContextBack"] = capture.window.session().canBack();
    diagnostic["contextBackTriggeredCount"] = clickChartContextAction(plot, "contextBackAction");
    capture.waitForSettledDisplay();
    diagnostic["afterContextBack"] = plot->renderStatistics();
    diagnostic["afterContextBackSnapshot"] = snapshotJson(capture.window.session().snapshot());
    diagnostic["canBackAfterContextBack"] = capture.window.session().canBack();
    diagnostic["canForwardAfterContextBack"] = capture.window.session().canForward();
    diagnostic["originalViewRestored"] = capture.window.session().snapshot() == original;
    require(diagnostic["contextBackTriggeredCount"].toInt() == 1 && capture.window.session().snapshot() == original,
            "Actual chart context Back did not restore the pre-diagnostic view");
    diagnostic["pass"] = true;
}

void diagnoseOrdinaryEventPump(Capture& capture, QJsonObject& diagnostic) {
    capture.waitForSettledDisplay();
    auto* plot = capture.plots[2];
    const auto original = capture.window.session().snapshot();
    diagnostic = {{"pass", false}, {"syntheticDataOnly", true}, {"frameCountersAreNotFps", true},
        {"originalSnapshot", snapshotJson(original)}, {"noAssumedHistoryGrouping", true},
        {"restoreAction", "contextBackAction"}, {"maximumBackActions", 4},
        {"initialCharts", chartRenderStatistics(capture)}};
    constexpr std::array deltas{120, 120, -120, 240};
    std::vector<double> eventMs, pumpMs;
    QJsonArray events;
    QElapsedTimer burst;
    burst.start();
    double previousStartMs = 0;
    const auto point = plot->plotRect().center();
    for (std::size_t index = 0; index < deltas.size(); ++index) {
        const double startMs = burst.nsecsElapsed() / 1e6;
        const double intervalMs = index ? startMs - previousStartMs : 0;
        previousStartMs = startMs;
        const auto previous = capture.window.session().snapshot();
        QWheelEvent event(point, plot->mapToGlobal(point.toPoint()), QPoint(), QPoint(0, deltas[index]),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QElapsedTimer timer;
        timer.start();
        QCoreApplication::sendEvent(plot, &event);
        const double sendMs = timer.nsecsElapsed() / 1e6;
        const auto immediate = plot->renderStatistics();
        const auto immediateCharts = chartRenderStatistics(capture);
        timer.restart();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        const double processMs = timer.nsecsElapsed() / 1e6;
        eventMs.push_back(sendMs); pumpMs.push_back(processMs);
        events.append(QJsonObject{{"event", static_cast<int>(index + 1)}, {"angleDeltaY", deltas[index]},
            {"sendEventMs", sendMs}, {"processEventsElapsedMs", processMs},
            {"timeSincePreviousEventMs", intervalMs}, {"accepted", event.isAccepted()},
            {"immediateRenderStatistics", immediate}, {"afterProcessRenderStatistics", plot->renderStatistics()},
            {"immediateCharts", immediateCharts}, {"afterProcessCharts", chartRenderStatistics(capture)},
            {"snapshot", snapshotJson(capture.window.session().snapshot())}});
        diagnostic["events"] = events;
        require(event.isAccepted() && capture.window.session().snapshot() != previous,
                "An ordinary pumped wheel event did not change the actual view");
    }
    diagnostic["totalBurstMs"] = burst.nsecsElapsed() / 1e6;
    diagnostic["eventResponse"] = responseSummary(eventMs);
    auto pumpSummary = responseSummary(pumpMs);
    pumpSummary["measurement"] = "Actual processEvents(AllEvents, 2) elapsed time, including native paint events";
    diagnostic["eventPumpResponse"] = pumpSummary;
    capture.waitForSettledDisplay();
    require(waitUntil([plot] { return !plot->hasPendingInteraction(); }), "Ordinary wheel history did not finish");
    diagnostic["settledCharts"] = chartRenderStatistics(capture);
    diagnostic["beforeContextBackSnapshot"] = snapshotJson(capture.window.session().snapshot());
    QJsonArray backActions;
    for (int step = 0; step < 4 && capture.window.session().snapshot() != original; ++step) {
        const auto previous = capture.window.session().snapshot();
        require(capture.window.session().canBack(), "Ordinary wheel history ended before the original view");
        const int triggered = clickChartContextAction(plot, "contextBackAction");
        capture.waitForSettledDisplay();
        const auto current = capture.window.session().snapshot();
        backActions.append(QJsonObject{{"step", step + 1}, {"triggeredCount", triggered},
            {"beforeSnapshot", snapshotJson(previous)}, {"afterSnapshot", snapshotJson(current)},
            {"renderStatistics", plot->renderStatistics()}, {"charts", chartRenderStatistics(capture)}});
        diagnostic["backActions"] = backActions;
        require(triggered == 1 && current != previous, "Actual Back menu action did not advance view history");
    }
    diagnostic["backActionCount"] = backActions.size();
    diagnostic["afterContextBackSnapshot"] = snapshotJson(capture.window.session().snapshot());
    diagnostic["originalViewRestored"] = capture.window.session().snapshot() == original;
    require(capture.window.session().snapshot() == original,
            "Up to four real Back actions did not restore the ordinary event-pump baseline");
    diagnostic["pass"] = true;
}

void diagnoseAuxiliaryHover(Capture& capture, QJsonObject& diagnostic) {
    auto* auxiliary = capture.plots[1];
    MouseMoveEvidence moves(auxiliary);
    const auto before = auxiliary->renderStatistics();
    const auto physicalColumns = static_cast<qint64>(std::floor(auxiliary->plotRect().width() * auxiliary->devicePixelRatioF()));
    const auto bound = 2 * physicalColumns + 2;
    diagnostic = {{"pass", false}, {"before", before}, {"hoverEvents", 2},
        {"physicalPlotColumns", physicalColumns}, {"maximumDrawnPoints", bound},
        {"delivery", "Qt injected QPA mouse event via QTest::mouseMove(QWindow*)"},
        {"nativeCursorBefore", pointJson(QCursor::pos())}};
    require(auxiliary->sourcePointCount() > auxiliary->drawnPointCount() && auxiliary->drawnPointCount() > 0 &&
            auxiliary->drawnPointCount() <= bound,
            "Auxiliary dense source must reduce to the physical pixel extrema budget");
    require(before.value("curvePathElements").toInteger() > 1,
            "Auxiliary hover diagnostic requires a real nonempty drawable path before hovering");
    const auto rect = auxiliary->plotRect();
    const auto start = QPointF(rect.left() + rect.width() * .20, rect.center().y()).toPoint();
    const auto end = QPointF(rect.left() + rect.width() * .70, rect.center().y()).toPoint();
    diagnostic["startGlobal"] = pointJson(auxiliary->mapToGlobal(start));
    diagnostic["targetGlobal"] = pointJson(auxiliary->mapToGlobal(end));
    moveThroughWindow(auxiliary, start);
    const bool startDelivered = waitUntil([&moves] { return moves.count() > 0; });
    diagnostic["mouseMoveEvents"] = moves.events();
    require(startDelivered, "Auxiliary start hover did not deliver a MouseMove");
    const auto countBeforeTarget = moves.count();
    QTest::qWait(80);
    moveThroughWindow(auxiliary, end);
    const bool targetDelivered = waitUntil([&moves, countBeforeTarget, end] {
        return moves.count() > countBeforeTarget && (moves.lastPosition() - QPointF(end)).manhattanLength() < 2;
    });
    diagnostic["mouseMoveEvents"] = moves.events();
    diagnostic["nativeCursorAfter"] = pointJson(QCursor::pos());
    require(targetDelivered, "Auxiliary target hover did not deliver the intended MouseMove");
    QTest::qWait(80);
    const auto after = auxiliary->renderStatistics();
    diagnostic["after"] = after;
    diagnostic["mouseMoveEvents"] = moves.events();
    diagnostic["nativeCursorAfter"] = pointJson(QCursor::pos());
    diagnostic["curveSourceReused"] = before.value("curveSourceGenerations") == after.value("curveSourceGenerations");
    require(after.value("curvePathElements").toInteger() > 1,
            "Auxiliary hover diagnostic requires a real nonempty drawable path after hovering");
    require(after.value("overlayUploads").toInteger() > before.value("overlayUploads").toInteger(),
            "Actual auxiliary hover events did not produce an updated overlay");
    require(before.value("curveSourceGenerations") == after.value("curveSourceGenerations"),
            "Two real auxiliary hover events must reuse the dense curve source");
    diagnostic["pass"] = true;
}

void diagnoseMainHover(Capture& capture, QPoint target, QJsonObject& diagnostic) {
    auto* plot = capture.plots[2];
    MouseMoveEvidence moves(plot);
    QJsonArray cursors;
    SampleIndex lastSample = 0;
    double lastFrequency = 0;
    QObject::connect(plot, &PlotWidget::cursorChanged, &moves, [&](SampleIndex sample, double frequency) {
        lastSample = sample; lastFrequency = frequency;
        cursors.append(QJsonObject{{"sample", QString::number(sample)}, {"frequencyHz", frequency}});
    });
    const auto start = plot->plotRect().topLeft().toPoint() + QPoint(2, 2);
    require(start != target, "Main hover must move between distinct plot coordinates");
    diagnostic = {{"pass", false}, {"delivery", "Qt injected QPA mouse event via QTest::mouseMove(QWindow*)"},
        {"nativeCursorBefore", pointJson(QCursor::pos())}, {"startGlobal", pointJson(plot->mapToGlobal(start))},
        {"targetGlobal", pointJson(plot->mapToGlobal(target))}};
    moveThroughWindow(plot, start);
    const bool startDelivered = waitUntil([&moves, &cursors] { return moves.count() > 0 && !cursors.isEmpty(); });
    diagnostic["mouseMoveEvents"] = moves.events();
    diagnostic["cursorChangedSignals"] = cursors;
    require(startDelivered,
            "Main start hover did not deliver MouseMove and cursorChanged");
    capture.waitForSettledDisplay();
    const auto before = plot->renderStatistics();
    const auto framesBeforeTarget = plot->completedFrameCount();
    const auto cursorSignalsBeforeTarget = cursors.size();
    const auto eventsBeforeTarget = moves.count();
    const auto sampleBeforeTarget = lastSample;
    const auto frequencyBeforeTarget = lastFrequency;
    diagnostic["beforeTarget"] = before;
    diagnostic["framesBeforeTarget"] = static_cast<qint64>(framesBeforeTarget);
    moveThroughWindow(plot, target);
    const bool targetDelivered = waitUntil([&] { return moves.count() > eventsBeforeTarget && cursors.size() > cursorSignalsBeforeTarget &&
                (moves.lastPosition() - QPointF(target)).manhattanLength() < 2 &&
                (lastSample != sampleBeforeTarget || lastFrequency != frequencyBeforeTarget); });
    diagnostic["mouseMoveEvents"] = moves.events();
    diagnostic["cursorChangedSignals"] = cursors;
    diagnostic["nativeCursorAfter"] = pointJson(QCursor::pos());
    require(targetDelivered,
            "Main target hover did not deliver a new mouse coordinate and cursorChanged");
    capture.waitForMainFrame(framesBeforeTarget);
    capture.waitForSettledDisplay();
    const auto after = plot->renderStatistics();
    diagnostic["afterTarget"] = after;
    diagnostic["mouseMoveEvents"] = moves.events();
    diagnostic["cursorChangedSignals"] = cursors;
    diagnostic["framesAfterTarget"] = static_cast<qint64>(plot->completedFrameCount());
    diagnostic["nativeCursorAfter"] = pointJson(QCursor::pos());
    require(after.value("overlayUploads").toInteger() > before.value("overlayUploads").toInteger(),
            "A real main cursor move did not upload an updated overlay");
    require(after.value("textureUploads") == before.value("textureUploads") &&
            after.value("powerGenerations") == before.value("powerGenerations"),
            "Mouse hover must reuse the heatmap texture and power cache");
    diagnostic["pass"] = true;
}

#include "tests/linked_cursor_capture.h"

} // namespace

int main(int argc, char* argv[]) {
    // Native acceptance must use the installed system scale. Do not set any
    // scale-factor or font-DPI override here, or select the software renderer.
    qputenv("QT_QPA_PLATFORM", "windows");
    QApplication app(argc, argv);
    QApplication::setApplicationName("SignalStudioNativeCapture");
    const auto arguments = app.arguments();
    if (arguments.size() == 2 && (arguments.at(1) == "--list-screens" || arguments.at(1) == "list")) {
        const QJsonObject inventory{{"platform", QGuiApplication::platformName()}, {"screens", screensJson()}};
        QTextStream(stdout) << QJsonDocument(inventory).toJson(QJsonDocument::Indented);
        return 0;
    }
    if (arguments.size() >= 3 && arguments.at(1) == "--linked-cursors")
        return runLinkedCursorCapture(arguments.at(2), arguments.size() > 3 ? arguments.at(3) : QString{});
    if (arguments.size() != 2 || !QFileInfo(arguments.at(1)).isAbsolute()) {
        QTextStream(stderr) << "Usage: SignalStudioUiCapture <absolute-output-directory> | --list-screens\n";
        return 2;
    }
    const QDir output(arguments.at(1));
    if (!QDir().mkpath(output.absolutePath())) {
        QTextStream(stderr) << "Could not create capture directory\n";
        return 2;
    }
    QScreen* screen = nullptr;
    for (auto* candidate : QGuiApplication::screens())
        if (isDisplayNumber(candidate, 2)) { screen = candidate; break; }
    QJsonObject report{{"platform", QGuiApplication::platformName()},
        {"screens", screensJson()}, {"requiredConnectedScreenNumber", 2},
        {"requestedLogicalWindowSize", sizeJson(QSize(2560, 1440))},
        {"requiredPixelSize", sizeJson(QSize(3840, 2160))}, {"requiredDevicePixelRatio", 1.5},
        {"qtScaleFactorEnvironment", QString::fromLocal8Bit(qgetenv("QT_SCALE_FACTOR"))},
        {"qtScreenScaleFactorsEnvironment", QString::fromLocal8Bit(qgetenv("QT_SCREEN_SCALE_FACTORS"))}};
    report["targetScreen"] = screenJson(screen);
    if (!screen) {
        report["pass"] = false;
        report["error"] = "Connected display2 is unavailable; capture did not use another screen";
        if (!writeReport(output, report)) QTextStream(stderr) << "Could not write native capture report\n";
        QTextStream(stderr) << report["error"].toString() << '\n';
        return 1;
    }
    MainWindow window;
    const auto firstFile = window.session().addDemoFile();
    window.session().addDemoFile();
    window.session().addDemoFile();
    window.session().activateFile(firstFile);
    window.refresh();
    window.resize(2560, 1440);
    Capture capture{window, output, {child<PlotWidget>(window, "navigationPlot"),
        child<PlotWidget>(window, "auxPlot"), child<PlotWidget>(window, "mainPlot")}};
    auto* main = capture.plots[2];
    QJsonObject wheelDiagnostic, ordinaryPumpDiagnostic, auxiliaryDiagnostic, mainHoverDiagnostic;
    try {
        require(QGuiApplication::platformName() == "windows", "Capture must use the native Windows platform");
        require(std::abs(screen->devicePixelRatio() - 1.5) < 0.001 &&
                screen->geometry().size() == QSize(2560, 1440) &&
                displayPixelSize(screen) == QSize(3840, 2160),
                "Connected display2 must actually be 3840x2160 at system DPR 1.5; no system settings were changed");
        showFullScreenOnScreen(window, screen);
        window.raise();
        window.activateWindow();
        require(waitUntil([&window, screen] { return window.isVisible() && window.isFullScreen() &&
                window.screen() == screen && window.size() == QSize(2560, 1440); }),
                "Full-screen client did not cover logical 2560x1440 on connected display2");
        require(window.screen() == screen && std::abs(screen->devicePixelRatio() - 1.5) < 0.001 &&
                std::abs(window.devicePixelRatioF() - 1.5) < 0.001,
                "Actual connected display2/window DPR is not 1.5; no scale override was applied");
        capture.waitForGpu();
        capture.scene("01_default_4k_150");
        diagnoseRapidWheel(capture, wheelDiagnostic);
        diagnoseOrdinaryEventPump(capture, ordinaryPumpDiagnostic);

        startCreating(main);
        const auto view = window.session().activeFile()->view;
        createMark(main, view, .25, .45, .35, .65);
        createMark(main, view, .35, .55, .25, .50);
        capture.scene("02_continuous_two_overlapping_marks", nullptr, [&] {
            require(window.session().activeFile()->marks.size() == 2 && main->isCreating(),
                    "Two real drags must create overlapping marks and retain continuous mode");
            const auto& marks = window.session().activeFile()->marks;
            require(marks[0].range.time.begin < marks[1].range.time.end &&
                    marks[1].range.time.begin < marks[0].range.time.end &&
                    marks[0].range.frequency.lowerHz < marks[1].range.frequency.upperHz &&
                    marks[1].range.frequency.lowerHz < marks[0].range.frequency.upperHz,
                    "The two dragged marks must overlap in both dimensions");
        });
        QTest::keyClick(main, Qt::Key_Escape);
        require(!main->isCreating(), "Escape must exit continuous creation");

        auto* tree = child<QTreeWidget>(window, "projectTree");
        require(waitUntil([tree] { return markItems(*tree).size() == 2; }), "Two created marks are missing in resource tree");
        const auto items = markItems(*tree);
        tree->scrollToItem(items[0]);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualItemRect(items[0]).center());
        tree->scrollToItem(items[1]);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::ControlModifier, tree->visualItemRect(items[1]).center());
        capture.scene("03_tf_tree_ctrl_selection", nullptr, [&] {
            require(window.session().activeFile()->selectedMarkIds.size() == 2,
                    "Tree Ctrl click must select both created marks");
        });

        const auto uploads = main->textureUploadCount();
        const auto generations = main->powerGenerationCount();
        diagnoseAuxiliaryHover(capture, auxiliaryDiagnostic);
        const auto& mark = window.session().activeFile()->marks[0];
        const auto hover = main->toPixel(mark.range.time.begin + (mark.range.time.end - mark.range.time.begin) / 2,
                                       (mark.range.frequency.lowerHz + mark.range.frequency.upperHz) / 2).toPoint();
        diagnoseMainHover(capture, hover, mainHoverDiagnostic);
        capture.scene("04_tf_actual_mouse_hover", nullptr, [&] {
            require(main->textureUploadCount() == uploads && main->powerGenerationCount() == generations,
                    "Mouse hover must reuse heatmap texture and power cache");
        });

        click(window, "psdMode");
        capture.scene("05_psd", nullptr, [&] {
            require(window.session().activeFile()->display.auxiliaryMode == AuxiliaryMode::Psd,
                    "Visible PSD segment did not activate PSD");
        });
        click(window, "waterfallMode");
        capture.scene("06_waterfall", nullptr, [&] {
            require(window.session().activeFile()->display.mainMode == MainMode::Waterfall,
                    "Visible waterfall segment did not activate waterfall");
        });
        auto* specToggle = child<QToolButton>(window, "specSectionToggle");
        if (!specToggle->isChecked()) click(window, "specSectionToggle");
        auto* colorbar = child<QCheckBox>(window, "colorbarToggle");
        child<QScrollArea>(window, "propScroll")->ensureWidgetVisible(colorbar);
        QTest::qWait(80);
        click(window, "colorbarToggle");
        chooseRelativeFrequency(window);
        capture.scene("07_waterfall_colorbar_baseband", nullptr, [&] {
            const auto& display = window.session().activeFile()->display;
            require(display.colorScale && !display.absoluteFrequency, "Colorbar/baseband controls did not update display");
        });

        auto frames = main->completedFrameCount();
        const auto previousHeight = main->height();
        click(window, "specMaximize");
        capture.waitForMainFrame(frames);
        capture.scene("08_spec_maximized", nullptr, [&] {
            require(main->height() > previousHeight && child<QWidget>(window, "maximizeHost")->isVisible(),
                    "Main chart did not maximize");
        });
        frames = main->completedFrameCount();
        click(window, "specMaximize");
        capture.waitForMainFrame(frames);
        capture.scene("09_spec_restored", nullptr, [&] {
            require(!child<QWidget>(window, "maximizeHost")->isVisible() && std::abs(main->height() - previousHeight) <= 2,
                    "Main chart did not restore its original layout");
        });

        click(window, "resourceToggle");
        click(window, "propClose");
        capture.scene("10_sidebars_collapsed", nullptr, [&] {
            require(child<QWidget>(window, "resources")->width() == 38 &&
                    child<QWidget>(window, "properties")->width() == 38,
                    "Sidebar buttons did not produce both rails");
        });
        click(window, "resourceToggle");
        click(window, "propRailOpen");
        QTimer newProjectConfirmation;
        QObject::connect(&newProjectConfirmation, &QTimer::timeout, [&newProjectConfirmation] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                if (auto* yes = dialog->button(QMessageBox::Yes)) {
                    newProjectConfirmation.stop();
                    QTest::mouseClick(yes, Qt::LeftButton);
                }
            }
        });
        newProjectConfirmation.start(10);
        QTest::keyClick(&window, Qt::Key_N, Qt::ControlModifier);
        newProjectConfirmation.stop();
        require(waitUntil([&window] { return window.session().project().files.empty(); }), "New-project shortcut did not clear the project");
        capture.scene("11_empty_project", nullptr, [&] {
            require(child<QWidget>(window, "emptyWorkspace")->isVisible(), "Empty workspace overlay is missing");
        });
        click(window, "projectAddSignal");
        QPointer<QDialog> dialog = window.findChild<QDialog*>("addFileDialog");
        require(dialog && waitUntil([&dialog] { return dialog && dialog->isVisible(); }), "Add-file dialog did not open");
        require(dialog->windowHandle() != nullptr, "Modal dialog has no native handle");
        dialog->windowHandle()->setScreen(screen);
        dialog->move(screen->geometry().center() - dialog->rect().center());
        capture.scene("12_add_signal_dialog", dialog.data(), [&] {
            require(dialog->screen() == screen && std::abs(dialog->devicePixelRatioF() - 1.5) < 0.001 &&
                    screen->geometry().contains(dialog->frameGeometry()),
                    "Modal add-file dialog must remain centered inside connected display2 at DPR 1.5");
        });
        click(*dialog, "cancelOpen");
        require(window.session().project().files.empty(), "Cancelled dialog must not add or read an IQ file");
    } catch (const std::exception& error) {
        capture.passed = false;
        report["error"] = QString::fromUtf8(error.what());
        QTextStream(stderr) << error.what() << '\n';
    }
    report["devicePixelRatio"] = window.devicePixelRatioF();
    report["windowScreen"] = screenJson(window.screen());
    report["windowFullScreen"] = window.isFullScreen();
    report["rapidWheelDiagnostic"] = wheelDiagnostic;
    report["ordinaryEventPumpDiagnostic"] = ordinaryPumpDiagnostic;
    report["auxiliaryHoverDiagnostic"] = auxiliaryDiagnostic;
    report["mainHoverDiagnostic"] = mainHoverDiagnostic;
    report["logicalWindowSize"] = sizeJson(window.size());
    if (!capture.scenes.isEmpty())
        report["capturedPixelSize"] = capture.scenes.at(0).toObject().value("capturedPixelSize");
    report["scenes"] = capture.scenes;
    report["pass"] = capture.passed && capture.scenes.size() == 12 &&
        wheelDiagnostic.value("pass").toBool() && ordinaryPumpDiagnostic.value("pass").toBool() &&
        auxiliaryDiagnostic.value("pass").toBool() && mainHoverDiagnostic.value("pass").toBool();
    if (!writeReport(output, report)) {
        QTextStream(stderr) << "Could not write native capture report\n";
        return 1;
    }
    return report["pass"].toBool() ? 0 : 1;
}
