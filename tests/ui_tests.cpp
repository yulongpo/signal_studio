#include "app/main_window.h"
#include "ui/charts/plot_widget.h"

#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QTreeWidget>
#include <QWheelEvent>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace signalstudio;

namespace {

void showWindow(MainWindow& window) {
    window.resize(1366, 768);
    window.show();
    QCoreApplication::processEvents();
    QTest::qWait(20);
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

void drag(PlotWidget* plot, QPoint start, QPoint end) {
    QTest::mousePress(plot, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(plot, end, 10);
    QTest::mouseRelease(plot, Qt::LeftButton, Qt::NoModifier, end);
    QCoreApplication::processEvents();
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

class UiTests : public QObject {
    Q_OBJECT
private slots:
    void workspaceLayout();
    void independentFileDisplaySettings();
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
};

void UiTests::workspaceLayout() {
    MainWindow window;
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
    QVERIFY2(main->plotRect().height() >= 200, "1366x768 must retain at least 200px for the main plot.");
    QCOMPARE(tree->selectionMode(), QAbstractItemView::ExtendedSelection);
    QVERIFY(!resultsToggle->isChecked());
    auto* results = window.findChild<QPlainTextEdit*>();
    QVERIFY(results);
    QVERIFY(!results->isVisible());
    QCOMPARE(window.session().project().files.size(), std::size_t{2});
    QVERIFY(window.session().project().activeFileId == window.session().project().files.front().metadata.id);
}

void UiTests::independentFileDisplaySettings() {
    MainWindow window;
    showWindow(window);
    auto* mainMode = window.findChild<QComboBox*>("modeMain");
    auto* auxiliaryMode = window.findChild<QComboBox*>("modeAux");
    auto* palette = window.findChild<QComboBox*>("palette");
    QVERIFY(mainMode);
    QVERIFY(auxiliaryMode);
    QVERIFY(palette);
    const auto firstId = window.session().project().files[0].metadata.id;
    const auto secondId = window.session().project().files[1].metadata.id;
    mainMode->setCurrentIndex(1);
    auxiliaryMode->setCurrentIndex(1);
    palette->setCurrentIndex(2);
    QCOMPARE(static_cast<int>(window.session().activeFile()->display.mainMode), static_cast<int>(MainMode::Waterfall));
    QCOMPARE(static_cast<int>(window.session().activeFile()->display.auxiliaryMode), static_cast<int>(AuxiliaryMode::Psd));
    QCOMPARE(static_cast<int>(window.session().activeFile()->display.palette), static_cast<int>(Palette::Gray));

    QVERIFY(window.session().activateFile(secondId));
    window.refresh();
    mainMode->setCurrentIndex(0);
    auxiliaryMode->setCurrentIndex(0);
    palette->setCurrentIndex(0);
    QVERIFY(window.session().activateFile(firstId));
    window.refresh();
    QCOMPARE(mainMode->currentIndex(), 1);
    QCOMPARE(auxiliaryMode->currentIndex(), 1);
    QCOMPARE(palette->currentIndex(), 2);
    QVERIFY(window.session().activateFile(secondId));
    window.refresh();
    QCOMPARE(mainMode->currentIndex(), 0);
    QCOMPARE(auxiliaryMode->currentIndex(), 0);
    QCOMPARE(palette->currentIndex(), 0);
}

void UiTests::mainModeCoordinateDirection() {
    MainWindow window;
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
    MainWindow window;
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
    MainWindow window;
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
    MainWindow window;
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
    MainWindow window;
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
    MainWindow window;
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
    MainWindow window;
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
    auto* remove = window.findChild<QAction*>("removeFileAction");
    auto* mode = window.findChild<QComboBox*>("modeMain");
    auto* plot = window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(create);
    QVERIFY(add);
    QVERIFY(remove);
    QVERIFY(mode);
    QVERIFY(plot);
    create->trigger();
    QVERIFY(window.session().project().files.empty());
    QVERIFY(!window.session().activeFile());
    QVERIFY(!remove->isEnabled());
    QVERIFY(!mode->isEnabled());
    QVERIFY(plot->isVisible());
    add->trigger();
    QCOMPARE(window.session().project().files.size(), std::size_t{1});
    QVERIFY(remove->isEnabled());
    QVERIFY(mode->isEnabled());
    remove->trigger();
    QVERIFY(window.session().project().files.empty());
    QVERIFY(!window.session().activeFile());
}

void UiTests::treeSelectionPreservesNodesAndShiftAnchor() {
    MainWindow window;
    showWindow(window);
    auto* tree=window.findChild<QTreeWidget*>("projectTree");
    QVERIFY(tree);
    const auto view=window.session().activeFile()->view;
    window.session().addMark(innerRange(view));
    window.session().addMark(innerRange(view));
    window.session().selectMarks({});
    window.refresh();
    QCoreApplication::processEvents();
    auto* root=tree->topLevelItem(0);
    QVERIFY(root);
    auto* fileNode=root->child(0);
    QCOMPARE(fileNode->childCount(),4);
    const auto originalView=window.session().activeFile()->view;
    auto* first=fileNode->child(0);
    auto* third=fileNode->child(2);
    auto* fourth=fileNode->child(3);
    const auto firstId=first->data(0,Qt::UserRole+1).toString().toStdString();
    const auto thirdId=third->data(0,Qt::UserRole+1).toString().toStdString();
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualItemRect(first).center());
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::ControlModifier,tree->visualItemRect(third).center());
    const auto& selected=window.session().activeFile()->selectedMarkIds;
    QCOMPARE(selected.size(),std::size_t{2});
    QVERIFY(std::find(selected.begin(),selected.end(),firstId)!=selected.end());
    QVERIFY(std::find(selected.begin(),selected.end(),thirdId)!=selected.end());
    QVERIFY(tree->topLevelItem(0)==root&&root->child(0)==fileNode&&fileNode->child(0)==first);
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualItemRect(first).center());
    window.refresh(); // Cursor and parameter refreshes must retain the native range anchor.
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::ShiftModifier,tree->visualItemRect(fourth).center());
    QCOMPARE(window.session().activeFile()->selectedMarkIds.size(),std::size_t{4});
    QVERIFY(window.session().activeFile()->view==originalView);
    QVERIFY(tree->topLevelItem(0)==root&&fileNode->child(3)==fourth);
    const auto firstFileId=window.session().project().activeFileId;
    const auto secondFileId=window.session().project().files[1].metadata.id;
    auto* secondFileNode=root->child(1);
    QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualItemRect(secondFileNode).center());
    QVERIFY(window.session().project().activeFileId==firstFileId);
    QVERIFY(tree->topLevelItem(0)==root); // File activation cannot destroy the dispatching item.
    QCoreApplication::processEvents();
    QTRY_VERIFY(window.session().project().activeFileId==secondFileId);
    QTRY_COMPARE(tree->topLevelItem(0)->child(1)->childCount(),1);
}

void UiTests::displayChangeSurvivesPendingWheelCommit() {
    MainWindow window;
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
    MainWindow window;
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
    MainWindow window;
    showWindow(window);
    auto* plot=window.findChild<PlotWidget*>("auxPlot");
    QVERIFY(plot);
    auto* file=window.session().activeFile();
    file->display.auxiliaryMode=psd?AuxiliaryMode::Psd:AuxiliaryMode::Waveform;
    file->display.auxiliaryMin=psd?-110:-1;
    file->display.auxiliaryMax=psd?-10:1;
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
    QVERIFY(!window.session().canBack());
}

void UiTests::selectedMarkCornerAndEdgeResize_data() {
    QTest::addColumn<bool>("waterfall");
    QTest::newRow("time-frequency")<<false;
    QTest::newRow("waterfall")<<true;
}

void UiTests::selectedMarkCornerAndEdgeResize() {
    QFETCH(bool,waterfall);
    MainWindow window;
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

QTEST_MAIN(UiTests)
#include "ui_tests.moc"
