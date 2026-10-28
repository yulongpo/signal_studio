#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryDir>
#include <QtEndian>
#include <QTest>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "app/main_window.h"
#include "ui/detection/detection_card.h"
#include "ui/detection/detection_panel.h"

using namespace signalstudio;

template <class Panel>
concept HasStampedStartRequest = requires(Panel& panel, DetectionRequest request,
                                          QString& error) {
  { panel.startRequest(std::move(request), error, std::uint64_t{1}) } -> std::same_as<bool>;
};

template <class Panel>
concept HasPublicCancelRun = requires(Panel& panel, const QString& runId) {
  panel.cancelRun(runId);
};

// 0 means the planned signature is absent. Callers must fail the case.
// A zero from this helper is not a passing result.
template <class Panel>
int exercisePlannedProvisionalCancel(Panel& panel, const QString& runId) {
  if constexpr (HasStampedStartRequest<Panel> && HasPublicCancelRun<Panel>) {
    panel.cancelRun(runId);
    return 1;
  } else {
    return 0;
  }
}

class VisiblePreparingPaint : public QObject {
 public:
  QElapsedTimer timer;
  bool armed = false;
  qint64 paintNs = -1;

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (!armed || event->type() != QEvent::Paint) return false;
    const auto* widget = qobject_cast<const QWidget*>(watched);
    if (!widget || !widget->isVisible()) return false;
    QString text;
    if (const auto* button = qobject_cast<const QPushButton*>(widget))
      text = button->text();
    else if (const auto* label = qobject_cast<const QLabel*>(widget))
      text = label->text();
    if (!text.contains(QStringLiteral("准备检测中"))) return false;
    paintNs = timer.nsecsElapsed();
    armed = false;
    return false;
  }
};

double percentileMs(std::vector<qint64> samples, double quantile) {
  std::sort(samples.begin(), samples.end());
  const double position = quantile * static_cast<double>(samples.size() - 1);
  const auto lower = static_cast<std::size_t>(position);
  const auto upper = std::min(lower + 1, samples.size() - 1);
  const double fraction = position - static_cast<double>(lower);
  const double nanoseconds =
      static_cast<double>(samples[lower]) +
      (static_cast<double>(samples[upper]) - static_cast<double>(samples[lower])) * fraction;
  return nanoseconds / 1e6;
}

class SsDeRpoUiTests : public QObject {
  Q_OBJECT

 private slots:
  void doubleClickArmsOneStart();
  void synchronousRejectionKeepsTheError();
  void repeatedVisiblePreparingPaintP95();
  void provisionalCancelBeforeCommit();
};

bool prepareCard(QTemporaryDir& directory, MainWindow& window, QString& error) {
  const auto iqPath = directory.filePath("IQ_FS1Msps_FC100MHz.dat");
  QFile iq(iqPath);
  if (!iq.open(QIODevice::WriteOnly)) {
    error = iq.errorString();
    return false;
  }
  QByteArray bytes(20000 * 4, '\0');
  for (int index = 0; index < 20000; ++index)
    qToLittleEndian<qint16>(12000, bytes.data() + index * 4);
  if (iq.write(bytes) != static_cast<qint64>(bytes.size())) {
    error = iq.errorString();
    return false;
  }
  iq.close();
  if (!window.createProject(directory.path(), "ss-de-rpo-ui", &error) ||
      !window.addIqFile(iqPath, &error))
    return false;
  window.show();
  const auto ready = QTest::qWaitFor(
      [&] {
        const auto* file = window.session().activeFile();
        return file && file->metadata.availability.status == LoadStatus::Ready;
      },
      10000);
  if (!ready) {
    error = QStringLiteral("IQ did not become ready");
    return false;
  }
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* range = card ? card->findChild<QComboBox*>("detectionRangeOrigin") : nullptr;
  auto* model = card ? card->findChild<QComboBox*>("detectionModel") : nullptr;
  if (!card || !range || !model || model->findData("pipeline-fixture-v1") < 0 ||
      range->findData("available-file") < 0) {
    error = QStringLiteral("detection card controls are missing");
    return false;
  }
  model->setCurrentIndex(model->findData("pipeline-fixture-v1"));
  range->setCurrentIndex(range->findData("available-file"));
  QCoreApplication::processEvents();
  return true;
}

QString gExposureNote = QStringLiteral("evidence=unspecified");

QString screenFact(const QScreen* screen) {
  if (!screen) return QStringLiteral("name=none geometry=none dpr=none");
  const QRect geometry = screen->geometry();
  return QStringLiteral("name=%1 geometry=%2,%3,%4x%5 dpr=%6")
      .arg(screen->name())
      .arg(geometry.x())
      .arg(geometry.y())
      .arg(geometry.width())
      .arg(geometry.height())
      .arg(QString::number(screen->devicePixelRatio(), 'f', 4));
}

// Physical 3840x2160 at 1.5 is the logical 2560x1440 workspace. A screen whose
// geometry is already reported as 3840x2160 at the same ratio is also a candidate.
bool screenIsNative4k150(const QScreen* screen) {
  if (!screen) return false;
  const double dpr = screen->devicePixelRatio();
  if (std::abs(dpr - 1.5) > 0.02) return false;
  const QRect geometry = screen->geometry();
  const int physicalWidth = qRound(static_cast<double>(geometry.width()) * dpr);
  const int physicalHeight = qRound(static_cast<double>(geometry.height()) * dpr);
  const bool logicalWorkspace = geometry.width() == 2560 && geometry.height() == 1440 &&
                                physicalWidth == 3840 && physicalHeight == 2160;
  const bool reportedPhysical = geometry.width() == 3840 && geometry.height() == 2160;
  return logicalWorkspace || reportedPhysical;
}

bool openDetectionPage(MainWindow& window, QPushButton* run) {
  if (auto* rail = window.findChild<QPushButton*>("propRailOpen");
      rail && rail->isVisible())
    rail->click();
  auto* tab = window.findChild<QAbstractButton*>("contextDetection");
  if (!tab) return false;
  if (!tab->isChecked()) tab->click();
  QCoreApplication::processEvents();
  return run && run->isVisible();
}

bool exposeDetectionRun(MainWindow& window, QPushButton* run) {
  const bool offscreen =
      QGuiApplication::platformName().compare(QStringLiteral("offscreen"), Qt::CaseInsensitive) ==
      0;
  if (offscreen) {
    window.resize(2560, 1440);
    window.show();
    QCoreApplication::processEvents();
    gExposureNote =
        QStringLiteral("evidence=functional-offscreen name=offscreen geometry=2560x1440 dpr=n/a window=%1x%2")
            .arg(window.width())
            .arg(window.height());
    qInfo().noquote() << QStringLiteral("R-19 screen ") << gExposureNote;
    if (window.width() != 2560 || window.height() != 1440) return false;
    return openDetectionPage(window, run);
  }

  QList<QScreen*> matches;
  QStringList listed;
  for (QScreen* screen : QGuiApplication::screens()) {
    listed.append(screenFact(screen));
    if (screenIsNative4k150(screen)) matches.append(screen);
  }
  qInfo().noquote() << QStringLiteral("R-19 screen candidates ")
                    << listed.join(QStringLiteral(" | "));
  if (matches.size() != 1) {
    gExposureNote = QStringLiteral("evidence=native-rejected matches=%1").arg(matches.size());
    qInfo().noquote() << QStringLiteral("R-19 screen ") << gExposureNote;
    return false;
  }
  QScreen* screen = matches.front();
  window.resize(2560, 1440);
  window.show();
  if (QWindow* handle = window.windowHandle()) handle->setScreen(screen);
  window.setGeometry(screen->geometry());
  window.showFullScreen();
  QCoreApplication::processEvents();
  gExposureNote = QStringLiteral("evidence=native-fullscreen %1 window=%2x%3")
                      .arg(screenFact(screen))
                      .arg(window.width())
                      .arg(window.height());
  qInfo().noquote() << QStringLiteral("R-19 screen ") << gExposureNote;
  if (window.width() != 2560 || window.height() != 1440) return false;
  return openDetectionPage(window, run);
}

void SsDeRpoUiTests::doubleClickArmsOneStart() {
  qApp->setProperty("enableDetectionTestFixture", true);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareCard(directory, window, error), qPrintable(error));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  QVERIFY(card && run);

  int emissions = 0;
  bool preparing = false;
  QObject::connect(card, &DetectionCard::requestSubmitted, &window,
                   [&](const DetectionRequest&) {
                     if (emissions == 0) {
                       preparing = card->startLatchActive() && !run->isEnabled() &&
                                   run->text().contains(QStringLiteral("准备检测中"));
                       run->click();
                     }
                     ++emissions;
                   });
  auto* preflight = card->findChild<QLabel*>("detectionPreflight");
  QElapsedTimer timer;
  timer.start();
  run->click();
  const auto elapsedMs = timer.elapsed();
  for (int extra = 0; extra < 4 && card->startLatchActive(); ++extra) run->click();
  qInfo().nospace() << "R-08 click handler returned in " << elapsedMs
                    << " ms; emissions=" << emissions
                    << " latch=" << card->startLatchActive();
  if (emissions == 0 && preflight)
    QFAIL(qPrintable(QStringLiteral("start did not arm: ") + preflight->text()));
  QCOMPARE(emissions, 1);
  QVERIFY(preparing);
  QVERIFY2(elapsedMs < 1000, "the click handler blocked for more than one second");
}

void SsDeRpoUiTests::repeatedVisiblePreparingPaintP95() {
  qApp->setProperty("enableDetectionTestFixture", true);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareCard(directory, window, error), qPrintable(error));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  auto* preflight = card ? card->findChild<QLabel*>("detectionPreflight") : nullptr;
  auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
  QVERIFY(card && run && preflight && panel);
  QVERIFY2(exposeDetectionRun(window, run),
           "the run button is not visible on the 2560x1440 detection page");
  // Hold the real precommit gate. requestSubmitted stays connected, so each
  // click is a StartCommit. Cancel and wait until that commit is idle before
  // the next click. noteStartRejected is not a fresh detection.
  auto gate = std::make_shared<std::atomic<bool>>(false);
  panel->setStartCommitGate(gate);

  VisiblePreparingPaint paint;
  run->installEventFilter(&paint);
  preflight->installEventFilter(&paint);
  constexpr int kRepeats = 20;
  std::vector<qint64> paintNs;
  std::vector<qint64> handlerNs;
  paintNs.reserve(kRepeats);
  handlerNs.reserve(kRepeats);
  for (int index = 0; index < kRepeats; ++index) {
    QVERIFY2(!panel->projectCommitInProgress() && !panel->startCommitGateBlocked() &&
                 !card->startLatchActive(),
             "previous StartCommit is still active; this click would not be a fresh detection");
    paint.paintNs = -1;
    paint.armed = true;
    paint.timer.start();
    QElapsedTimer handler;
    handler.start();
    run->click();
    handlerNs.push_back(handler.nsecsElapsed());
    const auto painted = QTest::qWaitFor([&paint] { return !paint.armed; }, 1000);
    QVERIFY2(painted && paint.paintNs >= 0,
             "no visible Paint of 准备检测中; handler time is not the p95 metric");
    paintNs.push_back(paint.paintNs);
    QVERIFY2(QTest::qWaitFor([&] { return panel->startCommitGateBlocked(); }, 20000),
             "StartCommit did not reach the precommit gate");
    const auto runId = QString::fromStdString(card->startLatchRunId());
    QVERIFY2(!runId.isEmpty(), "precommit gate is blocked without a provisional run id");
    panel->cancelRun(runId);
    const auto idle = QTest::qWaitFor(
        [&] {
          return !panel->startCommitGateBlocked() && !panel->projectCommitInProgress() &&
                 !card->startLatchActive();
        },
        20000);
    QVERIFY2(idle, "cancel did not finish the StartCommit before the next sample");
  }
  const auto paintP95 = percentileMs(paintNs, 0.95);
  const auto handlerP95 = percentileMs(handlerNs, 0.95);
  qInfo().nospace() << "R-19 metric=visible-paint n=" << kRepeats
                    << " paintP95Ms=" << paintP95 << " handlerP95Ms=" << handlerP95
                    << " targetMs=100 " << qPrintable(gExposureNote);
  QVERIFY2(paintNs.size() == static_cast<std::size_t>(kRepeats), "a paint sample is missing");
  QVERIFY2(paintP95 < 100.0,
           qPrintable(QStringLiteral("R-19 visible preparing paint p95 is %1 ms, target 100 ms")
                          .arg(paintP95, 0, 'f', 3)));
}

void SsDeRpoUiTests::provisionalCancelBeforeCommit() {
  qApp->setProperty("enableDetectionTestFixture", true);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareCard(directory, window, error), qPrintable(error));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  auto* cancel = window.findChild<QPushButton*>("cancelDetectionCardRun");
  auto* preflight = card ? card->findChild<QLabel*>("detectionPreflight") : nullptr;
  auto* panel = window.findChild<DetectionPanel*>();
  QVERIFY(card && run && cancel && preflight && panel);
  QVERIFY2(exposeDetectionRun(window, run), "detection actions are not visible");
  auto gate = std::make_shared<std::atomic<bool>>(false);
  panel->setStartCommitGate(gate);

  int cancelEmissions = 0;
  QString cancelledId;
  QObject::connect(card, &DetectionCard::cancelRunRequested, &window,
                   [&](const QString& id) {
                     ++cancelEmissions;
                     cancelledId = id;
                   });
  run->click();
  QVERIFY2(card->startLatchActive(), "provisional start did not arm");
  const auto latchId = QString::fromStdString(card->startLatchRunId());
  QVERIFY(!latchId.isEmpty());
  QVERIFY2(QTest::qWaitFor([&] { return panel->startCommitGateBlocked(); }, 20000),
           "provisional cancel did not reach the precommit gate");
  const auto* file = window.session().activeFile();
  QVERIFY(file);
  const auto formalBeforeCancel = file->detectionRuns.size();
  cancel->click();
  const int exercised = exercisePlannedProvisionalCancel(*panel, latchId);
  QVERIFY2(QTest::qWaitFor(
               [&] {
                 return !panel->startCommitGateBlocked() && !panel->projectCommitInProgress() &&
                        !card->startLatchActive();
               },
               20000),
           "provisional cancel left the StartCommit active");
  // Returning from the helper is not success. The planned API is
  // startRequest(request, error, clickSteadyNs) plus public cancelRun that
  // handles a provisional start before any persisted run.
  if (exercised == 0) {
    QFAIL(qPrintable(QStringLiteral(
        "R-09 FAIL planned DetectionPanel::startRequest(request, error, clickSteadyNs) "
        "and public cancelRun are not both available. cancelRunRequested emissions=%1 "
        "id=%2. This is not a pass.")
                         .arg(cancelEmissions)
                         .arg(cancelledId)));
  }
  QVERIFY2(cancelEmissions == 1 && cancelledId == latchId,
           qPrintable(QStringLiteral("provisional cancel emitted %1 id=%2 latch=%3")
                          .arg(cancelEmissions)
                          .arg(cancelledId, latchId)));
  QVERIFY2(!preflight->text().contains(QStringLiteral("检测结果正在提交")),
           qPrintable(preflight->text()));
  QVERIFY2(file->detectionRuns.size() == formalBeforeCancel,
           "provisional cancel changed the persisted run list");
  const bool completed = std::any_of(
      file->detectionRuns.begin(), file->detectionRuns.end(), [](const DetectionRun& run) {
        return run.lifecycle == DetectionLifecycle::Completed;
      });
  QVERIFY2(!completed, "provisional cancel published a formal Completed result");
}

void SsDeRpoUiTests::synchronousRejectionKeepsTheError() {
  qApp->setProperty("enableDetectionTestFixture", true);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  MainWindow window;
  QString error;
  QVERIFY2(prepareCard(directory, window, error), qPrintable(error));
  const auto projects = QDir(directory.path()).entryList({QStringLiteral("*.ssproj")}, QDir::Files);
  QCOMPARE(projects.size(), 1);
  QVERIFY(QFile::remove(directory.filePath(projects.front())));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  auto* preflight = card ? card->findChild<QLabel*>("detectionPreflight") : nullptr;
  QVERIFY(card && run && preflight);
  run->click();
  QVERIFY(!card->startLatchActive());
  QVERIFY(run->isEnabled());
  QVERIFY2(preflight->text().contains(QStringLiteral("检测事务需要当前已保存 .ssproj")),
           qPrintable(preflight->text()));
  QVERIFY(!preflight->text().contains(QStringLiteral("配置就绪")));
}

QTEST_MAIN(SsDeRpoUiTests)
#include "ss_de_rpo_ui_tests.moc"
