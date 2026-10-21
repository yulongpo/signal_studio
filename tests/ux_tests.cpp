#include <QtTest>
#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QListView>
#include <QLabel>
#include <QScrollArea>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>

#include "app/main_window.h"
#include "infrastructure/annotation_json.h"
#include "ui/controls/adaptive_value_edit.h"
#include "ui/detection/detection_card.h"
#include "ui/detection/detection_panel.h"
#include "ui/charts/plot_widget.h"

using namespace signalstudio;

namespace {
QString addSource(MainWindow& window, const QString& directory, const QString& name) {
  QFile source(directory + '/' + name);
  if (!source.open(QIODevice::WriteOnly) ||
      source.write(QByteArray(80000, '\0')) != 80000)
    return "Cannot create the IQ source";
  source.close();
  QString issue;
  if (!window.addIqFile(source.fileName(), &issue)) return issue;
  QElapsedTimer timer;
  timer.start();
  while (window.session().activeFile()->metadata.availability.status == LoadStatus::Loading &&
         timer.elapsed() < 10000)
    QTest::qWait(10);
  return window.session().activeFile()->metadata.availability.status == LoadStatus::Ready
             ? QString{} : "IQ source did not finish loading";
}

DetectionRun historyFor(MainWindow& window) {
  const auto& file = *window.session().activeFile();
  DetectionRun run;
  run.lifecycle = DetectionLifecycle::Completed;
  run.request.runId = "ux-history";
  run.request.sourceIdentity = annotationSourceIdentity(file.metadata, window.session().projectGeneration());
  run.request.profile = *findDetectionProfile("ffscn-gui-iq-v1");
  run.request.profile.fftSize = 8192;
  run.request.profile.hopSeconds = .004;
  run.request.profile.modelFrames = 6;
  run.request.profile.modelHopFrames = 3;
  run.request.spectrumMode = DetectionSpectrumMode::Mean;
  run.request.parameters.scoreThreshold = .42;
  run.request.parameters.timeStrategy = DetectionTimeStrategy::Roi;
  run.request.requestedRange = fullRange(file.metadata);
  run.request.requestedRange.time = {100, 1100};
  run.request.rangeOrigin = "visible";
  return run;
}

void expose(MainWindow& window) {
  window.resize(2560, 1440);
  window.show();
  QTest::qWait(30);
  window.refresh();
}

void focusHistory(MainWindow& window, const QString& id) {
  auto* list = window.findChild<QListView*>("detectionRunList");
  for (int row = 0; row < list->model()->rowCount(); ++row) {
    const auto index = list->model()->index(row, 0);
    if (index.data(Qt::UserRole).toString() == id) {
      list->setCurrentIndex(index);
      break;
    }
  }
  if (auto* tab = window.findChild<QAbstractButton*>("taskTab"))
    QTest::mouseClick(tab, Qt::LeftButton);
}
}  // namespace

class UxTests : public QObject {
  Q_OBJECT
 private:
  QTemporaryDir settings_;
 private slots:
  void uxP01_popupDismissal();
  void uxP02_popupChildrenAndGeometry();
  void uxS06_popoverDraftPolicy();
  void uxR01_chartToolbarLayout();
  void uxR02_chartSettingsStayStable();
  void initTestCase() {
    QCoreApplication::setOrganizationName("SignalStudioTests");
    QCoreApplication::setApplicationName("WidebandUx");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    QApplication::setFont(QFont("Microsoft YaHei UI", 10));
  }
  void init() { QSettings().clear(); }

  void uxD01_historicalDerivation() {
    QTemporaryDir directory;
    MainWindow window;
    QString issue;
    QVERIFY2(window.createProject(directory.path(), "history", &issue), qPrintable(issue));
    issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    const auto history = historyFor(window);
    window.session().activeFile()->detectionRuns.push_back(history);
    issue = addSource(window, directory.path(), "IQ_B_FS1Msps_FC200MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    QVERIFY(window.session().activeFile()->metadata.id != history.request.sourceIdentity.fileId);
    expose(window);
    window.findChild<DetectionPanel*>("detectionPanel")->refresh();
    focusHistory(window, "ux-history");
    auto* derive = window.findChild<QPushButton*>("deriveWidebandDetection");
    QVERIFY(derive && derive->isVisible() && derive->isEnabled());
    QTest::mouseClick(derive, Qt::LeftButton);
    const auto result = window.findChild<DetectionCard*>("detectionCard")->request();
    QCOMPARE(result.parentRunId, std::optional<std::string>(history.request.runId));
    QVERIFY(result.runId.empty());
    QCOMPARE(result.sourceIdentity.fileId, history.request.sourceIdentity.fileId);
    QCOMPARE(result.profile.fftSize, history.request.profile.fftSize);
    QCOMPARE(result.profile.hopSeconds, history.request.profile.hopSeconds);
    QCOMPARE(result.profile.modelFrames, history.request.profile.modelFrames);
    QCOMPARE(result.profile.modelHopFrames, history.request.profile.modelHopFrames);
    QCOMPARE(result.spectrumMode, history.request.spectrumMode);
    QCOMPARE(result.parameters.scoreThreshold, history.request.parameters.scoreThreshold);
    QCOMPARE(result.parameters.timeStrategy, history.request.parameters.timeStrategy);
    QCOMPARE(result.requestedRange, history.request.requestedRange);
    QCOMPARE(result.rangeOrigin, std::string("custom"));
    QCOMPARE(window.session().activeFile()->detectionRuns.size(), std::size_t{1});
  }

  void uxD03_crossFileCancellation() {
    QTemporaryDir directory;
    MainWindow window;
    QString issue;
    QVERIFY2(window.createProject(directory.path(), "cancellation", &issue), qPrintable(issue));
    issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    auto running = historyFor(window);
    running.lifecycle = DetectionLifecycle::Running;
    running.request.runId = "ux-running-a";
    running.computationPhase = "inference";
    const auto firstId = running.request.sourceIdentity.fileId;
    window.session().activeFile()->detectionRuns.push_back(running);
    issue = addSource(window, directory.path(), "IQ_B_FS1Msps_FC200MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    expose(window);
    window.findChild<QAction*>("widebandDetectionAction")->trigger();
    auto* card = window.findChild<DetectionCard*>("detectionCard");
    auto* cancel = window.findChild<QPushButton*>("cancelDetectionCardRun");
    QVERIFY(card && cancel);
    QVERIFY2(!cancel->isEnabled(), "A card for B must not cancel A's task");
    QSignalSpy cancelled(card, &DetectionCard::cancelRunRequested);
    QTest::mouseClick(cancel, Qt::LeftButton);
    QCOMPARE(cancelled.count(), 0);
    auto* view = window.findChild<QPushButton*>("viewRunningDetection");
    QVERIFY(view && view->isVisible());
    QTest::mouseClick(view, Qt::LeftButton);
    QCOMPARE(window.session().activeFile()->metadata.id, firstId);
    QVERIFY(cancel->isEnabled());
    QTest::mouseClick(cancel, Qt::LeftButton);
    QCOMPARE(cancelled.count(), 1);
    QCOMPARE(cancelled.takeFirst().at(0).toString(), QString("ux-running-a"));
    window.session().activeFile()->detectionRuns.front().lifecycle = DetectionLifecycle::Cancelled;
    window.refresh();
    QVERIFY(!cancel->isEnabled());
    QTest::mouseClick(cancel, Qt::LeftButton);
    QCOMPARE(cancelled.count(), 0);
  }

  void uxD02_viewSourceKeepsDraft() {
    QTemporaryDir directory;
    MainWindow window;
    QString issue;
    QVERIFY2(window.createProject(directory.path(), "source-view", &issue), qPrintable(issue));
    issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    window.session().activeFile()->detectionRuns.push_back(historyFor(window));
    expose(window);
    window.findChild<DetectionPanel*>("detectionPanel")->refresh();
    focusHistory(window, "ux-history");
    window.findChild<QAction*>("widebandDetectionAction")->trigger();
    auto* card = window.findChild<DetectionCard*>("detectionCard");
    auto* score = card->findChild<AdaptiveValueEdit*>("detectionScoreThreshold");
    window.findChild<QScrollArea*>("propScroll")->ensureWidgetVisible(score);
    const auto committed = card->request().parameters.scoreThreshold;
    score->setFocus();
    score->selectAll();
    QTest::keyClicks(score, "0.83");
    auto* list = window.findChild<QListView*>("detectionRunList");
    const auto index = list->currentIndex();
    QVERIFY(index.isValid());
    QSignalSpy focused(window.findChild<DetectionPanel*>("detectionPanel"),
                       &DetectionPanel::runFocused);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                     list->visualRect(index).topLeft() + QPoint(24, 12));
    QTest::keyClick(list, Qt::Key_Return);
    QTRY_VERIFY(!focused.isEmpty());
    QCOMPARE(score->text(), QString("0.83"));
    QVERIFY(score->property("parameterDraft").toBool());
    QCOMPARE(card->request().parameters.scoreThreshold, committed);
    QVERIFY(!card->request().parentRunId);
  }

  void uxD06_visibleRangeEntry() {
    QTemporaryDir directory;
    MainWindow window;
    QString issue;
    QVERIFY2(window.createProject(directory.path(), "visible-range", &issue), qPrintable(issue));
    issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    expose(window);
    auto* card = window.findChild<DetectionCard*>("detectionCard");
    const ViewRange custom{{500, 1000}, {99.9e6, 100.1e6}};
    card->setRequestedRoi(custom);
    const auto score = card->request().parameters.scoreThreshold;
    auto* button = window.findChild<QPushButton*>("widebandDetect");
    QVERIFY(button && button->isVisible() && button->isEnabled());
    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(card->request().rangeOrigin, std::string("visible"));
    QCOMPARE(card->request().requestedRange, window.session().activeFile()->view);
    QCOMPARE(card->request().parameters.scoreThreshold, score);
    QVERIFY(card->isVisible());
    QVERIFY(window.session().activeFile()->detectionRuns.empty());
  }

  void uxA01_roiConfirmationAndEscape() {
    QTemporaryDir directory;
    MainWindow window;
    QString issue;
    QVERIFY2(window.createProject(directory.path(), "roi", &issue), qPrintable(issue));
    issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    expose(window);
    auto* plot = window.findChild<PlotWidget*>("mainPlot");
    auto* manual = window.findChild<QPushButton*>("manualTool");
    auto* roi = window.findChild<QPushButton*>("detectionRoiTool");
    QVERIFY(plot && manual && roi);
    QTest::mouseClick(manual, Qt::LeftButton);
    QVERIFY(plot->isCreating());
    QTest::mouseClick(roi, Qt::LeftButton);
    QVERIFY(plot->isSelectingDetectionRoi());
    QTest::keyClick(plot, Qt::Key_Escape);
    QVERIFY(!plot->isSelectingDetectionRoi());
    QVERIFY(plot->isCreating());
    const auto count = window.session().activeFile()->marks.size();
    QTest::mouseClick(roi, Qt::LeftButton);
    const auto start = plot->toPixel(2000, 99.8e6).toPoint();
    const auto end = plot->toPixel(8000, 100.2e6).toPoint();
    QTest::mousePress(plot, Qt::LeftButton, Qt::NoModifier, start);
    QMouseEvent move(QEvent::MouseMove, end, plot->mapToGlobal(end),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(plot, &move);
    QTest::mouseRelease(plot, Qt::LeftButton, Qt::NoModifier, end);
    QVERIFY(plot->hasDetectionRoiPreview());
    const auto exact = *plot->detectionRoiPreview();
    QTest::keyClick(plot, Qt::Key_Return);
    auto* card = window.findChild<DetectionCard*>("detectionCard");
    QCOMPARE(card->request().rangeOrigin, std::string("custom"));
    QCOMPARE(card->request().requestedRange, exact);
    QVERIFY(card->isVisible());
    QVERIFY(plot->isCreating());
    QCOMPARE(window.session().activeFile()->marks.size(), count);
    QVERIFY(window.session().activeFile()->detectionRuns.empty());
  }
};

#include "popover_tests.h"
#include "toolbar_ux_tests.h"

QTEST_MAIN(UxTests)
#include "ux_tests.moc"
