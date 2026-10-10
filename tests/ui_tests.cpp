#include "app/main_window.h"
#include "ui/brand/brand_assets.h"
#include <QClipboard>
#include <QTableWidget>
#include "ui/charts/plot_widget.h"
#include "ui/charts/accelerated_surface.h"
#include "ui/charts/cursor_overlay.h"
#include "ui/display_target.h"
#include "ui/spectral_settings_widget.h"
#include "ui/import/signal_import_dialog.h"
#include "ui/controls/adaptive_value_edit.h"
#include "ui/import/source_preview_widget.h"
#include "tests/fixtures/raw_fixture.h"
#include <QSpinBox>
#include "ui/narrowband_workspace.h"

#include <QAction>
#include <QApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QWheelEvent>
#include <QWindow>
#include <QtEndian>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

using namespace signalstudio;

namespace {

void showWindow(MainWindow& window) {
    // 3840x2160 physical pixels at 150% scaling use this logical client size.
    window.resize(2560, 1440);
    QScreen* targetScreen = nullptr;
    if (QGuiApplication::platformName() == "windows") {
        for (auto* screen : QGuiApplication::screens()) {
            if (isDisplayNumber(screen, 2)) targetScreen = screen;
        }
        if (!targetScreen || !isDisplayNumber(targetScreen, 2) || displayPixelSize(targetScreen) != QSize(3840, 2160) ||
            targetScreen->geometry().size() != QSize(2560, 1440) ||
            std::abs(targetScreen->devicePixelRatio() - 1.5) > .001) {
            qFatal("Windows UI validation requires connected monitor 2 at 3840x2160 pixels and 150%% DPI.");
        }
        showFullScreenOnScreen(window, targetScreen);
        if (!QTest::qWaitForWindowExposed(&window, 5000)) qFatal("Monitor 2 full-screen window was not exposed.");
        QTest::qWait(250);
    } else {
        window.show();
    }
    QCoreApplication::processEvents();
    QTest::qWait(20);
    if (targetScreen && (window.screen() != targetScreen || !window.isFullScreen() ||
        window.size() != QSize(2560, 1440) || std::abs(window.devicePixelRatioF() - 1.5) > .001)) {
        qFatal("Windows UI window did not enter connected monitor 2 at native 4K / 150%% full screen.");
    }
}

QList<QTreeWidgetItem*> descendantsOfType(QTreeWidgetItem* parent, const QString& type) {
    QList<QTreeWidgetItem*> result;
    if (!parent) return result;
    for (int i = 0; i < parent->childCount(); ++i) {
        auto* item = parent->child(i);
        if (item->data(0, Qt::UserRole).toString() == type) result.push_back(item);
        result.append(descendantsOfType(item, type));
    }
    return result;
}

void triggerConfirmed(QAction* action) {
    QTimer responder;
    QObject::connect(&responder, &QTimer::timeout, [] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            if (auto* yes=dialog->button(QMessageBox::Yes)) yes->click();
            else dialog->done(QMessageBox::Yes);
        }
    });
    responder.start(5);
    action->trigger();
    responder.stop();
}

void wheelAt(QWidget* plot, QPointF point, int delta = 120) {
    QWheelEvent event(point, plot->mapToGlobal(point.toPoint()), QPoint(), QPoint(0, delta),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(plot, &event);
}

QStringList comboLabels(QComboBox* combo) {
    QStringList labels;
    if (combo) for (int i = 0; i < combo->count(); ++i) labels << combo->itemText(i);
    return labels;
}

SampleIndex sampleAt(const ViewRange& view, long double fraction) {
    return view.time.begin + static_cast<SampleIndex>((view.time.end - view.time.begin) * fraction);
}

double frequencyAt(const ViewRange& view, double fraction) {
    return view.frequency.lowerHz + (view.frequency.upperHz - view.frequency.lowerHz) * fraction;
}

QPoint pointAt(PlotWidget* plot, const ViewRange& view, double time, double frequency) {
    return plot->toPixel(sampleAt(view, time), frequencyAt(view, frequency)).toPoint();
}

void drag(QWidget* plot, QPoint start, QPoint end) {
    QTest::mousePress(plot, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(plot, end, 10);
    QTest::mouseRelease(plot, Qt::LeftButton, Qt::NoModifier, end);
    QCoreApplication::processEvents();
}

QImage chartImage(QWidget* chart) {
    if (auto* surface = chart->findChild<AcceleratedSurface*>(); surface && surface->isReady())
        return surface->grabFramebuffer();
    return chart->grab().toImage();
}

bool traceCoversPlot(const QImage& image, QSize logicalSize, QRectF plot) {
    if (image.isNull()) return false;
    const double sx = double(image.width()) / logicalSize.width();
    const double sy = double(image.height()) / logicalSize.height();
    // Data must span every quarter of the physical plot, including its left
    // half. A nonzero draw-call count alone cannot detect an NDC mapping error.
    for (int quarter = 0; quarter < 4; ++quarter) {
        int count = 0;
        const int first = qRound((plot.left() + plot.width() * quarter / 4.0 + 2) * sx);
        const int last = qRound((plot.left() + plot.width() * (quarter + 1) / 4.0 - 2) * sx);
        for (int y = qRound(plot.top() * sy); y <= qRound(plot.bottom() * sy); ++y)
            for (int x = first; x < last; ++x) {
                const auto color = image.pixelColor(x, y);
                if (color.green() > 140 && color.blue() > 140 && color.red() < 140) ++count;
            }
        if (count < 10) return false;
    }
    return true;
}

int changedPlotPixels(const QImage& a, const QImage& b, QSize logicalSize, QRectF plot) {
    if (a.isNull() || a.size() != b.size()) return 0;
    const double sx = double(a.width()) / logicalSize.width();
    const double sy = double(a.height()) / logicalSize.height();
    int changed = 0;
    // Exclude labels and status text. Check the left half, where the broken
    // overlay vertex binding used to leave all grid lines invisible.
    for (int y = qRound((plot.top() + 2) * sy); y < qRound((plot.bottom() - 2) * sy); ++y)
        for (int x = qRound((plot.left() + 2) * sx); x < qRound(plot.center().x() * sx); ++x)
            if (a.pixel(x, y) != b.pixel(x, y)) ++changed;
    return changed;
}

int tracePixelsOutsidePlot(const QImage& image, QSize logicalSize, QRectF plot, QColor traceColor) {
    if (image.isNull()) return -1;
    int count = 0;
    const QRectF allowed = plot.adjusted(-2, -2, 2, 2);
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        if (allowed.contains(QPointF(double(x) * logicalSize.width() / image.width(),
                                    double(y) * logicalSize.height() / image.height()))) continue;
        const auto color = image.pixelColor(x, y);
        if (std::abs(color.red() - traceColor.red()) < 8 &&
            std::abs(color.green() - traceColor.green()) < 8 &&
            std::abs(color.blue() - traceColor.blue()) < 8) ++count;
    }
    return count;
}

void clearDemoMarks(MainWindow& window) {
    auto* file = window.session().activeFile();
    file->marks.clear();
    file->channels.clear();
    window.session().selectMarks({});
    window.refresh();
}

ViewRange innerRange(const ViewRange& view) {
    return {{sampleAt(view, .25L), sampleAt(view, .45L)},
            {frequencyAt(view, .35), frequencyAt(view, .65)}};
}

} // namespace

class DemoMainWindow final : public MainWindow {
public:
    DemoMainWindow() {
        const auto first = session().addDemoFile();
        session().addDemoFile();
        session().addDemoFile();
        session().activateFile(first);
        refresh();
    }
};
class UiTests : public QObject {
    Q_OBJECT
private slots:
    void workspaceLayout();
    void globalDisplaySettingsApplyAcrossFiles();
    void mainModeCoordinateDirection();
    void continuousMarkCreation();
    void escapeCancelsCreation();
    void selectedMarkMovePreservesSpan();
    void escapeRollsBackMarkMove();
    void singleClickSelectsWithoutChangingView();
    void boxZoomAndHistoryActions();
    void emptyProjectAndFileActions();
    void treeSelectionPreservesNodesAndShiftAnchor();
    void displayChangeSurvivesPendingWheelCommit();
    void largeSampleCoordinatePrecision_data();
    void largeSampleCoordinatePrecision();
    void auxiliaryYAxisIsolationAndEscape_data();
    void auxiliaryYAxisIsolationAndEscape();
    void selectedMarkCornerAndEdgeResize_data();
    void selectedMarkCornerAndEdgeResize();
    void responsiveWorkspace_data();
    void responsiveWorkspace();
    void propertyFieldsFitSidebar();
    void prototypeDisplayDefaultsAndOptions();
    void paletteControlsStaySynchronized();
    void narrowbandPaletteSelectionUpdatesStftCharts();
    void widebandAuxiliaryRenderingAndGestures();
    void narrowbandAuxiliaryRenderingAndGestures();
    void widebandLinkedCursorsAndFrameSpectrum();
    void narrowbandLinkedCursorsAndFrameSpectrum();
    void powerInputsCommitAndFitReusesData();
    void parameterDraftsRequireExplicitCommit();
    void parameterInputsIgnoreWheel();
    void pinnedAxisLabelsSurviveLeave();
    void sharedInteractionFeedback();
    void customWindowControls();
    void initialFileViewShowsFirstFivePercentOrTenMilliseconds();
    void panelRailsAndBottomTabs();
    void sectionContextAndManualExpansion();
    void panelMaximizeAndRestore();
    void mainAxisWheelIsolationAndHistory_data();
    void mainAxisWheelIsolationAndHistory();
    void navigationClickPreservesSpanAndCancelsDrag();
    void independentSpectralSettingsAndCustomRows();
    void loadedPrefixStopAndRoundtrip();
    void fileSwitchCancelsUnfinishedEdit();
    void allResizeHandlesStayBounded_data();
    void allResizeHandlesStayBounded();
    void nativeProjectRoundtripAndInvalidImport();
    void contextMenuActionsAndCoveredMarkSelection();
    void unselectedMarkClickAndSubthresholdGesture();
    void clippedMarkBorderMovesWithoutInventingHandle();
    void captureLossAndDeactivationRollback();
    void resetRestoresCompleteViewSnapshot();
    void displayOnlyChangesPreserveBusinessViewAndPowerCache();
    void renameUsesPlainTextAndEightyCharacterLimit();
    void splitterKeyboardResetAndEscapeRollback();
    void treeToPlotShiftSelectionPreservesAnchor();
    void changingPaletteCancelsAuxiliaryGestureWithoutOverwritingY();
    void creationModeSurvivesMaximize();
    void escapeCancelsGestureBeforeLeavingMaximize();
    void addIqFileDialogCancelsAndImportsRealInt16Iq();
    void importFormatAndAdaptiveDraft();
    void importFloatingUnitsAndFailureRecovery();
    void importTemplatesBatchAndSigmf();
    void brandResourcesAndAbout();
    void importRealFilesAndCancelPrefix();
    void narrowbandDemoResourceAndFourPages();
    void waveformBandwidthAndVisiblePaneStftSettings();
    void uiStatePersistenceAndRecentProjects();
};

void UiTests::workspaceLayout() {
    DemoMainWindow window;
    showWindow(window);
    auto* navigation = window.findChild<PlotWidget*>("navigationPlot");
    auto* auxiliary = window.findChild<PlotWidget*>("auxPlot");
    auto* main = window.findChild<PlotWidget*>("mainPlot");
    auto* tree = window.findChild<QTreeWidget*>("projectTree");
    auto* resultsToggle = window.findChild<QToolButton*>("resultsToggle");
    QVERIFY(navigation);
    QVERIFY(auxiliary);
    QVERIFY(main);
    QVERIFY(tree);
    QVERIFY(resultsToggle);
    QVERIFY(navigation->isVisible());
    QVERIFY(auxiliary->isVisible());
    QVERIFY(main->isVisible());
    QVERIFY2(main->plotRect().height() >= 875, "4K at 150% scaling must retain the prototype main plot budget (about 888 logical px).");
    QTRY_VERIFY_WITH_TIMEOUT(main->isDisplaySettled(),5000);
    const auto waveform=auxiliary->grab().toImage();
    QVERIFY(!waveform.isNull());
    const auto waveformPlot=auxiliary->plotRect().adjusted(2,2,-2,-2);
    const auto imageDpr=waveform.devicePixelRatio();
    const auto imagePlot=QRectF(waveformPlot.left()*imageDpr,waveformPlot.top()*imageDpr,
        waveformPlot.width()*imageDpr,waveformPlot.height()*imageDpr).toAlignedRect().intersected(waveform.rect());
    int greenPixels=0;
    for(int y=imagePlot.top();y<=imagePlot.bottom();++y) {
        for(int x=imagePlot.left();x<=imagePlot.right();++x) {
            const auto pixel=waveform.pixel(x,y);
            if(qGreen(pixel)>qRed(pixel)+40&&qGreen(pixel)>qBlue(pixel)+10&&qGreen(pixel)>130) ++greenPixels;
        }
    }
    QVERIFY2(greenPixels>=100,"The default auxiliary plot must contain visible waveform strokes, beyond axes and grid.");
    QCOMPARE(tree->selectionMode(), QAbstractItemView::ExtendedSelection);
    QVERIFY(!resultsToggle->isChecked());
    auto* results = window.findChild<QPlainTextEdit*>();
    QVERIFY(results);
    QVERIFY(!results->isVisible());
    QCOMPARE(window.session().project().files.size(), std::size_t{3});
    QVERIFY(window.session().project().activeFileId == window.session().project().files.front().metadata.id);
    QVERIFY(window.session().activeFile()->marks.empty());
    QVERIFY(window.findChildren<QToolBar*>().empty());
}

void UiTests::globalDisplaySettingsApplyAcrossFiles() {
    DemoMainWindow window;
    showWindow(window);
    auto* mainMode = window.findChild<QComboBox*>("modeMain");
    auto* auxiliaryMode = window.findChild<QComboBox*>("modeAux");
    auto* palette = window.findChild<QComboBox*>("palette");
    auto* dynamic = window.findChild<QComboBox*>("dynamic");
    auto* reference = window.findChild<QComboBox*>("reference");
    QVERIFY(mainMode);
    QVERIFY(auxiliaryMode);
    QVERIFY(palette);
    QVERIFY(dynamic && reference);
    const auto firstId = window.session().project().files[0].metadata.id;
    const auto secondId = window.session().project().files[1].metadata.id;
    mainMode->setCurrentIndex(1);
    auxiliaryMode->setCurrentIndex(1);
    palette->setCurrentIndex(2);
    dynamic->setCurrentIndex(dynamic->findText("60 dB"));
    reference->setCurrentIndex(reference->findText("-40 dBFS/Hz"));
    QCOMPARE(static_cast<int>(window.session().activeFile()->display.mainMode), static_cast<int>(MainMode::Waterfall));
    QCOMPARE(static_cast<int>(window.session().activeFile()->display.auxiliaryMode), static_cast<int>(AuxiliaryMode::Psd));
    QCOMPARE(static_cast<int>(window.session().activeFile()->display.palette), static_cast<int>(Palette::Gray));
    QCOMPARE(window.session().activeFile()->display.dynamicRangeDb, 60.0);
    QCOMPARE(window.session().activeFile()->display.referenceLevelDb, -40.0);
    QCOMPARE(window.session().project().files[1].display.palette, Palette::Gray);
    QCOMPARE(window.session().project().files[1].display.dynamicRangeDb, 60.0);
    QCOMPARE(window.session().project().files[1].display.referenceLevelDb, -40.0);
    auto* auxiliaryPlot = window.findChild<PlotWidget*>("auxPlot");
    QVERIFY(auxiliaryPlot);
    const QPoint yAxisPoint(qRound(auxiliaryPlot->plotRect().left() - 20), qRound(auxiliaryPlot->plotRect().center().y()));
    QWheelEvent yAxisWheel(yAxisPoint, auxiliaryPlot->mapToGlobal(yAxisPoint), QPoint(), QPoint(0, 120),
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(auxiliaryPlot, &yAxisWheel);
    QCOMPARE(window.session().project().files[1].display.psdMin, window.session().activeFile()->display.psdMin);
    QCOMPARE(window.session().project().files[1].display.psdMax, window.session().activeFile()->display.psdMax);

    QVERIFY(window.session().activateFile(secondId));
    window.refresh();
    mainMode->setCurrentIndex(0);
    auxiliaryMode->setCurrentIndex(0);
    palette->setCurrentIndex(0);
    QVERIFY(window.session().activateFile(firstId));
    window.refresh();
    QCOMPARE(mainMode->currentIndex(), 0);
    QCOMPARE(auxiliaryMode->currentIndex(), 0);
    QCOMPARE(palette->currentIndex(), 0);
    QCOMPARE(dynamic->currentText(), QString("60 dB"));
    QCOMPARE(reference->currentText(), QString("-40 dBFS/Hz"));
    QVERIFY(window.session().activateFile(secondId));
    window.refresh();
    QCOMPARE(mainMode->currentIndex(), 0);
    QCOMPARE(auxiliaryMode->currentIndex(), 0);
    QCOMPARE(palette->currentIndex(), 0);
}

void UiTests::mainModeCoordinateDirection() {
    DemoMainWindow window;
    showWindow(window);
    auto* plot = window.findChild<PlotWidget*>("mainPlot");
    auto* mode = window.findChild<QComboBox*>("modeMain");
    QVERIFY(plot);
    QVERIFY(mode);
    const auto view = window.session().activeFile()->view;
    mode->setCurrentIndex(0);
    const auto tfEarlyLow = pointAt(plot, view, .2, .2);
    const auto tfLateHigh = pointAt(plot, view, .8, .8);
    QVERIFY(tfLateHigh.x() > tfEarlyLow.x());
    QVERIFY(tfLateHigh.y() < tfEarlyLow.y());
    mode->setCurrentIndex(1);
    const auto wfEarlyLow = pointAt(plot, view, .2, .2);
    const auto wfLateHigh = pointAt(plot, view, .8, .8);
    QVERIFY(wfLateHigh.x() > wfEarlyLow.x());
    QVERIFY(wfLateHigh.y() > wfEarlyLow.y());
    QVERIFY(window.session().activeFile()->view == view);
}

void UiTests::continuousMarkCreation() {
    DemoMainWindow window;
    showWindow(window);
    clearDemoMarks(window);
    auto* plot = window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    const auto view = window.session().activeFile()->view;
    plot->setCreating(true);
    drag(plot, pointAt(plot, view, .15, .3), pointAt(plot, view, .3, .5));
    QCOMPARE(window.session().activeFile()->marks.size(), std::size_t{1});
    QVERIFY(plot->isCreating());
    drag(plot, pointAt(plot, view, .6, .55), pointAt(plot, view, .75, .8));
    QCOMPARE(window.session().activeFile()->marks.size(), std::size_t{2});
    QVERIFY(plot->isCreating());
    QVERIFY(window.session().activeFile()->view == view);
}

void UiTests::escapeCancelsCreation() {
    DemoMainWindow window;
    showWindow(window);
    clearDemoMarks(window);
    auto* plot = window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    const auto view = window.session().activeFile()->view;
    const auto start = pointAt(plot, view, .15, .3);
    const auto end = pointAt(plot, view, .3, .5);
    plot->setCreating(true);
    plot->setFocus();
    QTest::mousePress(plot, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(plot, end, 10);
    QTest::keyClick(plot, Qt::Key_Escape);
    QTest::mouseRelease(plot, Qt::LeftButton, Qt::NoModifier, end);
    QVERIFY(window.session().activeFile()->marks.empty());
    QVERIFY(!plot->isCreating());
    QVERIFY(window.session().activeFile()->view == view);
}

void UiTests::selectedMarkMovePreservesSpan() {
    DemoMainWindow window;
    showWindow(window);
    clearDemoMarks(window);
    auto* plot = window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    const auto view = window.session().activeFile()->view;
    const auto range = innerRange(view);
    const auto id = window.session().addMark(range);
    QVERIFY(!id.empty());
    window.refresh();
    const auto start = pointAt(plot, view, .35, .5);
    drag(plot, start, start + QPoint(36, 18));
    const auto* mark = findMark(*window.session().activeFile(), id);
    QVERIFY(mark);
    QVERIFY(!(mark->range == range));
    QCOMPARE(mark->range.time.end - mark->range.time.begin, range.time.end - range.time.begin);
    QVERIFY(std::abs((mark->range.frequency.upperHz - mark->range.frequency.lowerHz) -
                     (range.frequency.upperHz - range.frequency.lowerHz)) < .001);
    QVERIFY(window.session().activeFile()->view == view);
}

void UiTests::escapeRollsBackMarkMove() {
    DemoMainWindow window;
    showWindow(window);
    clearDemoMarks(window);
    auto* plot = window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    const auto view = window.session().activeFile()->view;
    const auto range = innerRange(view);
    const auto id = window.session().addMark(range);
    QVERIFY(!id.empty());
    window.refresh();
    const auto start = pointAt(plot, view, .35, .5);
    const auto end = start + QPoint(36, 18);
    QTest::mousePress(plot, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(plot, end, 10);
    QTest::keyClick(plot, Qt::Key_Escape);
    QTest::mouseRelease(plot, Qt::LeftButton, Qt::NoModifier, end);
    const auto* mark = findMark(*window.session().activeFile(), id);
    QVERIFY(mark);
    QVERIFY(mark->range == range);
    QVERIFY(window.session().activeFile()->view == view);
}

void UiTests::singleClickSelectsWithoutChangingView() {
    DemoMainWindow window;
    showWindow(window);
    clearDemoMarks(window);
    auto* plot = window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    const auto view = window.session().activeFile()->view;
    const auto id = window.session().addMark(innerRange(view));
    window.session().selectMarks({});
    window.refresh();
    QTest::mouseClick(plot, Qt::LeftButton, Qt::NoModifier, pointAt(plot, view, .35, .5));
    QVERIFY(window.session().activeFile()->activeMarkId == id);
    QCOMPARE(window.session().activeFile()->selectedMarkIds.size(), std::size_t{1});
    QVERIFY(window.session().activeFile()->view == view);
}

void UiTests::boxZoomAndHistoryActions() {
    DemoMainWindow window;
    showWindow(window);
    clearDemoMarks(window);
    auto* plot = window.findChild<PlotWidget*>("mainPlot");
    auto* back = window.findChild<QAction*>("backAction");
    auto* forward = window.findChild<QAction*>("forwardAction");
    QVERIFY(plot);
    QVERIFY(back);
    QVERIFY(forward);
    const auto original = window.session().activeFile()->view;
    drag(plot, pointAt(plot, original, .2, .2), pointAt(plot, original, .7, .8));
    const auto zoomed = window.session().activeFile()->view;
    QVERIFY(!(zoomed == original));
    QVERIFY(zoomed.time.end - zoomed.time.begin < original.time.end - original.time.begin);
    QVERIFY(back->isEnabled());
    back->trigger();
    QVERIFY(window.session().activeFile()->view == original);
    QVERIFY(forward->isEnabled());
    forward->trigger();
    QVERIFY(window.session().activeFile()->view == zoomed);
}

void UiTests::emptyProjectAndFileActions() {
    MainWindow window;
    showWindow(window);
    auto* create = window.findChild<QAction*>("newProjectAction");
    auto* add = window.findChild<QAction*>("addDemoAction");
    auto* openDemo = window.findChild<QAction*>("openDemoProjectAction");
    auto* addSignal = window.findChild<QAction*>("openIqAction");
    auto* projectAddSignal = window.findChild<QPushButton*>("projectAddSignal");
    auto* remove = window.findChild<QAction*>("removeFileAction");
    auto* mode = window.findChild<QComboBox*>("modeMain");
    auto* plot = window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(create);
    QVERIFY(add);
    QVERIFY(openDemo);
    QVERIFY(addSignal);
    QVERIFY(projectAddSignal);
    QVERIFY(remove);
    QVERIFY(mode);
    QVERIFY(plot);
    QVERIFY(!addSignal->isEnabled());
    QVERIFY(!projectAddSignal->isVisible());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QTimer completeNew;
    connect(&completeNew, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "newProjectDialog") return;
        dialog->findChild<QLineEdit*>("newProjectName")->setText("workflow-project");
        dialog->findChild<QLineEdit*>("newProjectLocation")->setText(directory.path());
        dialog->findChild<QPushButton*>("createProjectButton")->click();
    });
    completeNew.start(5);
    create->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath("workflow-project/project.json")), 2'000);
    completeNew.stop();
    QVERIFY(addSignal->isEnabled());
    QVERIFY(projectAddSignal->isVisible());
    QCOMPARE(QString::fromStdString(window.session().project().name), QStringLiteral("workflow-project"));
    QCOMPARE(window.session().project().files.size(),std::size_t{0});
    QString createError;
    QVERIFY(!window.createProject(directory.path(), "workflow-project", &createError));
    QVERIFY(!createError.isEmpty());
    QCOMPARE(QString::fromStdString(window.session().project().name), QStringLiteral("workflow-project"));
    MainWindow reopened;
    QVERIFY(reopened.openProject(directory.filePath("workflow-project")));
    QCOMPARE(QString::fromStdString(reopened.session().project().name), QStringLiteral("workflow-project"));
    QVERIFY(window.session().project().files.empty());
    QVERIFY(!window.session().activeFile());
    QVERIFY(!remove->isEnabled());
    QVERIFY(!mode->isEnabled());
    QVERIFY(plot->isVisible());
    add->trigger();
    QCOMPARE(window.session().project().files.size(), std::size_t{1});
    QVERIFY(remove->isEnabled());
    QVERIFY(mode->isEnabled());
    triggerConfirmed(remove);
    QVERIFY(window.session().project().files.empty());
    QVERIFY(!window.session().activeFile());
    triggerConfirmed(openDemo);
    QCOMPARE(window.session().project().files.size(), std::size_t{3});
    QCOMPARE(QString::fromStdString(window.session().project().name), QStringLiteral("演示工程"));
}

void UiTests::treeSelectionPreservesNodesAndShiftAnchor() {
    DemoMainWindow window;
    showWindow(window);
    auto* tree=window.findChild<QTreeWidget*>("projectTree");
    QVERIFY(tree);
    const auto view=window.session().activeFile()->view;
    clearDemoMarks(window);
    for (int i = 0; i < 4; ++i) window.session().addMark(innerRange(view));
    window.session().selectMarks({});
    window.refresh();
    QCoreApplication::processEvents();
    auto* root=tree->topLevelItem(0);
    QVERIFY(root);
    auto* fileNode=root->child(0);
    const auto markNodes=descendantsOfType(fileNode,"mark");
    QCOMPARE(markNodes.size(),4);
    const auto originalView=window.session().activeFile()->view;
    auto* first=markNodes[0];
    auto* third=markNodes[2];
    auto* fourth=markNodes[3];
    const auto firstId=first->data(0,Qt::UserRole+1).toString().toStdString();
    const auto thirdId=third->data(0,Qt::UserRole+1).toString().toStdString();
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualItemRect(first).center());
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::ControlModifier,tree->visualItemRect(third).center());
    const auto& selected=window.session().activeFile()->selectedMarkIds;
    QCOMPARE(selected.size(),std::size_t{2});
    QVERIFY(std::find(selected.begin(),selected.end(),firstId)!=selected.end());
    QVERIFY(std::find(selected.begin(),selected.end(),thirdId)!=selected.end());
    QVERIFY(tree->topLevelItem(0)==root&&root->child(0)==fileNode&&descendantsOfType(fileNode,"mark")[0]==first);
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualItemRect(first).center());
    window.refresh(); // Cursor and parameter refreshes must retain the native range anchor.
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::ShiftModifier,tree->visualItemRect(fourth).center());
    QCOMPARE(window.session().activeFile()->selectedMarkIds.size(),std::size_t{4});
    QVERIFY(window.session().activeFile()->view==originalView);
    QVERIFY(tree->topLevelItem(0)==root&&descendantsOfType(fileNode,"mark")[3]==fourth);
    const auto firstFileId=window.session().project().activeFileId;
    const auto secondFileId=window.session().project().files[1].metadata.id;
    auto* secondFileNode=root->child(1);
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualItemRect(secondFileNode).center());
    QVERIFY(window.session().project().activeFileId==firstFileId);
    QVERIFY(tree->topLevelItem(0)==root); // File activation cannot destroy the dispatching item.
    QCoreApplication::processEvents();
    QTRY_VERIFY(window.session().project().activeFileId==secondFileId);
    QTRY_COMPARE(descendantsOfType(tree->topLevelItem(0)->child(1),"mark").size(),0);
}

void UiTests::displayChangeSurvivesPendingWheelCommit() {
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    auto* mode=window.findChild<QComboBox*>("modeMain");
    auto* palette=window.findChild<QComboBox*>("palette");
    QVERIFY(plot);
    QVERIFY(mode);
    QVERIFY(palette);
    const auto original=window.session().activeFile()->view;
    const auto point=plot->plotRect().center();
    QWheelEvent wheel(point,plot->mapToGlobal(point.toPoint()),QPoint(),QPoint(0,120),
                      Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QCoreApplication::sendEvent(plot,&wheel);
    const auto zoomed=window.session().activeFile()->view;
    QVERIFY(!(zoomed==original));
    QVERIFY(!window.session().canBack()); // Still inside the uncommitted 220ms group.
    mode->setCurrentIndex(1);
    QCOMPARE(mode->currentIndex(),1);
    QCOMPARE(static_cast<int>(window.session().activeFile()->display.mainMode),static_cast<int>(MainMode::Waterfall));
    QVERIFY(window.session().activeFile()->view==zoomed);
    QVERIFY(window.session().canBack());
    QWheelEvent secondWheel(point,plot->mapToGlobal(point.toPoint()),QPoint(),QPoint(0,120),
                            Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QCoreApplication::sendEvent(plot,&secondWheel);
    palette->setCurrentIndex(2);
    QCOMPARE(palette->currentIndex(),2);
    QCOMPARE(static_cast<int>(window.session().activeFile()->display.palette),static_cast<int>(Palette::Gray));
}

void UiTests::largeSampleCoordinatePrecision_data() {
    QTest::addColumn<qulonglong>("origin");
    QTest::addColumn<bool>("waterfall");
    constexpr qulonglong largeOrigin=(qulonglong{1}<<53)+17;
    const auto nearMaximum=std::numeric_limits<qulonglong>::max()-4096;
    QTest::newRow("time-frequency-beyond-2pow53")<<largeOrigin<<false;
    QTest::newRow("waterfall-beyond-2pow53")<<largeOrigin<<true;
    QTest::newRow("time-frequency-uint64-maximum")<<nearMaximum<<false;
    QTest::newRow("waterfall-uint64-maximum")<<nearMaximum<<true;
}

void UiTests::largeSampleCoordinatePrecision() {
    QFETCH(qulonglong,origin);
    QFETCH(bool,waterfall);
    DemoMainWindow window;
    showWindow(window);
    clearDemoMarks(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    auto* file=window.session().activeFile();
    file->metadata.sampleCount=std::numeric_limits<SampleIndex>::max();
    file->view.time={static_cast<SampleIndex>(origin),static_cast<SampleIndex>(origin)+4096};
    file->display.mainMode=waterfall?MainMode::Waterfall:MainMode::TimeFrequency;
    window.refresh();
    const auto view=file->view;
    const auto frequency=frequencyAt(view,.5);
    const auto start=plot->toPixel(view.time.begin,frequency);
    const auto adjacent=plot->toPixel(view.time.begin+1,frequency);
    const auto end=plot->toPixel(view.time.end,frequency);
    const auto delta=waterfall?adjacent.y()-start.y():adjacent.x()-start.x();
    const auto extent=waterfall?plot->plotRect().height():plot->plotRect().width();
    QVERIFY2(delta>0,"Adjacent uint64 sample positions must remain distinguishable.");
    QVERIFY2(std::abs(delta-extent/4096)<1e-9,"Adjacent samples must map to exactly one sample step.");
    const auto endDelta=waterfall?end.y()-start.y():end.x()-start.x();
    QVERIFY(std::abs(endDelta-extent)<1e-9);
    QVERIFY(file->view==view);
}

void UiTests::auxiliaryYAxisIsolationAndEscape_data() {
    QTest::addColumn<bool>("psd");
    QTest::newRow("waveform-amplitude")<<false;
    QTest::newRow("psd-power")<<true;
}

void UiTests::auxiliaryYAxisIsolationAndEscape() {
    QFETCH(bool,psd);
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("auxPlot");
    QVERIFY(plot);
    auto* file=window.session().activeFile();
    file->display.auxiliaryMode=psd?AuxiliaryMode::Psd:AuxiliaryMode::Waveform;
    window.session().setAuxiliaryRange(psd?-110:-60, psd?-10:60, false);
    window.refresh();
    const auto view=file->view;
    const auto originalSpan=file->display.auxiliaryMax-file->display.auxiliaryMin;
    const QPoint point(qRound(plot->plotRect().left()-20),qRound(plot->plotRect().center().y()));
    QWheelEvent wheel(point,plot->mapToGlobal(point),QPoint(),QPoint(0,120),
                      Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QCoreApplication::sendEvent(plot,&wheel);
    const auto wheelMin=file->display.auxiliaryMin;
    const auto wheelMax=file->display.auxiliaryMax;
    QVERIFY(wheelMax-wheelMin<originalSpan);
    QVERIFY(file->view==view);
    QVERIFY(!window.session().canBack());
    drag(plot,point,point+QPoint(0,20));
    QVERIFY(std::abs(file->display.auxiliaryMin-wheelMin)>1e-6);
    QVERIFY(std::abs((file->display.auxiliaryMax-file->display.auxiliaryMin)-(wheelMax-wheelMin))<1e-8);
    QVERIFY(file->view==view);
    const auto beforeCancelMin=file->display.auxiliaryMin;
    const auto beforeCancelMax=file->display.auxiliaryMax;
    QTest::mousePress(plot,Qt::LeftButton,Qt::NoModifier,point);
    QTest::mouseMove(plot,point+QPoint(0,24),10);
    QVERIFY(std::abs(file->display.auxiliaryMin-beforeCancelMin)>1e-6);
    QTest::keyClick(plot,Qt::Key_Escape);
    QTest::mouseRelease(plot,Qt::LeftButton,Qt::NoModifier,point+QPoint(0,24));
    QCOMPARE(file->display.auxiliaryMin,beforeCancelMin);
    QCOMPARE(file->display.auxiliaryMax,beforeCancelMax);
    QVERIFY(file->view==view);
    QVERIFY(window.session().canBack());
    QVERIFY(window.session().back());
    QCOMPARE(file->display.auxiliaryMin,wheelMin);
    QCOMPARE(file->display.auxiliaryMax,wheelMax);
    QVERIFY(window.session().back());
    QCOMPARE(file->display.auxiliaryMin,psd?-110.0:-60.0);
    QCOMPARE(file->display.auxiliaryMax,psd?-10.0:60.0);
    QVERIFY(!window.session().canBack());
}

void UiTests::selectedMarkCornerAndEdgeResize_data() {
    QTest::addColumn<bool>("waterfall");
    QTest::newRow("time-frequency")<<false;
    QTest::newRow("waterfall")<<true;
}

void UiTests::selectedMarkCornerAndEdgeResize() {
    QFETCH(bool,waterfall);
    DemoMainWindow window;
    showWindow(window);
    clearDemoMarks(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    auto* file=window.session().activeFile();
    file->display.mainMode=waterfall?MainMode::Waterfall:MainMode::TimeFrequency;
    const auto view=file->view;
    const auto original=innerRange(view);
    const auto id=window.session().addMark(original);
    window.refresh();
    const auto corner=plot->toPixel(original.time.begin,
        waterfall?original.frequency.lowerHz:original.frequency.upperHz).toPoint();
    drag(plot,corner,corner+QPoint(24,16));
    auto resized=findMark(*file,id)->range;
    QVERIFY(resized.time.begin>original.time.begin);
    QCOMPARE(resized.time.end,original.time.end);
    QVERIFY(resized.time.begin<resized.time.end);
    if(waterfall) {
        QVERIFY(resized.frequency.lowerHz>original.frequency.lowerHz);
        QCOMPARE(resized.frequency.upperHz,original.frequency.upperHz);
    } else {
        QCOMPARE(resized.frequency.lowerHz,original.frequency.lowerHz);
        QVERIFY(resized.frequency.upperHz<original.frequency.upperHz);
    }
    QVERIFY(resized.frequency.lowerHz<resized.frequency.upperHz);
    const auto pixelBox=QRectF(plot->toPixel(resized.time.begin,resized.frequency.lowerHz),
        plot->toPixel(resized.time.end,resized.frequency.upperHz)).normalized();
    const auto edge=QPointF(pixelBox.right(),pixelBox.center().y()).toPoint();
    drag(plot,edge,edge+QPoint(25,0));
    const auto enlarged=findMark(*file,id)->range;
    if(waterfall) {
        QVERIFY(enlarged.time==resized.time);
        QCOMPARE(enlarged.frequency.lowerHz,resized.frequency.lowerHz);
        QVERIFY(enlarged.frequency.upperHz>resized.frequency.upperHz);
    } else {
        QCOMPARE(enlarged.time.begin,resized.time.begin);
        QVERIFY(enlarged.time.end>resized.time.end);
        QVERIFY(enlarged.frequency==resized.frequency);
    }
    const auto bounds=fullRange(file->metadata);
    QVERIFY(enlarged.time.begin<enlarged.time.end&&enlarged.time.end<=bounds.time.end);
    QVERIFY(enlarged.frequency.lowerHz>=bounds.frequency.lowerHz&&enlarged.frequency.upperHz<=bounds.frequency.upperHz);
    QVERIFY(file->view==view);
    QVERIFY(!window.session().canBack());
}

void UiTests::responsiveWorkspace_data() {
    QTest::addColumn<QSize>("windowSize");
    QTest::addColumn<int>("resourceWidth");
    QTest::addColumn<int>("propertyWidth");
    QTest::newRow("4k-150-percent") << QSize(2560,1440) << 270 << 294;
}

void UiTests::responsiveWorkspace() {
    QFETCH(QSize,windowSize);
    QFETCH(int,resourceWidth);
    QFETCH(int,propertyWidth);
    DemoMainWindow window;
    window.resize(windowSize);
    window.show();
    QCoreApplication::processEvents();
    QTest::qWait(20);
    auto* resources=window.findChild<QWidget*>("resources");
    auto* properties=window.findChild<QWidget*>("properties");
    auto* nav=window.findChild<QWidget*>("navPanel");
    auto* aux=window.findChild<QWidget*>("auxPanel");
    auto* spec=window.findChild<QWidget*>("specPanel");
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(resources&&properties&&nav&&aux&&spec&&plot);
    QVERIFY2(std::abs(resources->width()-resourceWidth)<=2,"Resource width must follow the final HTML media rules.");
    QVERIFY2(std::abs(properties->width()-propertyWidth)<=2,"Property width must follow the final HTML media rules.");
    const int navY=nav->mapTo(&window,QPoint()).y();
    const int auxY=aux->mapTo(&window,QPoint()).y();
    const int specY=spec->mapTo(&window,QPoint()).y();
    QVERIFY(navY<auxY&&auxY<specY);
    QCOMPARE(nav->height(),82);
    QCOMPARE(aux->height(),205);
    QVERIFY(plot->plotRect().height()>=875);
    QVERIFY(window.findChildren<QToolBar*>().empty());
    QVERIFY(!window.findChild<QComboBox*>("modeMain")->isVisible());
    QVERIFY(!window.findChild<QComboBox*>("modeAux")->isVisible());
}

void UiTests::propertyFieldsFitSidebar() {
    DemoMainWindow window;
    window.resize(2560, 1440);
    window.show();
    QCoreApplication::processEvents();
    auto* scroll = window.findChild<QScrollArea*>("propScroll");
    QVERIFY(scroll);
    auto* viewport = scroll->viewport();
    QVERIFY(viewport && viewport->width() > 0);
    for (const auto* id : {"effectiveBandwidthMHz", "psdFft", "stftFft", "dynamic", "reference"}) {
        auto* field = window.findChild<QWidget*>(id);
        QVERIFY2(field, id);
        const auto fieldRect = QRect(field->mapTo(viewport, QPoint()), field->size());
        QVERIFY2(fieldRect.left() >= 0 && fieldRect.right() < viewport->width(),
                 qPrintable(QString("Property field %1 must not be clipped at the right edge").arg(id)));
    }
}

void UiTests::prototypeDisplayDefaultsAndOptions() {
    DemoMainWindow window;
    showWindow(window);
    const auto* file=window.session().activeFile();
    QVERIFY(file);
    QCOMPARE(QString::fromStdString(file->metadata.name),QString("wideband_100MHz.iq"));
    QCOMPARE(file->metadata.sampleRateHz,40'000'000.0);
    QCOMPARE(file->metadata.centerFrequencyHz,100'000'000.0);
    QCOMPARE(file->metadata.sampleCount,SampleIndex{19'200'000'000ULL});
    QCOMPARE(file->view.time.begin,SampleIndex{0});
    QCOMPARE(file->view.time.end,SampleIndex{960'000'000ULL});
    QCOMPARE(file->display.palette,Palette::CoolEditClassic);
    QCOMPARE(window.findChild<QComboBox*>("palette")->currentText(),QString("CoolEdit Classic"));
    QCOMPARE(file->view.frequency.lowerHz,80'000'000.0);
    QCOMPARE(file->view.frequency.upperHz,120'000'000.0);
    QVERIFY(file->marks.empty()&&file->channels.empty());
    QVERIFY(file->display.waveformAutoFit);
    QVERIFY(std::isfinite(file->display.waveformMin));
    QVERIFY(std::isfinite(file->display.waveformMax));
    QVERIFY(file->display.waveformMax>file->display.waveformMin);
    QCOMPARE(file->display.psdMin,-100.0);
    QCOMPARE(file->display.psdMax,0.0);
    auto* stft=window.findChild<QComboBox*>("stftFft");
    auto* psd=window.findChild<QComboBox*>("psdFft");
    auto* dynamic=window.findChild<QComboBox*>("dynamic");
    auto* reference=window.findChild<QComboBox*>("reference");
    auto* scope=window.findChild<QComboBox*>("psdScope");
    auto* grid=window.findChild<QCheckBox*>("gridToggle");
    auto* colorbar=window.findChild<QCheckBox*>("colorbarToggle");
    QVERIFY(stft&&psd&&dynamic&&reference&&scope&&grid&&colorbar);
    const QStringList palettes{"Turbo","Viridis","Gray","Plasma","Inferno","Magma","Cividis","CoolEdit Classic"};
    QCOMPARE(comboLabels(window.findChild<QComboBox*>("palette")), palettes);
    QCOMPARE(comboLabels(window.findChild<QComboBox*>("colormap")), palettes);
    QStringList fftSizes;for(int order=5;order<=16;++order)fftSizes<<QString::number(1<<order);
    QCOMPARE(comboLabels(stft),fftSizes);
    QCOMPARE(comboLabels(psd),fftSizes);
    QCOMPARE(comboLabels(dynamic),QStringList({"20 dB","40 dB","60 dB","80 dB","100 dB","120 dB"}));
    QCOMPARE(comboLabels(reference),QStringList({"0 dBFS/Hz","-20 dBFS/Hz","-40 dBFS/Hz","-60 dBFS/Hz","-80 dBFS/Hz","-100 dBFS/Hz"}));
    QCOMPARE(stft->currentText(),QString("2048"));
    QCOMPARE(psd->currentText(),QString("4096"));
    QCOMPARE(dynamic->currentIndex(),3);
    QCOMPARE(reference->currentIndex(),0);
    QCOMPARE(scope->currentIndex(),0);
    QVERIFY(grid->isChecked());
    QVERIFY(!colorbar->isChecked());
    scope->setCurrentIndex(1);
    QCOMPARE(scope->currentIndex(),0);
    QVERIFY(!file->display.psdFromSelection);
}

void UiTests::powerInputsCommitAndFitReusesData() {
    MainWindow window; window.openNarrowbandDemoProject(); showWindow(window);
    auto* workspace=window.findChild<NarrowbandWorkspace*>();
    auto* dynamic=window.findChild<QComboBox*>("dynamic");
    auto* reference=window.findChild<QComboBox*>("reference");
    auto* fit=window.findChild<QPushButton*>("autoPowerFit");
    auto* outside=window.findChild<QLabel*>("statusTime");
    QVERIFY(workspace && dynamic && reference && fit && outside);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(),15000);
    QTRY_VERIFY(fit->isEnabled());
    const auto context=window.session().activeChannel()->id;
    window.session().pinCursor(context,window.session().activeChannel()->visibleSourceTime.begin+100,0,true); window.refresh();
    const QJsonValue generation=workspace->renderStatistics()["requestGeneration"];
    const auto spectrum=window.session().spectrogram(context);
    const auto previousDynamic=window.session().activeFile()->display.dynamicRangeDb;
    const auto previousCustom=dynamic->property("lastCustomValue");
    auto* editor=dynamic->lineEdit(); editor->setFocus(); editor->selectAll();
    QTest::keyClicks(editor,"123.5"); window.refresh();
    QCOMPARE(window.session().activeFile()->display.dynamicRangeDb,previousDynamic);
    QCOMPARE(dynamic->property("lastCustomValue"),previousCustom);
    QVERIFY(editor->hasFocus()); QCOMPARE(editor->text(),QString("123.5")); QCOMPARE(editor->cursorPosition(),5);
    QTest::keyClick(editor,Qt::Key_Tab);
    QCOMPARE(window.session().activeFile()->display.dynamicRangeDb,123.5);
    QCOMPARE(editor->text(),QString("123.5 dB"));
    editor->setFocus(); editor->selectAll(); QTest::keyClicks(editor,"123.5"); QTest::keyClick(editor,Qt::Key_Return);
    QCOMPARE(window.session().activeFile()->display.dynamicRangeDb,123.5);
    QCOMPARE(dynamic->property("lastCustomValue").toDouble(),123.5);
    QCOMPARE(window.session().activeChannel()->psdAxisMinimum,window.session().activeFile()->display.referenceLevelDb-123.5);
    editor=reference->lineEdit(); editor->setFocus(); editor->selectAll(); QTest::keyClicks(editor,"-");
    const auto last=window.session().activeFile()->display.referenceLevelDb;
    QCOMPARE(editor->text(),QString("-"));
    QTest::keyClicks(editor,"35.25");
    QCOMPARE(window.session().activeFile()->display.referenceLevelDb,last);
    QTest::mouseClick(outside,Qt::LeftButton);
    QCOMPARE(window.session().activeFile()->display.referenceLevelDb,-35.25);
    QCOMPARE(window.session().activeChannel()->psdAxisMinimum,-158.75);
    QCOMPARE(window.session().activeChannel()->psdAxisMaximum,-35.25);
    QCOMPARE(window.session().spectrogram(context),spectrum);
    QCOMPARE(workspace->renderStatistics()["requestGeneration"],generation);
    QVERIFY(window.session().linkedCursor(context).pinned);
    QVERIFY(window.session().setChannelPsdRange(-350,-250,false)); window.refresh();
    editor->setFocus(); editor->selectAll(); QTest::keyClicks(editor,"-36");
    QCOMPARE(window.session().activeChannel()->psdAxisMaximum,-250.0);
    QTest::keyClick(editor,Qt::Key_Enter);
    QCOMPARE(window.session().activeChannel()->psdAxisMaximum,-36.0);
    auto snapshot=workspace->powerSnapshot(); QVERIFY(snapshot.ready);
    const auto expected=fitPowerDisplayRange(*snapshot.heatmap,snapshot.psd.get()); QVERIFY(expected.valid);
    QTest::mouseClick(fit,Qt::LeftButton);
    QCOMPARE(window.session().activeFile()->display.referenceLevelDb,expected.range.referenceLevelDb);
    QCOMPARE(window.session().activeChannel()->psdAxisMinimum,expected.range.lowerDb());
    QCOMPARE(window.session().spectrogram(context),spectrum);
    QCOMPARE(workspace->renderStatistics()["requestGeneration"],generation);
    editor->setFocus(); editor->selectAll(); QTest::keyClicks(editor,"101");
    QCOMPARE(window.session().activeFile()->display.referenceLevelDb,expected.range.referenceLevelDb);
    QTest::mouseClick(outside,Qt::LeftButton);
    QCOMPARE(window.session().activeFile()->display.referenceLevelDb,expected.range.referenceLevelDb);
    QVERIFY(reference->currentText().endsWith("dBFS/Hz"));
}

void UiTests::parameterDraftsRequireExplicitCommit() {
    MainWindow window; window.openNarrowbandDemoProject(); showWindow(window);
    auto* outside=window.findChild<QLabel*>("statusTime");
    auto* overlap=window.findChild<QComboBox*>("psdOverlap");
    auto* duration=window.findChild<QComboBox*>("psdSegmentMs");
    auto* parameters=overlap->parentWidget();
    auto* settings=qobject_cast<SpectralSettingsWidget*>(parameters); QVERIFY(settings);
    auto* channel=window.session().activeChannel();
    const double oldOverlap=channel->psd.parameters.overlap;
    const auto custom=overlap->property("lastCustomValue");
    overlap->lineEdit()->setFocus(); overlap->lineEdit()->selectAll(); QTest::keyClicks(overlap->lineEdit(),"62.5");
    QCOMPARE(settings->parameters().overlap,oldOverlap); QCOMPARE(channel->psd.parameters.overlap,oldOverlap);
    QCOMPARE(overlap->property("lastCustomValue"),custom);
    QTest::keyClick(overlap->lineEdit(),Qt::Key_Tab); window.refresh();
    QCOMPARE(overlap->currentText(),QString("62.5")); QCOMPARE(channel->psd.parameters.overlap,.625);
    overlap->lineEdit()->setFocus();overlap->lineEdit()->selectAll();QTest::keyClicks(overlap->lineEdit(),"63.75");
    // An unrelated programmatic change must use committed values, not draft text.
    window.findChild<QCheckBox*>("psdRemoveMean")->setChecked(true);
    QCOMPARE(channel->psd.parameters.overlap,.625);
    QTest::mouseClick(outside,Qt::LeftButton);
    QCOMPARE(channel->psd.parameters.overlap,.6375); QCOMPARE(overlap->property("lastCustomValue").toDouble(),63.75);
    duration->lineEdit()->setFocus(); duration->lineEdit()->selectAll(); QTest::keyClicks(duration->lineEdit(),"-");
    QTest::keyClick(duration->lineEdit(),Qt::Key_Return);
    QCOMPARE(channel->psd.parameters.segmentMilliseconds,0.0); QCOMPARE(duration->currentText(),QString("0"));
    window.findChild<QComboBox*>("psdMethod")->setCurrentIndex(static_cast<int>(SpectralMethod::Multitaper));
    // Imported parameters can have more precision than the editor displays.
    // A refresh must preserve the draft even when those numeric values differ.
    channel->psd.parameters.timeBandwidth=3.567;window.refresh();
    auto* nw=window.findChild<QDoubleSpinBox*>("psdTimeBandwidth");
    auto* edit=nw->findChild<QLineEdit*>(); const double previousNw=nw->value();
    const double previousParameterNw=channel->psd.parameters.timeBandwidth;
    edit->setFocus(); edit->selectAll(); QTest::keyClicks(edit,"4.5");
    QCOMPARE(nw->value(),previousNw); QCOMPARE(channel->psd.parameters.timeBandwidth,previousParameterNw);
    window.refresh();QCOMPARE(edit->text(),QString("4.5"));
    QTest::keyClick(edit,Qt::Key_Tab); window.refresh();
    QCOMPARE(nw->value(),4.5); QCOMPARE(edit->text(),QString("4.50"));
    QCOMPARE(settings->parameters().timeBandwidth,4.5);
    edit->setFocus();edit->selectAll();QTest::keyClicks(edit,"5.25");
    QFocusEvent deactivated(QEvent::FocusOut,Qt::ActiveWindowFocusReason);QCoreApplication::sendEvent(nw,&deactivated);window.refresh();
    QCOMPARE(nw->value(),4.5);QCOMPARE(edit->text(),QString("5.25"));
    QTest::keyClick(edit,Qt::Key_Return);
    QCOMPARE(channel->psd.parameters.timeBandwidth,5.25);
    auto* count=window.findChild<QSpinBox*>("psdTapers"); edit=count->findChild<QLineEdit*>();
    const int previousCount=count->value(); edit->setFocus(); edit->selectAll(); QTest::keyClicks(edit,"5");
    QCOMPARE(count->value(),previousCount); window.refresh(); QCOMPARE(edit->text(),QString("5"));
    QTest::keyClick(edit,Qt::Key_Backtab); QCOMPARE(channel->psd.parameters.tapers,5);
    edit->setFocus(); QTest::keyClick(edit,Qt::Key_Up);
    QCOMPARE(channel->psd.parameters.tapers,5); QTest::keyClick(edit,Qt::Key_Return);
    QCOMPARE(channel->psd.parameters.tapers,6);
    // Context switching cancels drafts instead of applying them to another file.
    auto* dynamic=window.findChild<QComboBox*>("dynamic");
    dynamic->lineEdit()->setFocus(); dynamic->lineEdit()->selectAll(); QTest::keyClicks(dynamic->lineEdit(),"77.25");
    const double oldDynamic=window.session().activeFile()->display.dynamicRangeDb;
    window.session().project().narrowbandWorkspaceOpen=false; window.refresh();
    QCOMPARE(window.session().activeFile()->display.dynamicRangeDb,oldDynamic);
    QVERIFY(!dynamic->property("parameterDraft").toBool());
    QVERIFY(dynamic->currentText()!=QString("77.25"));
    auto* bandwidth=window.findChild<QDoubleSpinBox*>("effectiveBandwidthMHz");edit=bandwidth->findChild<QLineEdit*>();
    const double originalBandwidth=window.session().activeFile()->metadata.effectiveBandwidthHz;
    const double nextBandwidth=bandwidth->value()*.8;
    edit->setFocus();edit->selectAll();QTest::keyClicks(edit,QString::number(nextBandwidth,'f',6).toLatin1().constData());
    window.refresh();QCOMPARE(window.session().activeFile()->metadata.effectiveBandwidthHz,originalBandwidth);
    QVERIFY(edit->text().startsWith(QString::number(nextBandwidth,'f',6)));
    QTest::keyClick(edit,Qt::Key_Tab);
    QVERIFY(std::abs(window.session().activeFile()->metadata.effectiveBandwidthHz-nextBandwidth*1e6)<.5);
    // Dynamically opened dialogs get the same numeric policy; Return commits
    // the editor, and does not accidentally submit the dialog's default button.
    window.findChild<QPushButton*>("extract")->click();
    auto* dialog=window.findChild<QDialog*>("channelConfigDialog"); QVERIFY(dialog);
    auto* center=dialog->findChild<QDoubleSpinBox*>("channelCenterMHz");
    const double oldCenter=center->value(); edit=center->findChild<QLineEdit*>();
    edit->setFocus(); edit->selectAll(); QTest::keyClicks(edit,QString::number(oldCenter+.01,'f',6).toLatin1().constData());
    QCOMPARE(center->value(),oldCenter);
    QTest::keyClick(edit,Qt::Key_Return);
    QVERIFY(std::abs(center->value()-(oldCenter+.01))<1e-6); QVERIFY(dialog->isVisible()); dialog->reject();
    window.session().project().narrowbandWorkspaceOpen=true;window.refresh();
    auto* workspace=window.findChild<NarrowbandWorkspace*>();workspace->activatePageForAcceptance(1);
    auto* rate=window.findChild<QDoubleSpinBox*>("symbolRate");edit=rate->findChild<QLineEdit*>();
    const double previousRate=channel->symbolRate;edit->setFocus();edit->selectAll();QTest::keyClicks(edit,"275000");
    QCOMPARE(channel->symbolRate,previousRate);QTest::keyClick(edit,Qt::Key_Tab);QCOMPARE(channel->symbolRate,275000.0);
    workspace->activatePageForAcceptance(2);
    auto* threshold=window.findChild<QDoubleSpinBox*>("recognitionThreshold");edit=threshold->findChild<QLineEdit*>();
    QSignalSpy edits(threshold,&QDoubleSpinBox::valueChanged);const double previousThreshold=threshold->value();
    edit->setFocus();edit->selectAll();QTest::keyClicks(edit,"0.75");QCOMPARE(threshold->value(),previousThreshold);QCOMPARE(edits.count(),0);
    QTest::keyClick(edit,Qt::Key_Tab);QCOMPARE(threshold->value(),.75);QCOMPARE(edits.count(),1);
}

void UiTests::parameterInputsIgnoreWheel() {
    MainWindow window; window.openNarrowbandDemoProject(); showWindow(window);
    int combos=0,spins=0;
    const auto check=[&](QWidget* root) {
        for(auto* combo:root->findChildren<QComboBox*>()) {
            const int index=combo->currentIndex(); const auto text=combo->currentText();
            for(bool focused:{false,true}) { if(focused)combo->setFocus();else window.setFocus();
                for(int delta:{120,-120})wheelAt(combo,combo->rect().center(),delta);
                QVERIFY2(combo->currentIndex()==index,qPrintable(QString("Wheel/focus changed %1 from %2 to %3").arg(combo->objectName()).arg(index).arg(combo->currentIndex()))); QCOMPARE(combo->currentText(),text);
                if(combo->lineEdit()){wheelAt(combo->lineEdit(),combo->lineEdit()->rect().center());QCOMPARE(combo->currentText(),text);}
            } ++combos;
        }
        for(auto* spin:root->findChildren<QAbstractSpinBox*>()) {
            const auto text=spin->findChild<QLineEdit*>()->text();
            for(bool focused:{false,true}) { if(focused)spin->setFocus();else window.setFocus();
                for(int delta:{120,-120})wheelAt(spin,spin->rect().center(),delta);
                QCOMPARE(spin->findChild<QLineEdit*>()->text(),text);
                wheelAt(spin->findChild<QLineEdit*>(),spin->rect().center());
                QCOMPARE(spin->findChild<QLineEdit*>()->text(),text);
            } ++spins;
        }
    };
    check(&window); QVERIFY2(combos>=30 && spins>=12,qPrintable(QString("Checked %1 combos and %2 spins").arg(combos).arg(spins)));
    window.session().project().narrowbandWorkspaceOpen=false;window.refresh();
    for(auto* toggle:window.findChildren<QToolButton*>())
        if(toggle->property("uiRole").toString()=="sectionToggle")toggle->setChecked(true);
    QCoreApplication::processEvents();
    auto* scroll=window.findChild<QScrollArea*>("propScroll");
    QTRY_VERIFY(scroll->verticalScrollBar()->maximum()>0);
    scroll->verticalScrollBar()->setValue(0);
    auto* palette=window.findChild<QComboBox*>("colormap");
    const int paletteIndex=palette->currentIndex();
    wheelAt(palette,palette->rect().center(),-120);
    QCOMPARE(palette->currentIndex(),paletteIndex);
    QVERIFY(scroll->verticalScrollBar()->value()>0);
    auto* dynamic=window.findChild<QComboBox*>("dynamic");dynamic->lineEdit()->setFocus();dynamic->lineEdit()->selectAll();
    const double previous=window.session().activeFile()->display.dynamicRangeDb;QTest::keyClicks(dynamic->lineEdit(),"73.25");
    wheelAt(dynamic,dynamic->rect().center());QCOMPARE(dynamic->currentText(),QString("73.25"));
    QCOMPARE(window.session().activeFile()->display.dynamicRangeDb,previous);
    window.session().project().narrowbandWorkspaceOpen=false;window.refresh();
    window.findChild<QPushButton*>("extract")->click();
    auto* dialog=window.findChild<QDialog*>("channelConfigDialog");QVERIFY(dialog);check(dialog);
    auto* source=dialog->findChild<QComboBox*>("channelSourceMark");source->showPopup();
    const int selected=source->currentIndex();const auto highlighted=source->view()->currentIndex();
    wheelAt(source->view()->viewport(),source->view()->viewport()->rect().center(),-120);
    QCOMPARE(source->currentIndex(),selected);QCOMPARE(source->view()->currentIndex(),highlighted);source->hidePopup();dialog->reject();
    window.findChild<QAction*>("openIqAction")->trigger();
    dialog=window.findChild<QDialog*>("signalImportDialog");QVERIFY(dialog);check(dialog);dialog->reject();
    qInfo("Wheel protection checked %d combos and %d spin boxes, including dialogs and editors",combos,spins);
    // This global policy does not intercept wheel interaction on plot surfaces.
    auto* main=window.findChild<PlotWidget*>("mainPlot");const auto view=window.session().activeFile()->view;
    wheelAt(main,main->plotRect().center());QVERIFY(!(window.session().activeFile()->view==view));
}

void UiTests::pinnedAxisLabelsSurviveLeave() {
    QImage overlay(900,600,QImage::Format_ARGB32_Premultiplied); const QColor background(21,41,61); overlay.fill(background);
    QPainter painter(&overlay); const auto plain=cursor_overlay::draw(painter,QRectF(20,20,860,560),QPointF(450,300),true,
        {"t = 1.234 ms","f = 100 MHz","P = -65.4 dBFS/Hz",{}},false); painter.end();
    for(const auto box : plain.rectangles()) {
        int unchanged=0,total=0;
        for(int py=qCeil(box.top());py<qFloor(box.bottom());++py) for(int px=qCeil(box.left());px<qFloor(box.right());++px) { ++total; if(overlay.pixelColor(px,py)==background) ++unchanged; }
        QVERIFY(total>0 && unchanged>total/2);
    }
    for(const auto backdrop : {QColor(21,41,61),QColor(244,236,12),QColor(190,10,170)}) {
        QImage image(900,600,QImage::Format_ARGB32_Premultiplied); image.fill(backdrop); QPainter destination(&image);
        cursor_overlay::paintInverseOverlay(destination,image.rect(),[](QPainter& mask) {
            cursor_overlay::draw(mask,QRectF(20,20,860,560),QPointF(450,300),true,
                {"t = 1.234 ms","f = 100 MHz","P = -65.4 dBFS/Hz",{}},true,{},true);
        }); destination.end();
        const QColor inverted(255-backdrop.red(),255-backdrop.green(),255-backdrop.blue());
        QCOMPARE(image.pixelColor(450,100),inverted); QCOMPARE(image.pixelColor(450,300),inverted);
        int glyphPixels=0;
        for(int y=250;y<290;++y) for(int x=460;x<620;++x) if(image.pixelColor(x,y)==inverted) ++glyphPixels;
        QVERIFY(glyphPixels>10); // Text glyphs, not a filled label box, also invert.
        QCOMPARE(image.pixelColor(500,200),backdrop);
    }
    DemoMainWindow window; showWindow(window);
    auto* main=window.findChild<PlotWidget*>("mainPlot"); auto* aux=window.findChild<PlotWidget*>("auxPlot");
    QTRY_VERIFY_WITH_TIMEOUT(main->isDisplaySettled(),10000);
    const auto plot=main->plotRect(); const auto point=plot.center().toPoint();
    QTest::mouseClick(main,Qt::LeftButton,Qt::NoModifier,point); QTest::qWait(30);
    QEvent leave(QEvent::Leave); QCoreApplication::sendEvent(main,&leave); QTest::qWait(30);
    const auto pinned=main->property("pinnedReadout").toString();
    QVERIFY(pinned.contains("t =") && pinned.contains("f =") && pinned.contains("P ="));
    QVERIFY(main->property("pinnedReadoutDetails").toString().contains("bin"));
    QVERIFY(!main->property("hoverCursorVisible").toBool());
    const auto x=main->property("pinnedXLabelRect").toRectF(); const auto y=main->property("pinnedYLabelRect").toRectF();
    const auto power=main->property("pinnedValueRect").toRectF();
    QVERIFY(plot.contains(x) && plot.contains(y) && plot.contains(power));
    QVERIFY(std::abs(x.bottom()-plot.bottom())<=5 && std::abs(y.left()-plot.left())<=5);
    QVERIFY(power.left()>point.x() && power.bottom()<point.y());
    const QRectF cursorX(point.x()-1,plot.top(),2,plot.height()),cursorY(plot.left(),point.y()-1,plot.width(),2);
    for(const auto& label : {x,y,power}) { QVERIFY(!label.intersects(cursorX)); QVERIFY(!label.intersects(cursorY)); }
    window.session().setAuxiliaryMode(AuxiliaryMode::Psd); window.refresh(); QTest::qWait(50);
    QCoreApplication::sendEvent(aux,&leave); QTest::qWait(30);
    QVERIFY(aux->property("pinnedReadout").toString().contains("dBFS/Hz"));
    QVERIFY(aux->plotRect().contains(aux->property("pinnedValueRect").toRectF()));
    window.session().clearCursor(window.session().activeFile()->metadata.id); window.refresh();
    for(const auto edge : {plot.topLeft()+QPointF(1,1),plot.bottomRight()-QPointF(1,1)}) {
        QTest::mouseClick(main,Qt::LeftButton,Qt::NoModifier,edge.toPoint()); QCoreApplication::sendEvent(main,&leave); QTest::qWait(30);
        QVERIFY(plot.contains(main->property("pinnedReadoutRect").toRectF()));
    }
    window.session().newProject(); window.refresh(); window.openNarrowbandDemoProject(); auto* workspace=window.findChild<NarrowbandWorkspace*>();
    QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(),15000);
    auto* heat=window.findChild<QWidget*>("narrowbandStftPanelChart");
    const QRectF nbPlot(66,14,heat->width()-82,heat->height()-55);
    QTest::mouseClick(heat,Qt::LeftButton,Qt::NoModifier,nbPlot.center().toPoint()); QCoreApplication::sendEvent(heat,&leave); QTest::qWait(30);
    for(const auto* name : {"narrowbandWaveformPanelChart","narrowbandPsdPanelChart","narrowbandStftPanelChart"}) {
        auto* chart=window.findChild<QWidget*>(name); QCoreApplication::sendEvent(chart,&leave); QTest::qWait(20);
        QVERIFY(!chart->property("pinnedReadout").toString().isEmpty()); QVERIFY(!chart->property("hoverCursorVisible").toBool());
    }
}

void UiTests::sharedInteractionFeedback() {
    DemoMainWindow window; showWindow(window);
    auto* main=window.findChild<PlotWidget*>("mainPlot");
    const auto plot=main->plotRect();
    QTest::mouseMove(main,QPoint(25,qRound(plot.center().y()))); QTest::qWait(20);
    const auto wideZone=main->property("interactionZone"); QCOMPARE(main->cursor().shape(),Qt::OpenHandCursor);
    QTest::mousePress(main,Qt::LeftButton,Qt::NoModifier,QPoint(25,qRound(plot.center().y())));
    QTest::mouseMove(main,QPoint(25,qRound(plot.center().y())+15)); QCOMPARE(main->cursor().shape(),Qt::ClosedHandCursor);
    QTest::keyClick(main,Qt::Key_Escape);
    window.session().newProject(); window.refresh(); window.openNarrowbandDemoProject(); auto* workspace=window.findChild<NarrowbandWorkspace*>();
    QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(),15000);
    for(const auto* name : {"narrowbandWaveformPanelChart","narrowbandPsdPanelChart","narrowbandStftPanelChart"}) {
        auto* chart=window.findChild<QWidget*>(name); const QRectF p(66,14,chart->width()-82,chart->height()-55);
        const QPoint axis(25,qRound(p.center().y()));
        QTest::mouseMove(chart,axis); QTest::qWait(20);
        QCOMPARE(chart->property("interactionZone"),wideZone); QCOMPARE(chart->cursor().shape(),Qt::OpenHandCursor);
        QTest::mousePress(chart,Qt::LeftButton,Qt::NoModifier,axis); QTest::mouseMove(chart,axis+QPoint(0,15));
        QCOMPARE(chart->cursor().shape(),Qt::ClosedHandCursor); QTest::keyClick(chart,Qt::Key_Escape);
        QEvent leave(QEvent::Leave); QCoreApplication::sendEvent(chart,&leave); QCOMPARE(chart->cursor().shape(),Qt::ArrowCursor);
        QCOMPARE(chart->property("interactionZone").toInt(),0);
        QTest::mouseMove(chart,p.center().toPoint()); QCOMPARE(chart->cursor().shape(),Qt::CrossCursor);
    }
}

void UiTests::customWindowControls() {
    DemoMainWindow window; showWindow(window);
    QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
    auto* maximize=window.findChild<QToolButton*>("windowMaximize");
    auto* minimize=window.findChild<QToolButton*>("windowMinimize");
    auto* close=window.findChild<QToolButton*>("windowClose");
    QVERIFY(maximize && minimize && close); QCOMPARE(window.menuBar()->height(),34);
    const auto context=window.session().activeFile()->metadata.id;
    const auto view=window.session().activeFile()->view;
    window.session().pinCursor(context,view.time.begin+10,view.frequency.lowerHz+100,false);
    if(window.isFullScreen()) maximize->click();
    if(QGuiApplication::platformName()!="windows") window.resize(window.size().boundedTo(window.screen()->availableGeometry().size()).expandedTo(window.minimumSize()));
    QVERIFY(!window.isFullScreen()); const auto normal=window.geometry();
    maximize->click(); QTRY_VERIFY(window.isMaximized()); QVERIFY(maximize->property("restoresWindow").toBool());
    maximize->click(); QTRY_VERIFY(!window.isMaximized()); QCOMPARE(window.geometry(),normal);
    if(QGuiApplication::platformName()!="windows") window.resize(2560,1440);
    QCoreApplication::processEvents();
    const QPoint blank(window.menuBar()->width()-210,16);
    QVERIFY(window.menuBar()->rect().contains(blank) && !window.menuBar()->actionAt(blank) && !window.menuBar()->childAt(blank));
    QTest::mouseDClick(window.menuBar(),Qt::LeftButton,Qt::NoModifier,blank); QTRY_VERIFY(window.isMaximized());
    if(QGuiApplication::platformName()=="windows") QTest::mouseDClick(window.menuBar(),Qt::LeftButton,Qt::NoModifier,QPoint(window.menuBar()->width()-210,16));
    else maximize->click();
    QTRY_VERIFY(!window.isMaximized());
    minimize->click(); QTRY_VERIFY(window.isMinimized()); window.showNormal(); QCoreApplication::processEvents();
    QCOMPARE(window.session().activeFile()->view,view); QVERIFY(window.session().linkedCursor(context).pinned);
    close->click(); QVERIFY(!window.isVisible());
}

void UiTests::initialFileViewShowsFirstFivePercentOrTenMilliseconds() {
    Session session;
    QVERIFY(!session.addDemoFile("five_percent.iq", 40e6, 100e6, 10.0).empty());
    auto* file = session.activeFile();
    QVERIFY(file);
    QCOMPARE(file->view.time, (TimeRange{0, 20'000'000ULL}));

    QVERIFY(!session.addDemoFile("ten_ms.iq", 1e6, 10e6, 0.1).empty());
    file = session.activeFile();
    QVERIFY(file);
    QCOMPARE(file->view.time, (TimeRange{0, 10'000ULL}));

    QVERIFY(!session.addDemoFile("short.iq", 1e6, 10e6, 0.006).empty());
    file = session.activeFile();
    QVERIFY(file);
    QCOMPARE(file->view.time, (TimeRange{0, file->metadata.sampleCount}));
}

void UiTests::paletteControlsStaySynchronized() {
    DemoMainWindow window;
    showWindow(window);
    auto* header=window.findChild<QComboBox*>("palette");
    auto* property=window.findChild<QComboBox*>("colormap");
    auto* rangeTag=window.findChild<QWidget*>("rangeTag");
    auto* maximize=window.findChild<QAbstractButton*>("specMaximize");
    QVERIFY(header&&property);
    QVERIFY(rangeTag&&maximize);
    QCOMPARE(header->width(),155);
    auto* spectrumHeader=header->parentWidget();
    QVERIFY(spectrumHeader);
    QVERIFY(header->isVisible()&&rangeTag->isVisible()&&maximize->isVisible());
    QVERIFY(header->geometry().right()<rangeTag->geometry().left());
    QVERIFY(rangeTag->geometry().right()<maximize->geometry().left());
    QVERIFY(maximize->geometry().right()<spectrumHeader->width());
    const QStringList expectedPalettes{"Turbo","Viridis","Gray","Plasma","Inferno","Magma","Cividis","CoolEdit Classic"};
    QCOMPARE(comboLabels(header),expectedPalettes);
    QCOMPARE(comboLabels(property),expectedPalettes);
    QCOMPARE(header->currentText(),QString("CoolEdit Classic"));
    header->setCurrentIndex(1);
    QCOMPARE(property->currentIndex(),1);
    property->setCurrentIndex(2);
    QCOMPARE(header->currentIndex(),2);
    QCOMPARE(static_cast<int>(window.session().activeFile()->display.palette),static_cast<int>(Palette::Gray));
    const auto first=window.session().project().activeFileId;
    QVERIFY(window.session().activateFile(window.session().project().files[1].metadata.id));
    window.refresh();
    QCOMPARE(header->currentIndex(),2);
    QCOMPARE(property->currentIndex(),2);
    QVERIFY(window.session().activateFile(first));
    window.refresh();
    QCOMPARE(header->currentIndex(),2);
    QCOMPARE(property->currentIndex(),2);
    const auto cividisIndex=header->findText("Cividis");
    QVERIFY(cividisIndex>=0);
    header->setCurrentIndex(cividisIndex);
    QCOMPARE(property->currentText(),QString("Cividis"));
    QCOMPARE(window.session().activeFile()->display.palette,Palette::Cividis);
    const auto classicIndex=header->findText("CoolEdit Classic");
    QVERIFY(classicIndex>=0);
    header->setCurrentIndex(classicIndex);
    QCOMPARE(property->currentText(),QString("CoolEdit Classic"));
    QCOMPARE(window.session().activeFile()->display.palette,Palette::CoolEditClassic);
    auto* colorbar=window.findChild<QCheckBox*>("colorbarToggle");
    auto* main=window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(colorbar&&main);
    colorbar->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(main->isDisplaySettled(),10'000);
    const auto image=main->grab().toImage();
    const qreal dpr=image.devicePixelRatio();
    const int colorX=qRound((main->width()-31.5)*dpr);
    QVERIFY(!image.isNull()&&colorX>=0&&colorX<image.width());
    bool hasRedTransition=false;
    const auto plot=main->plotRect();
    for(int y=qRound((plot.top()+4)*dpr);y<qRound((plot.bottom()-4)*dpr);++y) {
        const auto sample=image.pixelColor(colorX,y);
        if(sample.red()>160&&sample.red()>sample.green()*1.35&&sample.red()>sample.blue()*1.35) {
            hasRedTransition=true;
            break;
        }
    }
    QVERIFY2(hasRedTransition,"CoolEdit Classic must include its red high-energy transition.");
}

void UiTests::widebandAuxiliaryRenderingAndGestures() {
    DemoMainWindow window;
    showWindow(window);
    auto* chart = window.findChild<PlotWidget*>("auxPlot");
    auto* mode = window.findChild<QComboBox*>("modeAux");
    auto* grid = window.findChild<QCheckBox*>("gridToggle");
    QVERIFY(chart && mode && grid);
    auto* file = window.session().activeFile();
    for (const auto auxiliaryMode : {AuxiliaryMode::Waveform, AuxiliaryMode::Psd}) {
        mode->setCurrentIndex(static_cast<int>(auxiliaryMode));
        grid->setChecked(true);
        QTRY_VERIFY_WITH_TIMEOUT(chart->isDisplaySettled(), 10'000);
        if (QGuiApplication::platformName() == "windows") {
            QVERIFY(chart->gpuReady());
            auto* surface = chart->findChild<AcceleratedSurface*>();
            QVERIFY(surface && surface->chartDrawCallCount() > 0);
        }
        const QRectF plot = chart->plotRect();
        QVERIFY(traceCoversPlot(chartImage(chart), chart->size(), plot));
        const QImage withGrid = chartImage(chart);
        grid->setChecked(false);
        QTest::qWait(30);
        QVERIFY(changedPlotPixels(withGrid, chartImage(chart), chart->size(), plot) > 50);
        grid->setChecked(true);
        const auto renderingBase = window.session().snapshot();
        const double center = (file->display.auxiliaryMin + file->display.auxiliaryMax) / 2.0;
        const double halfSpan = (file->display.auxiliaryMax - file->display.auxiliaryMin) * .05;
        QVERIFY(window.session().setAuxiliaryRange(center - halfSpan, center + halfSpan, false));
        window.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(chart->isDisplaySettled(), 10'000);
        QCOMPARE(tracePixelsOutsidePlot(chartImage(chart), chart->size(), plot,
            auxiliaryMode == AuxiliaryMode::Psd ? QColor("#5abffa") : QColor("#51d7c5")), 0);
        window.session().restoreSnapshot(renderingBase);
        window.refresh();
        // The real hit-test target must be the chart, not its GPU surface.
        QCOMPARE(window.childAt(chart->mapTo(&window, plot.center().toPoint())), chart);

        const auto base = file->view;
        const QPointF anchor(plot.left() + plot.width() * .3, plot.center().y());
        wheelAt(chart, anchor);
        const auto zoomed = file->view;
        if (auxiliaryMode == AuxiliaryMode::Waveform) {
            QVERIFY(zoomed.time.end - zoomed.time.begin < base.time.end - base.time.begin);
            QVERIFY(std::abs(double(sampleAt(base, .3L)) - double(sampleAt(zoomed, .3L))) <= 2);
            QCOMPARE(zoomed.frequency, base.frequency);
        } else {
            QVERIFY(zoomed.frequency.upperHz - zoomed.frequency.lowerHz < base.frequency.upperHz - base.frequency.lowerHz);
            QVERIFY(std::abs(frequencyAt(base, .3) - frequencyAt(zoomed, .3)) < 1e-6);
            QCOMPARE(zoomed.time, base.time);
        }
        wheelAt(chart, anchor);
        QTest::qWait(300);
        QVERIFY(window.session().back());
        QCOMPARE(file->view, base); // Continuous wheel events produce one entry.
        window.refresh();

        const double low = file->display.auxiliaryMin, high = file->display.auxiliaryMax;
        wheelAt(chart, QPointF(40, plot.center().y()));
        QVERIFY(file->display.auxiliaryMax - file->display.auxiliaryMin < high - low);
        QCOMPARE(file->view, base);
        QTest::qWait(300);
        QVERIFY(window.session().back());
        window.refresh();
        QCOMPARE(file->display.auxiliaryMin, low);
        QCOMPARE(file->display.auxiliaryMax, high);
        const QPoint axis(40, qRound(plot.center().y()));
        QTest::mousePress(chart, Qt::LeftButton, Qt::NoModifier, axis);
        QTest::mouseMove(chart, axis + QPoint(0, 15), 10);
        QVERIFY(file->display.auxiliaryMin != low);
        QTest::keyClick(chart, Qt::Key_Escape);
        QTest::mouseRelease(chart, Qt::LeftButton, Qt::NoModifier, axis + QPoint(0, 15));
        QCOMPARE(file->display.auxiliaryMin, low);
        QCOMPARE(file->display.auxiliaryMax, high);

        const QPoint start(qRound(plot.left() + plot.width() * .2), qRound(plot.center().y()));
        const QPoint end(qRound(plot.left() + plot.width() * .7), qRound(plot.center().y()));
        drag(chart, start, end);
        QVERIFY(file->view != base);
        QVERIFY(window.session().back());
        window.refresh();
        QCOMPARE(file->view, base);
        // Shrink first, so an axis pan has room to move inside the bounds.
        drag(chart, start, end);
        const auto beforePan = file->view;
        drag(chart, QPoint(qRound(plot.center().x()), chart->height() - 10),
             QPoint(qRound(plot.center().x() + plot.width() * .05), chart->height() - 10));
        QVERIFY(file->view != beforePan);
        window.session().setView(base, false);
        window.refresh();
    }
}

void UiTests::narrowbandAuxiliaryRenderingAndGestures() {
    MainWindow window;
    showWindow(window);
    window.openNarrowbandDemoProject();
    auto* workspace = window.findChild<NarrowbandWorkspace*>("narrowbandWorkspace");
    auto* status = window.findChild<QLabel*>("narrowbandDataStatus");
    auto* grid = window.findChild<QCheckBox*>("narrowbandGrid");
    QVERIFY(workspace && status && grid);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("真实 DDC")), 15'000);
    // Keep the deterministic fixture's filtered noise floor inside the PSD
    // viewport so coverage checks test the trace, not a clipped axis border.
    QVERIFY(window.session().setChannelPsdRange(-220.0, 0.0, false));
    workspace->refreshFromSession();
    for (const auto* name : {"narrowbandWaveformPanelChart", "narrowbandPsdPanelChart"}) {
        auto* chart = window.findChild<QWidget*>(QString::fromLatin1(name));
        QVERIFY(chart);
        const bool waveform = QString::fromLatin1(name).contains("Waveform");
        QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("真实 DDC")), 15'000);
        auto* surface = chart->findChild<AcceleratedSurface*>();
        if (QGuiApplication::platformName() == "windows") {
            QTRY_VERIFY_WITH_TIMEOUT(surface && surface->isReady() && !surface->hasPendingUploads(), 5'000);
            QVERIFY(surface->chartDrawCallCount() > 0);
        }
        const QRectF plot(66, 14, chart->width() - 82, chart->height() - 55);
        grid->setChecked(true);
        QTest::qWait(30);
        QVERIFY(traceCoversPlot(chartImage(chart), chart->size(), plot));
        const QImage withGrid = chartImage(chart);
        grid->setChecked(false);
        QTest::qWait(30);
        QVERIFY(changedPlotPixels(withGrid, chartImage(chart), chart->size(), plot) > 50);
        grid->setChecked(true);
        QCOMPARE(window.childAt(chart->mapTo(&window, plot.center().toPoint())), chart);
        const auto base = window.session().channelViewSnapshot();
        auto* channel = window.session().activeChannel();
        const QPointF anchor(plot.left() + plot.width() * .3, plot.center().y());
        wheelAt(chart, anchor);
        if (waveform) {
            QVERIFY(channel->visibleSourceTime.end - channel->visibleSourceTime.begin < base.sourceTime.end - base.sourceTime.begin);
            QCOMPARE(channel->visibleBasebandFrequency, base.basebandFrequency);
            const auto oldAnchor = sampleAt({base.sourceTime, {}}, .3L);
            const auto nextAnchor = sampleAt({channel->visibleSourceTime, {}}, .3L);
            QVERIFY(std::abs(double(oldAnchor) - double(nextAnchor)) <= 2);
        } else {
            QVERIFY(channel->visibleBasebandFrequency.upperHz - channel->visibleBasebandFrequency.lowerHz <
                base.basebandFrequency.upperHz - base.basebandFrequency.lowerHz);
            QCOMPARE(channel->visibleSourceTime, base.sourceTime);
            QVERIFY(std::abs(frequencyAt({{}, base.basebandFrequency}, .3) -
                frequencyAt({{}, channel->visibleBasebandFrequency}, .3)) < 1e-6);
        }
        wheelAt(chart, anchor);
        QTest::qWait(300);
        QVERIFY(window.session().channelBack());
        window.refresh();
        QCOMPARE(channel->visibleSourceTime, base.sourceTime);
        QCOMPARE(channel->visibleBasebandFrequency, base.basebandFrequency);
        const double low = waveform ? channel->waveformAxisMinimum : channel->psdAxisMinimum;
        const double high = waveform ? channel->waveformAxisMaximum : channel->psdAxisMaximum;
        wheelAt(chart, QPointF(30, plot.center().y()));
        QVERIFY((waveform ? channel->waveformAxisMaximum - channel->waveformAxisMinimum :
            channel->psdAxisMaximum - channel->psdAxisMinimum) < high - low);
        QTest::keyClick(chart, Qt::Key_Escape);
        QCOMPARE(waveform ? channel->waveformAxisMinimum : channel->psdAxisMinimum, low);
        QCOMPARE(waveform ? channel->waveformAxisMaximum : channel->psdAxisMaximum, high);
        const QPoint start(qRound(plot.left() + plot.width() * .2), qRound(plot.center().y()));
        const QPoint end(qRound(plot.left() + plot.width() * .7), qRound(plot.center().y()));
        QTest::mousePress(chart, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(chart, end, 10);
        QTest::keyClick(chart, Qt::Key_Escape);
        QTest::mouseRelease(chart, Qt::LeftButton, Qt::NoModifier, end);
        QCOMPARE(channel->visibleSourceTime, base.sourceTime);
        QCOMPARE(channel->visibleBasebandFrequency, base.basebandFrequency);
        drag(chart, start, end);
        const auto beforePan = window.session().channelViewSnapshot();
        QVERIFY(beforePan.sourceTime != base.sourceTime || beforePan.basebandFrequency != base.basebandFrequency);
        drag(chart, QPoint(qRound(plot.center().x()), chart->height() - 10),
             QPoint(qRound(plot.center().x() + plot.width() * .05), chart->height() - 10));
        QVERIFY(channel->visibleSourceTime != beforePan.sourceTime || channel->visibleBasebandFrequency != beforePan.basebandFrequency);
        window.session().restoreChannelViewSnapshot(base);
        window.refresh();
    }
    workspace->cancelWork();
}

void UiTests::widebandLinkedCursorsAndFrameSpectrum() {
    MainWindow window;
    window.session().addDemoFile(); window.refresh(); showWindow(window);
    auto* main = window.findChild<PlotWidget*>("mainPlot");
    auto* aux = window.findChild<PlotWidget*>("auxiliaryPlot");
    if (!aux) aux = window.findChild<PlotWidget*>("auxPlot");
    QVERIFY(main && aux);
    QTRY_VERIFY_WITH_TIMEOUT(main->isDisplaySettled(), 15'000);
    const auto context = window.session().activeFile()->metadata.id;
    const auto matrix = window.session().spectrogram(context);
    QVERIFY(matrix && !matrix->frames.empty());
    const auto plot = main->plotRect();
    const QPoint point(qRound(plot.left() + plot.width() * .35), qRound(plot.top() + plot.height() * .45));
    const auto generations = main->powerGenerationCount();
    const auto uploads = main->textureUploadCount();
    auto* surface = main->findChild<AcceleratedSurface*>();
    const auto vertices = surface ? surface->chartVertexUploadCount() : 0;
    QTest::mouseMove(main, point); QTest::qWait(50);
    QVERIFY(main->property("hoverCursorVisible").toBool());
    QVERIFY(main->property("cursorReadout").toString().contains("dBFS/Hz"));
    QVERIFY(plot.contains(main->property("cursorReadoutRect").toRectF()));
    QCOMPARE(main->powerGenerationCount(), generations); QCOMPARE(main->textureUploadCount(), uploads);
    if (surface) QCOMPARE(surface->chartVertexUploadCount(), vertices);
    QTest::mouseClick(main, Qt::LeftButton, Qt::NoModifier, point);
    const auto pinned = window.session().linkedCursor(context);
    QVERIFY(pinned.pinned && pinned.framePsd);
    const auto* frame = window.session().selectedSpectralFrame(context); QVERIFY(frame);
    auto* mode = window.findChild<QComboBox*>("modeAux"); QVERIFY(mode);
    mode->setCurrentIndex(static_cast<int>(AuxiliaryMode::Psd));
    QTRY_VERIFY_WITH_TIMEOUT(aux->isDisplaySettled(), 15'000);
    QTRY_COMPARE_WITH_TIMEOUT(aux->sourcePointCount(), qsizetype(frame->linearPower.size()), 15'000);
    QTest::mouseMove(aux, aux->plotRect().center().toPoint()); QTest::qWait(30);
    QVERIFY(aux->property("cursorPinned").toBool()); QVERIFY(aux->property("framePsd").toBool());
    const auto pinnedBeforeFrequency = window.session().linkedCursor(context);
    QTest::mouseClick(aux, Qt::LeftButton, Qt::NoModifier, (aux->plotRect().topLeft() + QPointF(aux->plotRect().width() * .7, 35)).toPoint());
    QCOMPARE(window.session().linkedCursor(context).sourceSample, pinnedBeforeFrequency.sourceSample);
    QVERIFY(window.session().linkedCursor(context).frequencyHz != pinnedBeforeFrequency.frequencyHz);
    mode->setCurrentIndex(static_cast<int>(AuxiliaryMode::Waveform));
    QTRY_VERIFY_WITH_TIMEOUT(aux->isDisplaySettled(), 10'000);
    QTest::mouseMove(aux, aux->plotRect().center().toPoint()); QTest::qWait(30);
    QVERIFY(aux->property("cursorReadout").toString().contains("ADC"));
    auto menu = std::unique_ptr<QMenu>(main->createContextMenu(point));
    auto* clear = menu->findChild<QAction*>("contextClearCursor"); QVERIFY(clear); clear->trigger();
    QVERIFY(!window.session().linkedCursor(context).pinned);
    QEvent leave(QEvent::Leave); QCoreApplication::sendEvent(main, &leave); QTest::qWait(30);
    QVERIFY(!main->property("hoverCursorVisible").toBool());
    auto* file = window.session().activeFile(); const auto before = file->view;
    window.session().setView({{before.time.begin, before.time.begin + 8}, before.frequency}); window.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(main->isDisplaySettled(), 10'000);
    const auto unavailable = window.session().spectrogram(context);
    QVERIFY(unavailable && !unavailable->frames.empty() && unavailable->error.empty());
    QCOMPARE(file->view.time,(TimeRange{before.time.begin,before.time.begin+8}));
    QVERIFY(unavailable->frames.front()->paddedSamples>0);
    menu.reset(main->createContextMenu(point));
    auto* expand = menu->findChild<QAction*>("contextExpandAnalysisTime"); QVERIFY(expand); expand->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(main->isDisplaySettled(), 15'000);
    QVERIFY(!window.session().spectrogram(context)->frames.empty());
    const auto generation = window.session().projectGeneration();
    auto replacement = window.session().project(); window.session().replaceProject(std::move(replacement)); window.refresh();
    QVERIFY(window.session().projectGeneration() != generation);
    QTRY_VERIFY_WITH_TIMEOUT(main->isDisplaySettled(), 15'000);
    QVERIFY(window.session().spectrogram(context));
}

void UiTests::narrowbandLinkedCursorsAndFrameSpectrum() {
    MainWindow window; window.openNarrowbandDemoProject(); showWindow(window);
    auto* workspace = window.findChild<NarrowbandWorkspace*>("narrowbandWorkspace"); QVERIFY(workspace);
    auto* heat = window.findChild<QWidget*>("narrowbandStftPanelChart");
    auto* wave = window.findChild<QWidget*>("narrowbandWaveformPanelChart");
    auto* psd = window.findChild<QWidget*>("narrowbandPsdPanelChart");
    auto* psdPoints = window.findChild<QComboBox*>("narrowbandPsdFft");
    QVERIFY(heat && wave && psd && psdPoints);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(), 15'000);
    auto* rightStft = window.findChild<QComboBox*>("stftFft");
    auto* headerStft = window.findChild<QComboBox*>("narrowbandStftFft");
    QVERIFY(rightStft && headerStft);
    const int wideStft = window.session().activeFile()->display.stftSize;
    rightStft->setCurrentText("4096");
    QCOMPARE(window.session().activeChannel()->stftFftSize, 4096);
    QCOMPARE(headerStft->currentData().toInt(), 4096);
    QCOMPARE(window.session().activeFile()->display.stftSize, wideStft);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(), 15'000);
    headerStft->setCurrentIndex(headerStft->findData(2048));
    QCOMPARE(rightStft->currentText(), QString("2048"));
    QCOMPARE(window.session().activeFile()->display.stftSize, wideStft);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(), 15'000);
    const auto context = window.session().activeChannel()->id;
    const auto matrix = window.session().spectrogram(context); QVERIFY(matrix && !matrix->frames.empty());
    const QRectF plot(66, 14, heat->width() - 82, heat->height() - 55);
    QTest::mouseMove(heat, plot.center().toPoint()); QTest::qWait(40);
    QVERIFY(heat->property("cursorReadout").toString().contains("dBFS/Hz"));
    QVERIFY(plot.contains(heat->property("cursorReadoutRect").toRectF()));
    const auto hoveredSample = heat->property("cursorSourceSample").toULongLong();
    const double hoveredSeconds = static_cast<double>(hoveredSample) / window.session().activeFile()->metadata.sampleRateHz;
    QCOMPARE(heat->property("cursorTimeSeconds").toDouble(), hoveredSeconds);
    QVERIFY(heat->property("cursorReadout").toString().contains(QString::number(hoveredSeconds / .001, 'g', 10)));
    const auto uploads = workspace->visibleTextureUploads(), vertices = workspace->visibleGpuVertexUploads();
    QTest::mouseMove(heat, (plot.center() + QPointF(30, 20)).toPoint()); QTest::qWait(50);
    QCOMPARE(workspace->visibleTextureUploads(), uploads); QCOMPARE(workspace->visibleGpuVertexUploads(), vertices);
    QTest::mouseClick(heat, Qt::LeftButton, Qt::NoModifier, plot.center().toPoint()); QTest::qWait(40);
    QVERIFY(window.session().linkedCursor(context).pinned);
    const auto* frame = window.session().selectedSpectralFrame(context); QVERIFY(frame);
    QCOMPARE(psd->property("effectiveFftPoints").toInt(), int(frame->linearPower.size()));
    QVERIFY(!psdPoints->isEnabled());
    for (auto* chart : {wave, psd, heat}) QVERIFY(chart->property("cursorPinned").toBool());
    const auto before = window.session().linkedCursor(context);
    const QRectF psdPlot(66, 14, psd->width() - 82, psd->height() - 55);
    QTest::mouseClick(psd, Qt::LeftButton, Qt::NoModifier, (psdPlot.topLeft() + QPointF(psdPlot.width() * .75, 25)).toPoint());
    QCOMPARE(window.session().linkedCursor(context).sourceSample, before.sourceSample);
    QVERIFY(window.session().linkedCursor(context).frequencyHz != before.frequencyHz);
    auto* frequency = window.findChild<QComboBox*>("channelFrequencyMode"); QVERIFY(frequency);
    const auto bb = window.session().linkedCursor(context).frequencyHz; frequency->setCurrentIndex(1);
    QCOMPARE(window.session().linkedCursor(context).frequencyHz, bb);
    auto* rightFrequency = window.findChild<QComboBox*>("freqMode");
    if (!rightFrequency) rightFrequency = window.findChild<QComboBox*>("frequencyMode");
    QVERIFY(rightFrequency); QCOMPARE(rightFrequency->currentIndex(), 0);
    rightFrequency->setCurrentIndex(1); QCOMPARE(frequency->currentIndex(), 0);
    QCOMPARE(window.session().linkedCursor(context).frequencyHz, bb);
    auto* rightGrid = window.findChild<QCheckBox*>("gridToggle");
    auto* headerGrid = window.findChild<QCheckBox*>("narrowbandGrid");
    QVERIFY(rightGrid && headerGrid);
    rightGrid->setChecked(false); QCOMPARE(headerGrid->isChecked(), false);
    headerGrid->setChecked(true); QCOMPARE(rightGrid->isChecked(), true);

    QTest::keyClick(heat, Qt::Key_Escape); QTest::qWait(30);
    QVERIFY(!window.session().linkedCursor(context).pinned); QVERIFY(psdPoints->isEnabled());
    const auto time = window.session().activeChannel()->visibleSourceTime;
    const auto band = window.session().activeChannel()->visibleBasebandFrequency;
    window.session().setChannelView(time, {band.lowerHz / 2, band.upperHz / 2}); window.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(), 15'000);
    const auto zoomed = window.session().spectrogram(context); QVERIFY(zoomed);
    QCOMPARE(zoomed->plan.points, matrix->plan.points);
    QVERIFY(zoomed->plan.binHz < matrix->plan.binHz && zoomed->plan.inputSamples > matrix->plan.inputSamples);
}

void UiTests::narrowbandPaletteSelectionUpdatesStftCharts() {
    MainWindow window;
    window.resize(1600, 900);
    window.show();
    window.openNarrowbandDemoProject();

    auto* palette = window.findChild<QComboBox*>("colormap");
    auto* status = window.findChild<QLabel*>("narrowbandDataStatus");
    auto* observation = window.findChild<QWidget*>("narrowbandStftPanelChart");
    auto* modulation = window.findChild<QWidget*>("narrowbandModulationStftChart");
    auto* recognition = window.findChild<QWidget*>("recognitionStftPanelChart");
    QVERIFY(palette && status && observation && modulation && recognition);
    QVERIFY(window.session().activeFile() && window.session().activeChannel());
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("真实 DDC")), 15'000);

    const int originalPalette = palette->currentIndex();
    QCOMPARE(observation->property("paletteIndex").toInt(), originalPalette);
    QCOMPARE(modulation->property("paletteIndex").toInt(), originalPalette);
    QCOMPARE(recognition->property("paletteIndex").toInt(), originalPalette);
    const auto oldHeatmap = observation->grab().toImage();

    const int nextPalette = originalPalette == static_cast<int>(Palette::Gray)
        ? static_cast<int>(Palette::Turbo) : static_cast<int>(Palette::Gray);
    palette->setCurrentIndex(nextPalette);
    QCOMPARE(window.session().activeFile()->display.palette, static_cast<Palette>(nextPalette));
    QCoreApplication::processEvents();
    QCOMPARE(observation->property("paletteIndex").toInt(), nextPalette);
    QCOMPARE(modulation->property("paletteIndex").toInt(), nextPalette);
    QCOMPARE(recognition->property("paletteIndex").toInt(), nextPalette);
    QVERIFY(oldHeatmap != observation->grab().toImage());
    if (auto* workspace = window.findChild<NarrowbandWorkspace*>("narrowbandWorkspace"))
        workspace->cancelWork();
}

void UiTests::panelRailsAndBottomTabs() {
    DemoMainWindow window;
    showWindow(window);
    auto* resources=window.findChild<QWidget*>("resources");
    auto* properties=window.findChild<QWidget*>("properties");
    auto* resourceToggle=window.findChild<QAbstractButton*>("resourceToggle");
    auto* propClose=window.findChild<QAbstractButton*>("propClose");
    auto* propOpen=window.findChild<QAbstractButton*>("propRailOpen");
    auto* bottom=window.findChild<QWidget*>("bottom");
    auto* toggle=window.findChild<QToolButton*>("resultsToggle");
    QVERIFY(resources&&properties&&resourceToggle&&propClose&&propOpen&&bottom&&toggle);
    QCOMPARE(bottom->height(),29);
    QTest::mouseClick(resourceToggle,Qt::LeftButton);
    QCOMPARE(resources->width(),38);
    QTest::mouseClick(resourceToggle,Qt::LeftButton);
    QCOMPARE(resources->width(),270);
    QTest::mouseClick(propClose,Qt::LeftButton);
    QCOMPARE(properties->width(),38);
    QVERIFY(propOpen->isVisible());
    QTest::mouseClick(propOpen,Qt::LeftButton);
    QCOMPARE(properties->width(),294);
    for (const auto& tabName : {"resultTab","taskTab","logTab"}) {
        auto* tab=window.findChild<QAbstractButton*>(tabName);
        QVERIFY(tab);
        QTest::mouseClick(tab,Qt::LeftButton);
        QVERIFY(tab->isChecked());
        QCOMPARE(bottom->height(),150);
        QVERIFY(toggle->isChecked());
    }
    auto* logs=window.findChild<QPlainTextEdit*>("resultsPanel");
    QVERIFY(logs&&logs->isVisible());
    QTest::mouseClick(toggle,Qt::LeftButton);
    QCOMPARE(bottom->height(),29);
    QVERIFY(!logs->isVisible());
}

void UiTests::sectionContextAndManualExpansion() {
    DemoMainWindow window;
    showWindow(window);
    auto* file=window.findChild<QWidget*>("fileSection");
    auto* mark=window.findChild<QWidget*>("markSection");
    auto* psd=window.findChild<QWidget*>("psdSection");
    auto* toggle=window.findChild<QToolButton*>("psdSectionToggle");
    auto* content=window.findChild<QWidget*>("psdSectionContent");
    QVERIFY(file&&mark&&psd&&toggle&&content);
    QVERIFY(file->isVisible());
    QVERIFY(!mark->isVisible());
    QVERIFY(content->isVisible());
    QTest::mouseClick(toggle,Qt::LeftButton);
    QVERIFY(!content->isVisible());
    window.refresh();window.refresh();
    QVERIFY(!content->isVisible());
    window.session().addMark(innerRange(window.session().activeFile()->view));
    window.refresh();
    QCoreApplication::processEvents();
    QVERIFY(mark->isVisible());
    QVERIFY(mark->mapTo(&window,QPoint()).y()<file->mapTo(&window,QPoint()).y());
    QCOMPARE(window.findChildren<QWidget*>("markSection").size(),1);
    window.session().selectMarks({});
    window.refresh();
    QCoreApplication::processEvents();
    QVERIFY(!mark->isVisible());
    auto* psdMode=window.findChild<QAbstractButton*>("psdMode");
    QVERIFY(psdMode);
    QTest::mouseClick(psdMode,Qt::LeftButton);
    QCoreApplication::processEvents();
    QVERIFY(!content->isVisible());
    QVERIFY(psd->mapTo(&window,QPoint()).y()<file->mapTo(&window,QPoint()).y());
}

void UiTests::panelMaximizeAndRestore() {
    DemoMainWindow window;
    showWindow(window);
    const QStringList panelNames={"navPanel","auxPanel","specPanel"};
    const QStringList buttonNames={"navMaximize","auxMaximize","specMaximize"};
    const QStringList plotNames={"navigationPlot","auxPlot","mainPlot"};
    const bool checkGpu=QGuiApplication::platformName()=="windows";
    for (int i=0;i<panelNames.size();++i) {
        auto* panel=window.findChild<QWidget*>(panelNames[i]);
        auto* button=window.findChild<QAbstractButton*>(buttonNames[i]);
        auto* plot=window.findChild<PlotWidget*>(plotNames[i]);
        QVERIFY(panel&&button&&plot);
        if(checkGpu) {
            QTRY_VERIFY(plot->gpuReady());
            QTRY_VERIFY(plot->completedFrameCount()>0);
        }
        const auto initialFrames=plot->completedFrameCount();
        const auto originalSize=panel->size();
        QTest::mouseClick(button,Qt::LeftButton);
        QCoreApplication::processEvents();
        QVERIFY(panel->isVisible());
        QVERIFY(panel->height()>originalSize.height());
        if(checkGpu) {
            QTRY_VERIFY(plot->gpuReady());
            QTRY_VERIFY(plot->completedFrameCount()>initialFrames);
        }
        const auto maximizedFrames=plot->completedFrameCount();
        QTest::mouseClick(button,Qt::LeftButton);
        QCoreApplication::processEvents();
        auto* graphSplitter=window.findChild<QSplitter*>("graphSplitter");
        QVERIFY(graphSplitter);
        qInfo().noquote()<<panelNames[i]<<"original"<<originalSize<<"restored"<<panel->size()
                        <<"graph sizes"<<graphSplitter->sizes();
        QVERIFY(std::abs(panel->height()-originalSize.height())<=2);
        if(checkGpu) {
            QTRY_VERIFY(plot->gpuReady());
            QTRY_VERIFY(plot->completedFrameCount()>maximizedFrames);
            qInfo().noquote()<<plotNames[i]<<plot->renderingBackend()<<"frames initial/max/restored"
                            <<initialFrames<<maximizedFrames<<plot->completedFrameCount();
        }
        for (const auto& name : panelNames) QVERIFY(window.findChild<QWidget*>(name)->isVisible());
        QVERIFY(window.findChild<QWidget*>("navSplit"));
        QVERIFY(window.findChild<QWidget*>("auxSplit"));
    }
}

void UiTests::mainAxisWheelIsolationAndHistory_data() {
    QTest::addColumn<bool>("waterfall");
    QTest::addColumn<bool>("leftAxis");
    QTest::newRow("tf-time-x")<<false<<false;
    QTest::newRow("tf-frequency-y")<<false<<true;
    QTest::newRow("waterfall-frequency-x")<<true<<false;
    QTest::newRow("waterfall-time-y")<<true<<true;
}

void UiTests::mainAxisWheelIsolationAndHistory() {
    QFETCH(bool,waterfall);
    QFETCH(bool,leftAxis);
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    auto* file=window.session().activeFile();
    file->display.mainMode=waterfall?MainMode::Waterfall:MainMode::TimeFrequency;
    window.refresh();
    const auto original=file->view;
    QPointF point=plot->plotRect().center();
    if(leftAxis) point.setX(plot->plotRect().left()-20);
    wheelAt(plot,point);wheelAt(plot,point);
    const bool frequencyAxis=waterfall?!leftAxis:leftAxis;
    if(frequencyAxis) {
        QVERIFY(file->view.time==original.time);
        QVERIFY(file->view.frequency.upperHz-file->view.frequency.lowerHz<original.frequency.upperHz-original.frequency.lowerHz);
    } else {
        QVERIFY(file->view.frequency==original.frequency);
        QVERIFY(file->view.time.end-file->view.time.begin<original.time.end-original.time.begin);
    }
    QTRY_VERIFY_WITH_TIMEOUT(window.session().canBack(),1000);
    QVERIFY(window.session().back());
    QVERIFY(file->view==original);
    QVERIFY(!window.session().canBack());
}

void UiTests::navigationClickPreservesSpanAndCancelsDrag() {
    DemoMainWindow window;
    showWindow(window);
    auto* nav=window.findChild<PlotWidget*>("navigationPlot");
    QVERIFY(nav);
    auto* file=window.session().activeFile();
    const auto original=file->view;
    const auto full=fullRange(file->metadata);
    const auto jump=nav->toPixel(sampleAt(full,.8L),file->metadata.centerFrequencyHz).toPoint();
    QTest::mouseClick(nav,Qt::LeftButton,Qt::NoModifier,jump);
    QVERIFY(file->view.time.begin>original.time.begin);
    QCOMPARE(file->view.time.end-file->view.time.begin,original.time.end-original.time.begin);
    QVERIFY(file->view.frequency==original.frequency);
    QVERIFY(window.session().back());
    QVERIFY(file->view==original);
    const auto inside=nav->toPixel(sampleAt(original,.5L),file->metadata.centerFrequencyHz).toPoint();
    QTest::mousePress(nav,Qt::LeftButton,Qt::NoModifier,inside);
    QTest::mouseMove(nav,inside+QPoint(45,0),10);
    QTest::keyClick(nav,Qt::Key_Escape);
    QTest::mouseRelease(nav,Qt::LeftButton,Qt::NoModifier,inside+QPoint(45,0));
    QVERIFY(file->view==original);
    QVERIFY(!window.session().canBack());
}

void UiTests::independentSpectralSettingsAndCustomRows() {
    MainWindow window;window.openNarrowbandDemoProject();showWindow(window);
    auto* workspace=window.findChild<NarrowbandWorkspace*>();QVERIFY(workspace);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(),15000);
    const auto context=window.session().activeChannel()->id;
    const auto spectrum=window.session().spectrogram(context);
    const QJsonValue generation=workspace->renderStatistics().value("requestGeneration");
    auto* psd=window.findChild<QComboBox*>("psdFft");psd->setCurrentText("32");
    QTRY_COMPARE(window.session().activeChannel()->psdFftSize,32);
    QCOMPARE(workspace->renderStatistics()["requestGeneration"],generation);QCOMPARE(window.session().spectrogram(context),spectrum);
    auto* windowType=window.findChild<QComboBox*>("psdWindow");windowType->setCurrentIndex(2);
    QCOMPARE(window.session().activeChannel()->psd.parameters.window,SpectralWindow::Hamming);
    QCOMPARE(window.session().spectrogram(context),spectrum);
    auto* method=window.findChild<QComboBox*>("psdMethod");method->setCurrentIndex(1);
    QCOMPARE(window.findChild<QComboBox*>("psdOverlap")->currentText(),QString("0"));
    QVERIFY(!window.findChild<QComboBox*>("psdOverlap")->isEnabled());method->setCurrentIndex(2);
    auto* stft=window.findChild<QComboBox*>("stftFft");stft->setCurrentText("32");
    QTRY_VERIFY_WITH_TIMEOUT(window.session().spectrogram(context)&&window.session().spectrogram(context)!=spectrum,15000);
    auto* dynamic=window.findChild<QComboBox*>("dynamic");const int presets=dynamic->count();
    dynamic->lineEdit()->setFocus();dynamic->lineEdit()->selectAll();QTest::keyClicks(dynamic->lineEdit(),"71.5");QTest::keyClick(dynamic->lineEdit(),Qt::Key_Return);
    QCOMPARE(dynamic->itemData(0).toDouble(),71.5);QCOMPARE(dynamic->count(),presets+2);QVERIFY(dynamic->itemText(1).isEmpty());
    dynamic->lineEdit()->selectAll();QTest::keyClicks(dynamic->lineEdit(),"87.25");QTest::keyClick(dynamic->lineEdit(),Qt::Key_Return);
    QCOMPARE(dynamic->itemData(0).toDouble(),87.25);QCOMPARE(dynamic->count(),presets+2);QCOMPARE(dynamic->currentText(),QString("87.25 dB"));
    QVERIFY(dynamic->lineEdit()->hasFocus());
    window.session().project().narrowbandWorkspaceOpen=false;window.refresh();
    auto* nav=window.findChild<PlotWidget*>("navigationPlot");auto* file=window.session().activeFile();
    const auto total=availableSamples(file->metadata);window.session().setView({{total/4,total/2},file->view.frequency},false);window.refresh();
    const auto center=file->view.time.begin+(file->view.time.end-file->view.time.begin)/2;
    wheelAt(nav,QPointF(nav->width()*.9,nav->height()*.5));
    const auto after=file->view.time.begin+(file->view.time.end-file->view.time.begin)/2;
    QVERIFY(after>=center?after-center<=1:center-after<=1);
}
void UiTests::loadedPrefixStopAndRoundtrip() {
    QTemporaryDir directory;QVERIFY(directory.isValid());const auto path=directory.filePath("IQ0_FS1Msps_BW800kHz_FC10MHz.dat");
    QFile raw(path);QVERIFY(raw.open(QIODevice::WriteOnly));QVERIFY(raw.resize(256*1024*1024));raw.close();
    MainWindow window;showWindow(window);QString error;QVERIFY2(window.addIqFile(path,&error),qPrintable(error));
    auto* file=window.session().activeFile();QVERIFY(file);QCOMPARE(file->metadata.availability.status,LoadStatus::Loading);
    QTRY_VERIFY_WITH_TIMEOUT(file->metadata.availability.availableSamples>0,10000);
    auto* stop=window.findChild<QPushButton*>("stopSourceLoad");QVERIFY(stop&&stop->isVisible());stop->click();
    QTRY_VERIFY_WITH_TIMEOUT(file->metadata.availability.status!=LoadStatus::Loading,10000);
    QCOMPARE(file->metadata.availability.status,LoadStatus::Partial);const auto prefix=availableSamples(file->metadata);
    QVERIFY(prefix>0&&prefix<file->metadata.sampleCount);QVERIFY(file->view.time.end<=prefix);QVERIFY(file->navigationEnvelope.size()<=2048);
    QVERIFY(!stop->isVisible());QVERIFY(window.saveProject(directory.filePath("project.json")));
    QVERIFY(window.openProject(directory.filePath("project.json")));
    QTRY_VERIFY_WITH_TIMEOUT(window.session().activeFile()->metadata.availability.status!=LoadStatus::Loading,10000);
    QCOMPARE(availableSamples(window.session().activeFile()->metadata),prefix);
    QVERIFY(window.session().activeFile()->view.time.end<=prefix);
}
void UiTests::fileSwitchCancelsUnfinishedEdit() {
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    auto* first=window.session().activeFile();
    const auto firstId=first->metadata.id;
    const auto view=first->view;
    const auto markRange=innerRange(view);
    const auto markId=window.session().addMark(markRange);
    window.refresh();
    const auto start=pointAt(plot,view,.35,.5);
    QTest::mousePress(plot,Qt::LeftButton,Qt::NoModifier,start);
    QTest::mouseMove(plot,start+QPoint(35,20),10);
    QVERIFY(!(findMark(*first,markId)->range==markRange));
    auto* tree=window.findChild<QTreeWidget*>("projectTree");
    QVERIFY(tree);
    auto* nextFile=tree->topLevelItem(0)->child(1);
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualItemRect(nextFile).center());
    QCoreApplication::processEvents();
    QVERIFY(window.session().project().activeFileId!=firstId);
    QTest::mouseRelease(plot,Qt::LeftButton,Qt::NoModifier,start+QPoint(35,20));
    QVERIFY(window.session().activateFile(firstId));window.refresh();
    QVERIFY(findMark(*window.session().activeFile(),markId)->range==markRange);
    QVERIFY(window.session().activeFile()->view==view);
    QVERIFY(!plot->isCreating());
}

void UiTests::allResizeHandlesStayBounded_data() {
    QTest::addColumn<bool>("waterfall");
    QTest::addColumn<QString>("handle");
    for (bool waterfall : {false,true}) for (const auto& handle : {"n","ne","e","se","s","sw","w","nw"})
        QTest::newRow(qPrintable(QString("%1-%2").arg(waterfall?"waterfall":"tf",handle)))<<waterfall<<QString(handle);
}

void UiTests::allResizeHandlesStayBounded() {
    QFETCH(bool,waterfall);
    QFETCH(QString,handle);
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    auto* file=window.session().activeFile();
    file->display.mainMode=waterfall?MainMode::Waterfall:MainMode::TimeFrequency;
    const auto view=file->view;
    const auto range=innerRange(view);
    const auto id=window.session().addMark(range);
    window.refresh();
    const QRectF box=QRectF(plot->toPixel(range.time.begin,range.frequency.lowerHz),
                           plot->toPixel(range.time.end,range.frequency.upperHz)).normalized();
    QPointF start=box.center();
    int dx=0,dy=0;
    if(handle.contains('w')) {start.setX(box.left());dx=600;}
    if(handle.contains('e')) {start.setX(box.right());dx=-600;}
    if(handle.contains('n')) {start.setY(box.top());dy=600;}
    if(handle.contains('s')) {start.setY(box.bottom());dy=-600;}
    drag(plot,start.toPoint(),start.toPoint()+QPoint(dx,dy));
    const auto edited=findMark(*file,id)->range;
    const auto bounds=fullRange(file->metadata);
    QVERIFY(!(edited==range));
    QVERIFY(edited.time.begin<edited.time.end);
    QVERIFY(edited.time.end<=bounds.time.end);
    QVERIFY(edited.frequency.lowerHz<edited.frequency.upperHz);
    QVERIFY(edited.frequency.lowerHz>=bounds.frequency.lowerHz);
    QVERIFY(edited.frequency.upperHz<=bounds.frequency.upperHz);
    QVERIFY(edited.time.end-edited.time.begin>=static_cast<SampleIndex>(file->display.stftSize));
    QVERIFY(edited.frequency.upperHz-edited.frequency.lowerHz>=file->metadata.sampleRateHz/file->display.stftSize-.001);
    QVERIFY(file->view==view);
    QVERIFY(!window.session().canBack());
}

void UiTests::nativeProjectRoundtripAndInvalidImport() {
    DemoMainWindow window;
    showWindow(window);
    window.session().addMark(innerRange(window.session().activeFile()->view));
    window.session().addMark(innerRange(window.session().activeFile()->view));
    const auto secondId=window.session().project().files[1].metadata.id;
    QVERIFY(window.session().activateFile(secondId));window.refresh();
    const auto markId=window.session().addMark(innerRange(window.session().activeFile()->view));
    window.refresh();
    auto* mode=window.findChild<QComboBox*>("modeMain");
    auto* auxiliary=window.findChild<QComboBox*>("modeAux");
    auto* palette=window.findChild<QComboBox*>("palette");
    auto* scope=window.findChild<QComboBox*>("psdScope");
    QVERIFY(mode&&auxiliary&&palette&&scope);
    mode->setCurrentIndex(1);
    auxiliary->setCurrentIndex(1);
    palette->setCurrentIndex(1);
    scope->setCurrentIndex(1);
    QCOMPARE(mode->currentIndex(),1);
    QCOMPARE(auxiliary->currentIndex(),1);
    QCOMPARE(palette->currentIndex(),1);
    QCOMPARE(scope->currentIndex(),1);
    const auto expected=window.session().activeFile()->view;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto saved=temporary.filePath("roundtrip.json");
    QVERIFY(window.saveProject(saved));
    window.session().newProject();window.refresh();
    QVERIFY(window.openProject(saved));
    QVERIFY(window.session().project().activeFileId==secondId);
    QVERIFY(window.session().activeFile()->view==expected);
    QVERIFY(window.session().activeFile()->activeMarkId==markId);
    QCOMPARE(window.session().activeFile()->selectedMarkIds.size(),std::size_t{1});
    QCOMPARE(palette->currentIndex(),1);
    QCOMPARE(scope->currentIndex(),1);
    const auto invalid=temporary.filePath("invalid.json");
    QFile file(invalid);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"signals\":[{\"name\":\"bad\",\"fs\":0}]}");file.close();
    QTimer dismiss;
    connect(&dismiss,&QTimer::timeout,[]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))box->accept();});
    dismiss.start(5);
    QVERIFY(!window.openProject(invalid));
    dismiss.stop();
    QVERIFY(window.session().project().activeFileId==secondId);
    QVERIFY(window.session().activeFile()->view==expected);
    QCOMPARE(window.session().project().files.size(),std::size_t{3});
}

void UiTests::contextMenuActionsAndCoveredMarkSelection() {
    DemoMainWindow window;
    showWindow(window);
    const QStringList viewActions={"contextBackAction","contextForwardAction","contextFitAction","contextResetAction"};
    for (const auto& plotName : {"navigationPlot","auxPlot","mainPlot"}) {
        auto* plot=window.findChild<PlotWidget*>(plotName);
        QVERIFY(plot);
        std::unique_ptr<QMenu> menu(plot->createContextMenu(plot->plotRect().center().toPoint()));
        QVERIFY(menu);
        for (const auto& name : viewActions) QVERIFY(menu->findChild<QAction*>(name));
    }
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    const auto view=window.session().activeFile()->view;
    const auto lower=window.session().addMark(innerRange(view));
    window.session().addMark(innerRange(view));
    window.session().selectMarks({});window.refresh();
    std::unique_ptr<QMenu> menu(plot->createContextMenu(pointAt(plot,view,.35,.5)));
    auto* toggle=menu->findChild<QAction*>("contextMarkToggle");
    QVERIFY(toggle&&toggle->isCheckable());
    toggle->trigger();
    QVERIFY(plot->isCreating());
    toggle->trigger();
    QVERIFY(!plot->isCreating());
    QAction* covered=nullptr;
    for (auto* action : menu->findChildren<QAction*>()) if (action->data().toString()==QString::fromStdString(lower)) covered=action;
    QVERIFY2(covered,"The context menu must expose a covered lower mark at the same point.");
    covered->trigger();
    QVERIFY(window.session().activeFile()->activeMarkId==lower);
    QVERIFY(window.session().activeFile()->view==view);
    std::unique_ptr<QMenu> selectedMenu(plot->createContextMenu(pointAt(plot,view,.35,.5)));
    auto* locate=selectedMenu->findChild<QAction*>("contextLocateAction");
    auto* remove=selectedMenu->findChild<QAction*>("contextDeleteAction");
    QVERIFY(locate&&locate->isEnabled());
    QVERIFY(remove&&remove->isEnabled());
    remove->trigger();
    QVERIFY(!findMark(*window.session().activeFile(),lower));
    QVERIFY(window.session().activeFile()->view==view);
}

void UiTests::unselectedMarkClickAndSubthresholdGesture() {
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    const auto view=window.session().activeFile()->view;
    const auto range=innerRange(view);
    const auto id=window.session().addMark(range);
    window.session().selectMarks({});window.refresh();
    const auto center=pointAt(plot,view,.35,.5);
    QTest::mouseClick(plot,Qt::LeftButton,Qt::NoModifier,center);
    QVERIFY(window.session().activeFile()->activeMarkId==id);
    QVERIFY(findMark(*window.session().activeFile(),id)->range==range);
    QVERIFY(window.session().activeFile()->view==view);
    drag(plot,center,center+QPoint(3,2));
    QVERIFY(findMark(*window.session().activeFile(),id)->range==range);
    QVERIFY(!window.session().canBack());
    plot->setCreating(true);
    const auto blank=pointAt(plot,view,.7,.8);
    drag(plot,blank,blank+QPoint(7,7));
    QCOMPARE(window.session().activeFile()->marks.size(),std::size_t{1});
    QVERIFY(plot->isCreating());
    QVERIFY(window.session().activeFile()->view==view);
}

void UiTests::clippedMarkBorderMovesWithoutInventingHandle() {
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    auto* file=window.session().activeFile();
    auto view=file->view;
    const auto visibleSpan=view.time.end-view.time.begin;
    QVERIFY(window.session().setView({{file->metadata.sampleCount/2,file->metadata.sampleCount/2+visibleSpan},view.frequency},false));
    window.refresh();
    view=file->view;
    const auto span=view.time.end-view.time.begin;
    const ViewRange range{{view.time.begin-span/5,view.time.begin+span/4},
                          {frequencyAt(view,.35),frequencyAt(view,.65)}};
    const auto id=window.session().addMark(range);
    window.refresh();
    const QPoint start(qRound(plot->plotRect().left()+2),qRound(plot->plotRect().center().y()));
    drag(plot,start,start+QPoint(28,0));
    const auto edited=findMark(*file,id)->range;
    QVERIFY(!(edited==range));
    QCOMPARE(edited.time.end-edited.time.begin,range.time.end-range.time.begin);
    QVERIFY(edited.frequency==range.frequency);
    QVERIFY(file->view==view);
}

void UiTests::captureLossAndDeactivationRollback() {
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    const auto view=window.session().activeFile()->view;
    const auto start=pointAt(plot,view,.2,.3);
    const auto end=pointAt(plot,view,.45,.7);
    plot->setCreating(true);
    QTest::mousePress(plot,Qt::LeftButton,Qt::NoModifier,start);
    QTest::mouseMove(plot,end,10);
    QEvent captureLost(QEvent::UngrabMouse);
    QCoreApplication::sendEvent(plot,&captureLost);
    QTest::mouseRelease(plot,Qt::LeftButton,Qt::NoModifier,end);
    QVERIFY(window.session().activeFile()->marks.empty());
    QVERIFY(plot->isCreating());
    QTest::mousePress(plot,Qt::LeftButton,Qt::NoModifier,start);
    QTest::mouseMove(plot,end,10);
    QEvent deactivated(QEvent::WindowDeactivate);
    QCoreApplication::sendEvent(&window,&deactivated);
    QTest::mouseRelease(plot,Qt::LeftButton,Qt::NoModifier,end);
    QVERIFY(window.session().activeFile()->marks.empty());
    QVERIFY(!plot->isCreating());
    QVERIFY(window.session().activeFile()->view==view);
    QVERIFY(!window.session().canBack());
}

void UiTests::resetRestoresCompleteViewSnapshot() {
    DemoMainWindow window;
    showWindow(window);
    auto* file=window.session().activeFile();
    const auto original=file->view;
    auto custom=original;
    custom.time={sampleAt(original,.1L),sampleAt(original,.9L)};
    window.session().setView(custom,false);
    window.session().setAuxiliaryRange(-30,30,false);
    window.session().setAuxiliaryMode(AuxiliaryMode::Psd);
    window.session().setAuxiliaryRange(-130,-20,false);
    const auto before=window.session().snapshot();
    window.refresh();
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(plot);
    std::unique_ptr<QMenu> menu(plot->createContextMenu(plot->plotRect().center().toPoint()));
    auto* reset=menu->findChild<QAction*>("contextResetAction");
    QVERIFY(reset);reset->trigger();
    QVERIFY(file->view==original);
    QCOMPARE(file->display.waveformMin,-32768.0);
    QCOMPARE(file->display.waveformMax,32768.0);
    QCOMPARE(file->display.psdMin,-100.0);
    QCOMPARE(file->display.psdMax,0.0);
    QVERIFY(window.session().back());
    QVERIFY(window.session().snapshot()==before);
}

void UiTests::displayOnlyChangesPreserveBusinessViewAndPowerCache() {
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    auto* palette=window.findChild<QComboBox*>("palette");
    auto* relative=window.findChild<QComboBox*>("freqMode");
    auto* scale=window.findChild<QCheckBox*>("colorbarToggle");
    QVERIFY(plot&&palette&&relative&&scale);
    QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(plot->isDisplaySettled(),5000);
    const auto initialSamples=plot->powerGenerationCount();
    QVERIFY(initialSamples>0);
    const auto view=window.session().activeFile()->view;
    palette->setCurrentIndex(1);
    QCoreApplication::processEvents();
    QCOMPARE(plot->powerGenerationCount(),initialSamples);
    QTest::mouseMove(plot,plot->plotRect().center().toPoint(),10);
    QCoreApplication::processEvents();
    QCOMPARE(plot->powerGenerationCount(),initialSamples);
    relative->setCurrentIndex(1);
    QCoreApplication::processEvents();
    QVERIFY(!window.session().activeFile()->display.absoluteFrequency);
    QVERIFY(window.session().activeFile()->view==view);
    const auto widthWithoutScale=plot->plotRect().width();
    scale->setChecked(true);
    QVERIFY(plot->plotRect().width()<widthWithoutScale);
    QVERIFY(window.session().activeFile()->view==view);
    wheelAt(plot,plot->plotRect().center());
    QCoreApplication::processEvents();
    QVERIFY(plot->powerGenerationCount()>initialSamples);
}

void UiTests::renameUsesPlainTextAndEightyCharacterLimit() {
    DemoMainWindow window;
    showWindow(window);
    const auto id=window.session().addMark(innerRange(window.session().activeFile()->view));
    window.refresh();
    auto* rename=window.findChild<QAbstractButton*>("renameMark");
    QVERIFY(rename);
    const QString typed="<b>"+QString(96,'x')+"</b>";
    QTimer accept;
    connect(&accept,&QTimer::timeout,[typed] {
        if(auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            dialog->setTextValue(typed);dialog->accept();
        }
    });
    accept.start(5);rename->click();accept.stop();
    const auto renamed=QString::fromStdString(findMark(*window.session().activeFile(),id)->name);
    QCOMPARE(renamed,typed.left(80));
    auto* tree=window.findChild<QTreeWidget*>("projectTree");
    QVERIFY(tree);
    const auto nodes=descendantsOfType(tree->topLevelItem(0),"mark");
    QCOMPARE(nodes.size(),1);
    QVERIFY(nodes.front()->text(0).contains(renamed));
    QTimer reject;
    connect(&reject,&QTimer::timeout,[]{if(auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))dialog->reject();});
    reject.start(5);rename->click();reject.stop();
    QCOMPARE(QString::fromStdString(findMark(*window.session().activeFile(),id)->name),renamed);
}

void UiTests::splitterKeyboardResetAndEscapeRollback() {
    DemoMainWindow window;
    showWindow(window);
    auto* splitter=window.findChild<QSplitter*>("graphSplitter");
    auto* handle=window.findChild<QWidget*>("navSplit");
    auto* auxiliary=window.findChild<QWidget*>("auxSplit");
    auto* main=window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(splitter&&handle&&auxiliary&&main);
    const auto initial=splitter->sizes();
    handle->setFocus();
    QTest::keyClick(handle,Qt::Key_Down);
    QCOMPARE(splitter->sizes()[0],initial[0]+10);
    QTest::keyClick(handle,Qt::Key_Up);
    QCOMPARE(splitter->sizes(),initial);
    const auto start=handle->rect().center();
    QTest::mousePress(handle,Qt::LeftButton,Qt::NoModifier,start);
    QTest::mouseMove(handle,start+QPoint(0,-12),10);
    QVERIFY(splitter->sizes()!=initial);
    QTest::keyClick(handle,Qt::Key_Escape);
    QTest::mouseRelease(handle,Qt::LeftButton,Qt::NoModifier,start+QPoint(0,-12));
    QCOMPARE(splitter->sizes(),initial);
    QTest::keyClick(auxiliary,Qt::Key_Up);
    QVERIFY(splitter->sizes()!=initial);
    QTest::mouseDClick(auxiliary,Qt::LeftButton,Qt::NoModifier,auxiliary->rect().center());
    QCoreApplication::processEvents();
    QCOMPARE(splitter->sizes()[0],82);
    QCOMPARE(splitter->sizes()[1],205);
    QVERIFY(main->plotRect().height()>=875);
}

void UiTests::treeToPlotShiftSelectionPreservesAnchor() {
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    auto* tree=window.findChild<QTreeWidget*>("projectTree");
    QVERIFY(plot&&tree);
    const auto view=window.session().activeFile()->view;
    std::vector<std::string> ids;
    for(int i=0;i<4;++i) {
        const long double start=.1L+.2L*i;
        ids.push_back(window.session().addMark({{sampleAt(view,start),sampleAt(view,start+.1L)},
                                               {frequencyAt(view,.35),frequencyAt(view,.65)}}));
    }
    window.refresh();QCoreApplication::processEvents();
    const auto nodes=descendantsOfType(tree->topLevelItem(0),"mark");
    QCOMPARE(nodes.size(),4);
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualItemRect(nodes[1]).center());
    QCOMPARE(window.session().activeFile()->selectedMarkIds.size(),std::size_t{1});
    QTest::mouseClick(plot,Qt::LeftButton,Qt::ShiftModifier,pointAt(plot,view,.75,.5));
    const auto& selected=window.session().activeFile()->selectedMarkIds;
    QCOMPARE(selected.size(),std::size_t{3});
    for(int i=1;i<4;++i) QVERIFY(std::find(selected.begin(),selected.end(),ids[i])!=selected.end());
    QVERIFY(window.session().activeFile()->activeMarkId==ids[3]);
    QVERIFY(window.session().activeFile()->view==view);
}

void UiTests::changingPaletteCancelsAuxiliaryGestureWithoutOverwritingY() {
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("auxPlot");
    auto* palette=window.findChild<QComboBox*>("palette");
    QVERIFY(plot&&palette);
    window.session().setAuxiliaryMode(AuxiliaryMode::Waveform);
    QVERIFY(window.session().setAuxiliaryRange(-30,30,false));
    window.refresh();QCoreApplication::processEvents();
    const auto before=window.session().snapshot();
    const QPoint start(qRound(plot->plotRect().left()-20),qRound(plot->plotRect().center().y()));
    QTest::mousePress(plot,Qt::LeftButton,Qt::NoModifier,start);
    QTest::mouseMove(plot,start+QPoint(0,20),10);
    QVERIFY(!(window.session().snapshot()==before));
    palette->setCurrentIndex(1);
    QTest::mouseRelease(plot,Qt::LeftButton,Qt::NoModifier,start+QPoint(0,20));
    QVERIFY(window.session().snapshot()==before);
    QCOMPARE(static_cast<int>(window.session().activeFile()->display.palette),static_cast<int>(Palette::Viridis));
    QCOMPARE(palette->currentIndex(),1);
    QVERIFY(!window.session().canBack());
}

void UiTests::creationModeSurvivesMaximize() {
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    auto* button=window.findChild<QAbstractButton*>("specMaximize");
    QVERIFY(plot&&button);
    plot->setCreating(true);
    QTest::mouseClick(button,Qt::LeftButton);QCoreApplication::processEvents();
    QVERIFY(plot->isCreating());
    const auto view=window.session().activeFile()->view;
    drag(plot,pointAt(plot,view,.2,.3),pointAt(plot,view,.4,.7));
    QCOMPARE(window.session().activeFile()->marks.size(),std::size_t{1});
    QVERIFY(plot->isCreating());
    QTest::mouseClick(button,Qt::LeftButton);QCoreApplication::processEvents();
    QVERIFY(plot->isCreating());
    QVERIFY(window.session().activeFile()->view==view);
}

void UiTests::escapeCancelsGestureBeforeLeavingMaximize() {
    DemoMainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("mainPlot");
    auto* button=window.findChild<QAbstractButton*>("specMaximize");
    auto* host=window.findChild<QWidget*>("maximizeHost");
    QVERIFY(plot&&button&&host);
    QTest::mouseClick(button,Qt::LeftButton);QCoreApplication::processEvents();
    QVERIFY(host->isVisible());
    const auto view=window.session().activeFile()->view;
    const auto start=pointAt(plot,view,.2,.3);
    const auto end=pointAt(plot,view,.5,.7);
    QTest::mousePress(plot,Qt::LeftButton,Qt::NoModifier,start);
    QTest::mouseMove(plot,end,10);
    QTest::keyClick(plot,Qt::Key_Escape);
    QTest::mouseRelease(plot,Qt::LeftButton,Qt::NoModifier,end);
    QVERIFY(host->isVisible());
    QVERIFY(window.session().activeFile()->view==view);
    QVERIFY(!window.session().canBack());
    QTest::keyClick(plot,Qt::Key_Escape);QCoreApplication::processEvents();
    QVERIFY(!host->isVisible());
}

void UiTests::importFloatingUnitsAndFailureRecovery(){
    QTemporaryDir temporary;MainWindow window;showWindow(window);QString error;
    const auto path=temporary.filePath("CF32_FS1Msps_FC0Hz.raw");SampleFormat format;format.componentEncoding=ComponentEncoding::Float32;format.normalizeIntegerAdc=false;
    {QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.write(fixtures::generate(format,64))>0);}
    SignalImportDialog dialog("units",&window);dialog.show();QVERIFY(dialog.addPath(path));
    dialog.commitSource=[&](FileState source,QString& e){return window.addImportedSource(std::move(source),e);};dialog.startImport();
    auto* finish=dialog.findChild<QPushButton*>("importProgressFinish");QTRY_VERIFY_WITH_TIMEOUT(finish->isEnabled(),15000);finish->click();
    auto* waveform=window.findChild<QComboBox*>("waveformMode");auto* reference=window.findChild<QComboBox*>("reference");
    QVERIFY(waveform->itemText(0).contains("原始幅度"));QVERIFY(reference->currentText().contains("dB(unit^2/Hz)"));
    reference->lineEdit()->setFocus();reference->lineEdit()->selectAll();QTest::keyClicks(reference->lineEdit(),"-42 dB(unit^2/Hz)");QTest::keyClick(reference->lineEdit(),Qt::Key_Return);
    QCOMPARE(window.session().activeFile()->display.referenceLevelDb,-42.0);
    const auto bad=temporary.filePath("CI16_FS1Msps_FC0Hz.raw");{QFile file(bad);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.write("odd")==3);}
    SignalImportDialog failure("recovery",&window);failure.show();QVERIFY(failure.addPath(bad));failure.startImport();
    auto* back=failure.findChild<QPushButton*>("importProgressBack");QTRY_VERIFY_WITH_TIMEOUT(back->isVisible(),15000);back->click();QVERIFY(!failure.findChild<QWidget*>("importProgressOverlay")->isVisible());
    {QFile file(bad);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.write(fixtures::generate({}))>0);}
    failure.startImport();QTRY_VERIFY_WITH_TIMEOUT(failure.findChild<QPushButton*>("importProgressFinish")->isEnabled(),15000);QCOMPARE(window.session().project().files.size(),std::size_t{1});
    failure.findChild<QPushButton*>("importProgressFinish")->click();QCOMPARE(window.session().project().files.size(),std::size_t{1});
}
void UiTests::importTemplatesBatchAndSigmf(){
    QTemporaryDir temporary;QString error;
    const auto data=temporary.filePath("CI16_FS2Msps_FC10MHz.sigmf-data");const auto meta=temporary.filePath("CI16_FS2Msps_FC10MHz.sigmf-meta");
    QVERIFY(QFile::copy(QStringLiteral(SS_SOURCE_DIR "/tests/fixtures/IQ0_FS1Msps_BW800kHz_FC100MHz.dat"),data));
    {QFile file(meta);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.write(R"({"global":{"core:version":"1.2.6","core:datatype":"ci16_le","core:sample_rate":1000000},"captures":[{"core:sample_start":0,"core:frequency":0}],"annotations":[]})")>0);}
    QSettings templateSettings(temporary.filePath("templates.ini"),QSettings::IniFormat);
    MainWindow window;showWindow(window);SignalImportDialog dialog("test",&window,&templateSettings);dialog.show();QVERIFY(dialog.addPath(meta));
    QCOMPARE(dialog.controller().rows()[0].metadata.sampleRateHz,1e6);QCOMPARE(dialog.controller().rows()[0].metadata.centerFrequencyHz,0.0);QVERIFY(dialog.controller().rows()[0].provenance.contains("冲突"));QVERIFY(dialog.controller().rows()[0].confirmed);
    auto* fc=dialog.findChild<AdaptiveValueEdit*>("importCenterFrequency");fc->setFocus();fc->selectAll();QTest::keyClicks(fc,"2.450000125 GHz");QTest::keyClick(fc,Qt::Key_Return);QCOMPARE(dialog.controller().rows()[0].metadata.centerFrequencyHz,2450000125.0);
    QVERIFY2(dialog.saveTemplate("UI完整格式",error),qPrintable(error));const auto templates=temporary.filePath("templates.json");QVERIFY(dialog.exportTemplates(templates,error));QVERIFY(!dialog.exportTemplates(data,error));QVERIFY(dialog.importTemplates(templates,error));
    const auto real=QStringLiteral(SS_SOURCE_DIR "/tests/fixtures/real_ADC_FS1Msps_FC0Hz_RI16.raw");QVERIFY(dialog.addPath(real));QVERIFY(!dialog.addPath(real));QCOMPARE(dialog.controller().rows().size(),std::size_t{2});
    dialog.setPage(1);auto* table=dialog.findChild<QTableWidget*>("importBatchTable");QVERIFY(table);table->selectRow(1);
    dialog.findChild<QComboBox*>("importBatchTemplate")->setCurrentIndex(4);dialog.findChild<QPushButton*>("batchApplyTemplate")->click();QCOMPARE(dialog.controller().rows()[1].metadata.sampleFormat.structure,SampleStructure::Real);QCOMPARE(dialog.controller().rows()[1].metadata.sampleRateHz,1e6);QCOMPARE(dialog.controller().rows()[0].metadata.centerFrequencyHz,2450000125.0);
    std::size_t committed=0;dialog.commitSource=[&](FileState file,QString& e){++committed;return window.addImportedSource(std::move(file),e);};table->clearSelection();dialog.startImport();
    auto* finish=dialog.findChild<QPushButton*>("importProgressFinish");QTRY_VERIFY_WITH_TIMEOUT(finish->isEnabled(),15000);finish->click();QCOMPARE(committed,std::size_t{2});QCOMPARE(window.session().project().files.size(),std::size_t{2});
    for(int i=0;i<5;++i){auto quick=std::make_unique<SignalImportDialog>("cancel",&window);quick->show();QVERIFY(quick->addPath(real));QVERIFY(quick->addPath(data));quick.reset();QCoreApplication::processEvents();}
}
void UiTests::brandResourcesAndAbout(){
    QVERIFY(!BrandAssets::windowIcon().isNull());
    for(const auto& kind:QStringList{"icon","wordmark","lockup"}) {
        for(const auto& variant:QStringList{"primary","dark","light","mono-dark","mono-light"}) {
            QFile file(QString(":/branding/%1-%2.svg").arg(kind,variant));QVERIFY(file.open(QIODevice::ReadOnly));
            const auto xml=file.readAll();QVERIFY(!xml.contains("<script"));QVERIFY(!xml.contains("<image"));QVERIFY(!xml.contains("<text"));
            const auto pixmap=BrandAssets::pixmap(kind,QSize(120,40),1.5,variant);QVERIFY(!pixmap.isNull());QCOMPARE(pixmap.size(),QSize(180,60));
        }
        const auto fallback=BrandAssets::pixmap(kind,QSize(120,40),1.5,"dark",true);QVERIFY(!fallback.isNull());
        const auto image=fallback.toImage();int visible=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y).alpha()>0)++visible;QVERIFY(visible>20);
    }
    MainWindow window;window.show();QVERIFY(!window.windowIcon().isNull());
    QVERIFY(window.findChild<QLabel*>("welcomeBrand"));QVERIFY(window.findChild<QLabel*>("titleBrandWordmark"));
    window.findChild<QAction*>("aboutSignalStudioAction")->trigger();
    auto* about=window.findChild<QDialog*>("aboutSignalStudioDialog");QVERIFY(about);QVERIFY(about->isVisible());
    about->findChild<QPushButton*>("aboutCopyBuild")->click();QCOMPARE(qApp->clipboard()->text(),BrandAssets::buildInformation());
    about->reject();
}
void UiTests::importFormatAndAdaptiveDraft(){
    QCOMPARE(*parseAdaptiveValue("2450000125",UnitKind::Frequency),2450000125.0);
    QCOMPARE(*parseAdaptiveValue("2450.000125 MHz",UnitKind::Frequency),2450000125.0);
    QCOMPARE(*parseAdaptiveValue("2.450000125 GHz",UnitKind::Frequency),2450000125.0);
    QCOMPARE(*parseAdaptiveValue("102.4 MS/s",UnitKind::SampleRate),102400000.0);
    QVERIFY(!parseAdaptiveValue("102 MHz",UnitKind::SampleRate));
    QCOMPARE(*parseAdaptiveValue(formatAdaptiveValue(102400000.125,UnitKind::SampleRate),UnitKind::SampleRate),102400000.125);
    MainWindow window;showWindow(window);SignalImportDialog dialog("test",&window);dialog.show();
    const auto path=QStringLiteral(SS_SOURCE_DIR "/tests/fixtures/IQ0_FS1Msps_BW800kHz_FC100MHz.dat");QVERIFY(dialog.addPath(path));
    auto* fc=dialog.findChild<AdaptiveValueEdit*>("importCenterFrequency");QVERIFY(fc);const auto original=fc->value();fc->setFocus();fc->selectAll();QTest::keyClicks(fc,"2.450000125 GHz");const auto draft=fc->text();const auto cursor=fc->cursorPosition();QCoreApplication::processEvents();QCOMPARE(fc->text(),draft);QCOMPARE(fc->cursorPosition(),cursor);QCOMPARE(fc->value(),original);QTest::keyClick(fc,Qt::Key_Return);QCOMPARE(fc->value(),2450000125.0);
    fc->selectAll();QTest::keyClicks(fc,"bad");QTest::keyClick(fc,Qt::Key_Return);QCOMPARE(fc->value(),2450000125.0);
    auto* structure=dialog.findChild<QComboBox*>("importStructure");auto* encoding=dialog.findChild<QComboBox*>("importEncoding");auto* order=dialog.findChild<QComboBox*>("importByteOrder");auto* layout=dialog.findChild<QComboBox*>("importIQLayout");auto* norm=dialog.findChild<QCheckBox*>("importNormalize");QVERIFY(structure&&encoding&&order&&layout&&norm);
    order->setCurrentIndex(1);layout->setCurrentIndex(1);structure->setCurrentIndex(1);QCOMPARE(dialog.controller().rows()[0].metadata.sampleFormat.iqLayout,IQLayout::NotApplicable);QVERIFY(!layout->isVisible());structure->setCurrentIndex(0);QCOMPARE(dialog.controller().rows()[0].metadata.sampleFormat.iqLayout,IQLayout::QIInterleaved);
    encoding->setCurrentIndex(2);QCOMPARE(dialog.controller().rows()[0].metadata.sampleFormat.byteOrder,ByteOrder::NotApplicable);QVERIFY(!order->isVisible());encoding->setCurrentIndex(1);QCOMPARE(dialog.controller().rows()[0].metadata.sampleFormat.byteOrder,ByteOrder::Big);QVERIFY(!norm->isEnabled());QVERIFY(!dialog.controller().rows()[0].metadata.sampleFormat.normalizeIntegerAdc);
}

void UiTests::importRealFilesAndCancelPrefix(){
    MainWindow window;showWindow(window);
    const auto complex=QStringLiteral(SS_SOURCE_DIR "/tests/fixtures/IQ0_FS1Msps_BW800kHz_FC100MHz.dat");
    const auto real=QStringLiteral(SS_SOURCE_DIR "/tests/fixtures/real_ADC_FS1Msps_FC0Hz_RI16.raw");
    {SignalImportDialog dialog("test",&window);dialog.show();QVERIFY(dialog.addPath(complex));dialog.findChild<QPushButton*>("importCancel")->click();QVERIFY(window.session().project().files.empty());}
    for(const auto& path:QStringList{complex,real}){
        SignalImportDialog dialog("test",&window);dialog.commitSource=[&window](FileState file,QString& error){return window.addImportedSource(std::move(file),error);};dialog.show();QVERIFY(dialog.addPath(path));dialog.findChild<QCheckBox*>("importConfirmFormat")->setChecked(true);
        auto* preview=dialog.findChild<SourcePreviewWidget*>("sourcePreview");QVERIFY(preview);QTRY_VERIFY_WITH_TIMEOUT(preview->ready(),15000);QVERIFY(preview->error().isEmpty());dialog.startImport();QVERIFY(dialog.findChild<QWidget*>("importProgressOverlay")->isVisible());
        auto* finish=dialog.findChild<QPushButton*>("importProgressFinish");QTRY_VERIFY_WITH_TIMEOUT(finish->isVisible()&&finish->isEnabled(),15000);finish->click();QCOMPARE(window.session().activeFile()->metadata.sampleCount,SampleIndex{16384});
        if(path==real){const auto* file=window.session().activeFile();QCOMPARE(file->metadata.sampleFormat.structure,SampleStructure::Real);QCOMPARE(file->metadata.centerFrequencyHz,0.0);QVERIFY((fullRange(file->metadata).frequency==FrequencyRange{0,500000}));QVERIFY(window.session().createChannel("real","none",100000,10000,20000,{0,10000},ChannelFilter::Standard,true,true).empty());}
    }
    QTemporaryDir temp;const auto large=temp.filePath("CI8_FS1Msps_FC0Hz.raw");{QFile file(large);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.resize(128*1024*1024));}
    SignalImportDialog dialog("test",&window);dialog.show();QVERIFY(dialog.addPath(large));dialog.startImport();QTRY_VERIFY_WITH_TIMEOUT(dialog.controller().rows()[0].load.loaded>0,15000);dialog.findChild<QPushButton*>("importProgressStop")->click();QTRY_VERIFY_WITH_TIMEOUT(!dialog.controller().running(),15000);const auto& row=dialog.controller().rows()[0];QVERIFY(row.load.loaded>0&&row.load.loaded<row.metadata.sampleCount);QCOMPARE(row.status,ImportStatus::Partial);QVERIFY(row.load.envelope.size()<=2048);QCOMPARE(dialog.findChild<QLabel*>("importProgressTitle")->text(),QString("读入已停止 · 已保留样本前缀"));
}

void UiTests::addIqFileDialogCancelsAndImportsRealInt16Iq() {
    DemoMainWindow window;
    showWindow(window);
    auto* open=window.findChild<QAbstractButton*>("projectAddSignal");
    QVERIFY(open);
    const auto initialCount=window.session().project().files.size();
    open->click();QCoreApplication::processEvents();
    QPointer<QDialog> dialog=window.findChild<QDialog*>("signalImportDialog");
    QVERIFY(dialog&&dialog->isVisible());
    auto* fs=dialog->findChild<AdaptiveValueEdit*>("importSampleRate");
    auto* fc=dialog->findChild<AdaptiveValueEdit*>("importCenterFrequency");
    auto* add=dialog->findChild<QAbstractButton*>("importStart");
    auto* cancel=dialog->findChild<QAbstractButton*>("importCancel");
    QVERIFY(fs&&fc&&add&&cancel);
    QCOMPARE(fs->value(),102'400'000.0);
    QCOMPARE(fc->value(),830'000'000.0);
    QVERIFY(!add->isEnabled());
    cancel->click();
    QTRY_VERIFY(dialog.isNull());
    QCOMPARE(window.session().project().files.size(),initialCount);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path=directory.filePath(QStringLiteral("IQ0_FS1Msps_BW800kHz_FC10MHz.dat"));
    QFile raw(path); QVERIFY(raw.open(QIODevice::WriteOnly));
    QByteArray samples(65'536*4, '\0');
    QCOMPARE(raw.write(samples),static_cast<qint64>(samples.size())); raw.close();
    QString error;
    QVERIFY2(window.addIqFile(path,&error),qPrintable(error));
    QCOMPARE(window.session().project().files.size(),initialCount+1);
    const auto* file=window.session().activeFile();
    QCOMPARE(file->metadata.sampleRateHz,1'000'000.0);
    QCOMPARE(file->metadata.centerFrequencyHz,10'000'000.0);
    QCOMPARE(file->metadata.declaredBandwidthHz,800'000.0);
    QCOMPARE(file->metadata.sampleCount,SampleIndex{65'536});
    QVERIFY(!file->metadata.demo);
    QVERIFY(file->display.waveformAutoFit);
    QVERIFY(std::isfinite(file->display.waveformMin));
    QVERIFY(std::isfinite(file->display.waveformMax));
    QVERIFY(file->display.waveformMax>file->display.waveformMin);
    QVERIFY(file->marks.empty()&&file->channels.empty());

    auto* main=window.findChild<PlotWidget*>("mainPlot");
    auto* auxiliary=window.findChild<PlotWidget*>("auxPlot");
    QVERIFY(main&&auxiliary);
    QTRY_VERIFY_WITH_TIMEOUT(main->isDisplaySettled()&&auxiliary->isDisplaySettled(),10'000);
    QVERIFY(main->drawnPointCount()>0&&auxiliary->sourcePointCount()>0);
    const auto zoomed=file->view;
    auto next=zoomed; next.time={16'384,49'152};
    QVERIFY(window.session().setView(next));
    window.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(main->isDisplaySettled(),10'000);
    QVERIFY(main->drawnPointCount()>0);
    auto* mode=window.findChild<QComboBox*>("modeAux"); QVERIFY(mode);
    mode->setCurrentIndex(1); QCoreApplication::processEvents();
    QTRY_VERIFY_WITH_TIMEOUT(auxiliary->isDisplaySettled(),10'000);
    QCOMPARE(window.session().activeFile()->display.auxiliaryMode,AuxiliaryMode::Psd);
    QVERIFY(auxiliary->sourcePointCount()>0);
}

void UiTests::narrowbandDemoResourceAndFourPages() {
    MainWindow window;
    showWindow(window);
    window.openNarrowbandDemoProject();

    const auto* file = window.session().activeFile();
    const auto* channel = window.session().activeChannel();
    QVERIFY(file && channel);
    QCOMPARE(file->metadata.path, std::string(":/signalstudio/demo/narrowband_demo.iq"));
    QCOMPARE(file->metadata.sampleCount, SampleIndex{262'144});
    QVERIFY(!file->metadata.demo);
    QCOMPARE(channel->processingState, ChannelProcessingState::Ready);
    const TimeRange expectedSourceTime{0, 196'608};
    QCOMPARE(channel->sourceTime, expectedSourceTime);
    QCOMPARE(file->marks.size(), std::size_t{2});

    auto* workspace = window.findChild<QWidget*>("narrowbandWorkspace");
    auto* stack = window.findChild<QStackedWidget*>("narrowbandPageStack");
    auto* status = window.findChild<QLabel*>("narrowbandDataStatus");
    auto* eyeComponent = window.findChild<QComboBox*>("eyeComponent");
    auto* eyePeriods = window.findChild<QComboBox*>("eyePeriods");
    auto* eyeTraces = window.findChild<QComboBox*>("eyeTraces");
    auto* frequencyMode = window.findChild<QComboBox*>("channelFrequencyMode");
    auto* waveformMode = window.findChild<QComboBox*>("narrowbandWaveformMode");
    auto* waveformChart = window.findChild<QWidget*>("narrowbandWaveformPanelChart");
    auto* stftChart = window.findChild<QWidget*>("narrowbandStftPanelChart");
    auto* palette = window.findChild<QComboBox*>("colormap");
    auto* channelProperties = window.findChild<QWidget*>("narrowbandChannelSection");
    auto* grid = window.findChild<QCheckBox*>("narrowbandGrid");
    QVERIFY(workspace && workspace->isVisible() && eyeComponent && eyePeriods && eyeTraces && frequencyMode && waveformMode && waveformChart && stftChart && palette);
    QVERIFY(stack && status && channelProperties && channelProperties->isVisible() && grid);
    QVERIFY(!window.findChild<QWidget*>("narrowbandNavigation"));
    QVERIFY(window.findChild<QWidget*>("narrowbandChartToolbar"));
    QCOMPARE(waveformMode->count(), 4);
    QCOMPARE(waveformMode->itemText(3), QStringLiteral("幅度包络"));
    QCOMPARE(window.findChild<QLabel*>("narrowbandCenter")->text(), QStringLiteral("2511.2 MHz"));
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("真实 DDC")), 15'000);
    const int originalPalette = palette->currentIndex();
    QCOMPARE(stftChart->property("paletteIndex").toInt(), originalPalette);
    const int nextPalette = originalPalette == static_cast<int>(Palette::Gray)
        ? static_cast<int>(Palette::Turbo) : static_cast<int>(Palette::Gray);
    palette->setCurrentIndex(nextPalette);
    QCOMPARE(file->display.palette, static_cast<Palette>(nextPalette));
    QCOMPARE(stftChart->property("paletteIndex").toInt(), nextPalette);
    auto* modulationStftChart = window.findChild<QWidget*>("narrowbandModulationStftChart");
    auto* recognitionStftChart = window.findChild<QWidget*>("recognitionStftPanelChart");
    QVERIFY(modulationStftChart && recognitionStftChart);
    QCOMPARE(modulationStftChart->property("paletteIndex").toInt(), nextPalette);
    QCOMPARE(recognitionStftChart->property("paletteIndex").toInt(), nextPalette);
    QTRY_VERIFY_WITH_TIMEOUT(waveformChart->property("fitDataVerticalFraction").toDouble() > 0.0, 15'000);
    QVERIFY(std::abs(waveformChart->property("fitDataVerticalFraction").toDouble() - .75) <= .03);
    QVERIFY(std::abs(waveformChart->property("fitDataCenterOffsetFraction").toDouble()) <= .03);
    const std::array<const char*, 8> chartNames{"narrowbandWaveformPanelChart", "narrowbandPsdPanelChart",
        "narrowbandStftPanelChart", "narrowbandConstellationPreviewChart", "narrowbandConstellationLargeChart",
        "narrowbandEyeLargeChart", "recognitionTimelinePanelChart", "demodConstellationPanelChart"};
    for (const auto* name : chartNames) {
        auto* chart = window.findChild<QWidget*>(QString::fromLatin1(name));
        QVERIFY2(chart, name);
        QVERIFY2(!chart->property("xAxisLabel").toString().isEmpty(), name);
        QVERIFY2(!chart->property("yAxisLabel").toString().isEmpty(), name);
        QVERIFY(chart->property("gridVisible").toBool());
    }
    grid->setChecked(false);
    QCoreApplication::processEvents();
    for (const auto* name : chartNames)
        QVERIFY(!window.findChild<QWidget*>(QString::fromLatin1(name))->property("gridVisible").toBool());
    grid->setChecked(true);
    const auto fullVisibleTime = window.session().activeChannel()->visibleSourceTime;
    const auto frequencyView = window.session().activeChannel()->visibleBasebandFrequency;
    QVERIFY(window.session().setChannelView({fullVisibleTime.begin, fullVisibleTime.begin + 512}, frequencyView, false));
    window.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(waveformChart->property("samplePointsVisible").toBool(), 15'000);
    QVERIFY(waveformChart->property("samplePointCount").toULongLong() > 1);
    QVERIFY(window.session().setChannelView(fullVisibleTime, frequencyView, false));
    window.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!waveformChart->property("samplePointsVisible").toBool(), 15'000);
    waveformMode->setCurrentIndex(3);
    QCOMPARE(window.session().activeChannel()->waveform, NarrowbandWaveform::Envelope);
    QTRY_VERIFY_WITH_TIMEOUT(waveformChart->property("fitDataVerticalFraction").toDouble() > 0.0, 15'000);
    QVERIFY(std::abs(waveformChart->property("fitDataVerticalFraction").toDouble() - .75) <= .03);
    const QPoint yAxisPoint(30, qRound(waveformChart->height() / 2.0));
    const auto globalYAxisPoint = waveformChart->mapToGlobal(yAxisPoint);
    QWheelEvent amplitudeWheel(QPointF(yAxisPoint), QPointF(globalYAxisPoint), QPoint(), QPoint(0, 120),
                               Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(waveformChart, &amplitudeWheel);
    QVERIFY(!window.session().activeChannel()->waveformAutoScale);
    waveformMode->setCurrentIndex(1);
    QVERIFY(window.session().activeChannel()->waveformAutoScale);
    QTRY_VERIFY_WITH_TIMEOUT(waveformChart->property("fitDataVerticalFraction").toDouble() > 0.0, 15'000);

    const std::array<const char*, 4> buttons{"observePage", "modulationPage", "deepLearningPage", "demodulationPage"};
    for (int index = 0; index < 4; ++index) {
        auto* button = window.findChild<QPushButton*>(QString::fromLatin1(buttons[static_cast<std::size_t>(index)]));
        QVERIFY(button && button->isEnabled());
        button->click();
        QCoreApplication::processEvents();
        QCOMPARE(stack->currentIndex(), index);
        QCOMPARE(static_cast<int>(window.session().activeChannel()->page), index);
        if (index < 3) {
            const char* heatName = index == 0 ? "narrowbandStftPanelChart" :
                index == 1 ? "narrowbandModulationStftChart" : "recognitionStftPanelChart";
            auto* heat = window.findChild<QWidget*>(QString::fromLatin1(heatName));
            auto* surface = heat->findChild<AcceleratedSurface*>();
            QVERIFY(surface);
            const QRectF expected(66, 14, std::max(1, heat->width() - 82), std::max(1, heat->height() - 55));
            QTRY_COMPARE(surface->heatmapTargetRect(), expected);
        }
    }
    QVERIFY(window.session().activeChannel()->waveformAutoScale);
    QCOMPARE(window.session().activeChannel()->waveform, NarrowbandWaveform::Magnitude);
    eyeComponent->setCurrentIndex(2); eyePeriods->setCurrentIndex(2); eyeTraces->setCurrentIndex(2);
    frequencyMode->setCurrentIndex(1);
    QVERIFY(window.session().activeChannel()->absoluteFrequencyLabels);
    QCOMPARE(window.session().activeChannel()->eyeComponent, 2);
    QCOMPARE(window.session().activeChannel()->eyePeriods, 3);
    QCOMPARE(window.session().activeChannel()->eyeTraces, 128);
    auto* run = window.findChild<QPushButton*>("runRecognition");
    auto* stop = window.findChild<QPushButton*>("stopRecognition");
    auto* statusText = window.findChild<QLabel*>("recognitionStatus");
    QVERIFY(run && stop && statusText);
    run->click();
    QVERIFY(stop->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(statusText->text().contains(QStringLiteral("完成")), 5'000);
    auto* top5 = window.findChild<QLabel*>("recognitionTop5");
    auto* threshold = window.findChild<QDoubleSpinBox*>("recognitionThreshold");
    QVERIFY(top5 && threshold);
    QVERIFY(top5->text().contains(QStringLiteral("片段 1 / 18")));
    threshold->setValue(.95);
    QVERIFY(top5->text().contains(QStringLiteral("未知 / 需复核")));

    QTemporaryDir projectDirectory;
    QVERIFY(projectDirectory.isValid());
    const auto projectPath = projectDirectory.filePath(QStringLiteral("narrowband-project.json"));
    QVERIFY(window.saveProject(projectPath));
    MainWindow reopened;
    QVERIFY(reopened.openProject(projectPath));
    QVERIFY(reopened.session().activeChannel());
    QCOMPARE(reopened.session().activeChannel()->page, NarrowbandPage::Demodulation);
    QCOMPARE(reopened.session().activeChannel()->sourceTime, expectedSourceTime);
    QCOMPARE(reopened.session().activeChannel()->eyeComponent, 2);
    QCOMPARE(reopened.session().activeChannel()->eyePeriods, 3);
    QCOMPARE(reopened.session().activeChannel()->eyeTraces, 128);
    QVERIFY(reopened.session().activeChannel()->absoluteFrequencyLabels);

    const auto channelId = QString::fromStdString(channel->id);
    auto* locate = window.findChild<QPushButton*>("narrowbandLocateSource");
    QVERIFY(locate);
    locate->click();
    QVERIFY(!window.session().project().narrowbandWorkspaceOpen);
    QCOMPARE(window.session().activeFile()->activeMarkId, std::string("demo-mark-signal-01"));
    QVERIFY(!window.session().activeChannel());
    QVERIFY(window.session().activateChannel(channelId.toStdString()));
    window.refresh();
    QVERIFY(window.session().project().narrowbandWorkspaceOpen);
    QCOMPARE(window.session().activeChannel()->page, NarrowbandPage::Demodulation);
}

void UiTests::waveformBandwidthAndVisiblePaneStftSettings() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MainWindow window;
    showWindow(window);
    const auto path = directory.filePath(QStringLiteral("IQ0_FS1Msps_BW800kHz_FC10MHz.dat"));
    QFile raw(path);
    QVERIFY(raw.open(QIODevice::WriteOnly));
    QByteArray samples(131'072 * 4, '\0');
    for (int n = 0; n < 131'072; ++n) {
        qToLittleEndian<qint16>(16384, reinterpret_cast<uchar*>(samples.data()) + n * 4);
        qToLittleEndian<qint16>(-8192, reinterpret_cast<uchar*>(samples.data()) + n * 4 + 2);
    }
    QCOMPARE(raw.write(samples), static_cast<qint64>(samples.size()));
    raw.close();
    QString error;
    QVERIFY2(window.addIqFile(path, &error), qPrintable(error));

    QTRY_VERIFY_WITH_TIMEOUT(window.session().activeFile()->metadata.availability.status==LoadStatus::Ready,10000);
    auto* waveform = window.findChild<QComboBox*>("waveformMode");
    auto* bandwidth = window.findChild<QDoubleSpinBox*>("effectiveBandwidthMHz");
    auto* stft = window.findChild<QComboBox*>("stftFft");
    auto* dynamic = window.findChild<QComboBox*>("dynamic");
    auto* reference = window.findChild<QComboBox*>("reference");
    QVERIFY(waveform && bandwidth && stft && dynamic && reference);
    auto* file = window.session().activeFile();
    QCOMPARE(static_cast<int>(file->display.waveformMode), static_cast<int>(WaveformMode::IqRms));
    QCOMPARE(bandwidth->value(), .8);
    QCOMPARE(fullRange(file->metadata).frequency, (FrequencyRange{9.6e6, 10.4e6}));
    for (const auto* id : {"fileSection", "viewSection", "markSection", "psdSection", "specSection"}) {
        auto* toggle = window.findChild<QToolButton*>(QString(id) + "Toggle");
        QVERIFY(toggle && toggle->isChecked());
    }
    QVERIFY(window.findChild<QAction*>("mouseInteractionHelpAction"));
    QVERIFY(!window.findChild<QWidget*>("helpSection"));

    waveform->setCurrentIndex(static_cast<int>(WaveformMode::I));
    QCOMPARE(static_cast<int>(file->display.waveformMode), static_cast<int>(WaveformMode::I));
    QVERIFY(file->display.waveformAutoFit);
    auto* auxiliary = window.findChild<PlotWidget*>("auxPlot");
    QVERIFY(auxiliary);
    QTRY_VERIFY_WITH_TIMEOUT(auxiliary->isDisplaySettled(), 10'000);
    waveform->setCurrentIndex(static_cast<int>(WaveformMode::Q));
    QCOMPARE(static_cast<int>(file->display.waveformMode), static_cast<int>(WaveformMode::Q));
    QTRY_VERIFY_WITH_TIMEOUT(auxiliary->isDisplaySettled(), 10'000);
    waveform->setCurrentIndex(static_cast<int>(WaveformMode::IqRms));
    QVERIFY(file->display.waveformAutoFit);
    QTRY_VERIFY_WITH_TIMEOUT(auxiliary->isDisplaySettled(), 10'000);

    auto* psd = window.findChild<QComboBox*>("psdFft");
    QVERIFY(psd);
    const auto fullViewTime = file->view.time;
    stft->setCurrentText("256");
    psd->setCurrentText("1024");
    QVERIFY(window.session().setView({{fullViewTime.begin, fullViewTime.begin + 1024}, file->view.frequency}, false));
    window.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(auxiliary->isDisplaySettled(), 10'000);
    QVERIFY(auxiliary->samplePointsVisible());
    QCOMPARE(auxiliary->samplePointCount(), std::size_t{1024});
    QVERIFY(window.session().setView({fullViewTime, file->view.frequency}, false));
    window.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!auxiliary->samplePointsVisible(), 10'000);

    bandwidth->setValue(.5);
    QCOMPARE(file->metadata.effectiveBandwidthHz, 500'000.0);
    QCOMPARE(file->view.frequency, (FrequencyRange{9.75e6, 10.25e6}));
    QCOMPARE(stft->count(), 12);
    QCOMPARE(stft->currentText(), QString("256"));
    psd->setCurrentText("8192");
    QCOMPARE(file->display.psdSize, 8192);
    const auto oldTime = file->view.time;
    QVERIFY(window.session().setView({{oldTime.begin + 20, oldTime.begin + 120}, file->view.frequency}, false));
    window.refresh();
    QCOMPARE(file->view.time.end - file->view.time.begin, SampleIndex{100});

    stft->setCurrentText("65536");
    QCOMPARE(file->display.stftSize, 65'536);
    QCOMPARE(file->view.time.end - file->view.time.begin, SampleIndex{100});

    auto* main = window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(main);
    QTRY_VERIFY_WITH_TIMEOUT(main->isDisplaySettled(), 30'000);
    const auto initialPowerGenerations = main->powerGenerationCount();
    const auto initialColorTransforms = main->renderStatistics().value("colorTransformGenerations").toInt();
    dynamic->setFocus(); dynamic->lineEdit()->selectAll(); QTest::keyClicks(dynamic->lineEdit(), "87.5");
    QTest::keyClick(dynamic->lineEdit(), Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(file->display.dynamicRangeDb - 87.5) < 1e-9, 2'000);
    QTRY_VERIFY_WITH_TIMEOUT(main->renderStatistics().value("colorTransformGenerations").toInt() > initialColorTransforms, 2'000);
    const auto dynamicColorTransforms = main->renderStatistics().value("colorTransformGenerations").toInt();
    reference->setFocus(); reference->lineEdit()->selectAll(); QTest::keyClicks(reference->lineEdit(), "-33.5");
    QTest::keyClick(reference->lineEdit(), Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(file->display.referenceLevelDb + 33.5) < 1e-9, 2'000);
    QTRY_VERIFY_WITH_TIMEOUT(main->renderStatistics().value("colorTransformGenerations").toInt() > dynamicColorTransforms, 2'000);
    QCOMPARE(main->powerGenerationCount(), initialPowerGenerations);

    const auto projectPath = directory.filePath(QStringLiteral("saved-state.json"));
    QVERIFY2(window.saveProject(projectPath), "The project JSON must preserve the analysis snapshot.");
    window.session().newProject(); window.refresh();
    QVERIFY(window.openProject(projectPath));
    file = window.session().activeFile();
    QVERIFY(file);
    QCOMPARE(file->metadata.effectiveBandwidthHz, 500'000.0);
    QCOMPARE(file->display.waveformMode, WaveformMode::IqRms);
    QCOMPARE(file->display.stftSize, 65'536);
    QCOMPARE(file->display.psdSize, 8192);
    QCOMPARE(file->display.dynamicRangeDb, 87.5);
    QCOMPARE(file->display.referenceLevelDb, -33.5);
}

void UiTests::uiStatePersistenceAndRecentProjects() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto oldOrganization = QCoreApplication::organizationName();
    const auto oldApplication = QCoreApplication::applicationName();
    const bool oldPersistence = qApp->property("uiStatePersistenceEnabled").toBool();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setOrganizationName("Signal Studio UI Test");
    QCoreApplication::setApplicationName("SignalStudioPersistenceTest");
    QSettings().clear();
    qApp->setProperty("uiStatePersistenceEnabled", true);
    const auto projectPath = directory.filePath(QStringLiteral("recent-empty-project.json"));
    const auto iqPath = directory.filePath(QStringLiteral("IQ0_FS1Msps_BW800kHz_FC10MHz.dat"));
    const auto otherIqPath = directory.filePath(QStringLiteral("IQ1_FS2Msps_BW1MHz_FC20MHz.dat"));
    QFile raw(iqPath);
    QVERIFY(raw.open(QIODevice::WriteOnly));
    const QByteArray samples(65'536 * 4, '\0');
    QCOMPARE(raw.write(samples), static_cast<qint64>(samples.size()));
    raw.close();
    QFile otherRaw(otherIqPath);
    QVERIFY(otherRaw.open(QIODevice::WriteOnly));
    QCOMPARE(otherRaw.write(samples), static_cast<qint64>(samples.size()));
    otherRaw.close();
    {
        MainWindow window;
        window.resize(1480, 920);
        auto* section = window.findChild<QToolButton*>("specSectionToggle");
        auto* results = window.findChild<QToolButton*>("resultsToggle");
        QVERIFY(section && results);
        section->setChecked(false);
        results->setChecked(true);
        QVERIFY(window.saveProject(projectPath));
        QCOMPARE(window.findChildren<QAction*>("recentProjectAction").size(), 1);
        QString error;
        QVERIFY2(window.addIqFile(iqPath, &error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(window.session().activeFile()->metadata.availability.status!=LoadStatus::Loading,10000);
        auto* palette = window.findChild<QComboBox*>("palette");
        auto* mainMode = window.findChild<QComboBox*>("modeMain");
        auto* auxiliaryMode = window.findChild<QComboBox*>("modeAux");
        auto* waveformMode = window.findChild<QComboBox*>("waveformMode");
        auto* stft = window.findChild<QComboBox*>("stftFft");
        auto* psd = window.findChild<QComboBox*>("psdFft");
        auto* dynamic = window.findChild<QComboBox*>("dynamic");
        auto* reference = window.findChild<QComboBox*>("reference");
        auto* frequency = window.findChild<QComboBox*>("freqMode");
        auto* bandwidth = window.findChild<QDoubleSpinBox*>("effectiveBandwidthMHz");
        auto* grid = window.findChild<QCheckBox*>("gridToggle");
        auto* colorbar = window.findChild<QCheckBox*>("colorbarToggle");
        QVERIFY(palette && mainMode && auxiliaryMode && waveformMode && stft && psd && dynamic && reference && frequency && bandwidth && grid && colorbar);
        waveformMode->setCurrentIndex(static_cast<int>(WaveformMode::Q));
        mainMode->setCurrentIndex(static_cast<int>(MainMode::Waterfall));
        auxiliaryMode->setCurrentIndex(static_cast<int>(AuxiliaryMode::Psd));
        palette->setCurrentIndex(palette->findText(QStringLiteral("CoolEdit Classic")));
        stft->setCurrentText(QStringLiteral("4096"));
        psd->setCurrentText(QStringLiteral("8192"));
        dynamic->setCurrentIndex(dynamic->findText(QStringLiteral("60 dB")));
        reference->setCurrentIndex(reference->findText(QStringLiteral("-40 dBFS/Hz")));
        frequency->setCurrentIndex(1);
        bandwidth->setValue(.5);
        grid->setChecked(false);
        colorbar->setChecked(true);
    }
    {
        MainWindow restored;
        auto* section = restored.findChild<QToolButton*>("specSectionToggle");
        auto* results = restored.findChild<QToolButton*>("resultsToggle");
        QVERIFY(section && results);
        QVERIFY(!section->isChecked());
        QVERIFY(results->isChecked());
        QString error;
        QVERIFY2(restored.addIqFile(iqPath, &error), qPrintable(error));
        const auto* file = restored.session().activeFile();
        QVERIFY(file);
        QCOMPARE(file->display.mainMode, MainMode::Waterfall);
        QCOMPARE(file->display.auxiliaryMode, AuxiliaryMode::Psd);
        QCOMPARE(file->display.waveformMode, WaveformMode::Q);
        QCOMPARE(file->display.palette, Palette::CoolEditClassic);
        QCOMPARE(file->display.stftSize, 4096);
        QCOMPARE(file->display.psdSize, 8192);
        QCOMPARE(file->display.dynamicRangeDb, 60.0);
        QCOMPARE(file->display.referenceLevelDb, -40.0);
        QVERIFY(!file->display.absoluteFrequency);
        QVERIFY(!file->display.grid);
        QVERIFY(file->display.colorScale);
        QCOMPARE(file->metadata.effectiveBandwidthHz, 500'000.0);
        QVERIFY2(restored.addIqFile(otherIqPath, &error), qPrintable(error));
        const auto* secondFile = restored.session().activeFile();
        QVERIFY(secondFile);
        QCOMPARE(secondFile->display.mainMode, MainMode::Waterfall);
        QCOMPARE(secondFile->display.auxiliaryMode, AuxiliaryMode::Psd);
        QCOMPARE(secondFile->display.waveformMode, WaveformMode::Q);
        QCOMPARE(secondFile->display.palette, Palette::CoolEditClassic);
        QCOMPARE(secondFile->display.stftSize, 4096);
        QCOMPARE(secondFile->display.psdSize, 8192);
        QCOMPARE(secondFile->display.dynamicRangeDb, 60.0);
        QCOMPARE(secondFile->display.referenceLevelDb, -40.0);
        QVERIFY(!secondFile->display.absoluteFrequency);
        QVERIFY(!secondFile->display.grid);
        QVERIFY(secondFile->display.colorScale);
        QCOMPARE(secondFile->metadata.effectiveBandwidthHz, 1'000'000.0);
        auto* menu = restored.findChild<QMenu*>("recentProjectsMenu");
        QVERIFY(menu);
        auto* recent = menu->findChild<QAction*>("recentProjectAction");
        QVERIFY(recent && recent->isEnabled());
        recent->trigger();
        QVERIFY(restored.session().project().files.empty());
        QCOMPARE(restored.session().project().name, std::string("射频信号分析工程"));
    }
    QSettings().clear();
    QCoreApplication::setOrganizationName(oldOrganization);
    QCoreApplication::setApplicationName(oldApplication);
    qApp->setProperty("uiStatePersistenceEnabled", oldPersistence);
}

QTEST_MAIN(UiTests)
#include "ui_tests.moc"
