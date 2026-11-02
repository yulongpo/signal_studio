#pragma once

#include <QCheckBox>
#include <QJsonObject>
#include <QScopeGuard>
#include <QStyleOptionButton>
#include <QVBoxLayout>
#include <map>

#include "infrastructure/detection_store.h"
#include "ui/controls/unsigned_value_edit.h"
#include "ui/parameter_input.h"

namespace {
// The caller must expose MainWindow before using the actual inspector entry.
bool uxDetectionPage(MainWindow& window) {
  auto* context = window.findChild<QAbstractButton*>("contextDetection");
  if (!context || !context->isVisible()) return false;
  if (context->isChecked() && window.findChild<DetectionCard*>("detectionCard")->isVisible()) return true;
  QTest::mouseClick(context, Qt::LeftButton);
  return window.findChild<DetectionCard*>("detectionCard")->isVisible();
}

bool uxDetectionReveal(MainWindow& window, QWidget* widget) {
  if (!widget || !uxDetectionPage(window)) return false;
  auto* scroll = window.findChild<QScrollArea*>("propScroll");
  if (!scroll) return false;
  scroll->ensureWidgetVisible(widget);
  QCoreApplication::processEvents();
  return widget->isVisible() &&
         scroll->viewport()->rect().intersects(QRect(
             widget->mapTo(scroll->viewport(), QPoint()), widget->size()));
}

bool uxDetectionFold(MainWindow& window, const char* name) {
  auto* toggle = window.findChild<QToolButton*>(name);
  if (!uxDetectionReveal(window, toggle)) return false;
  if (!toggle->isChecked()) QTest::mouseClick(toggle, Qt::LeftButton);
  return toggle->isChecked();
}
}  // namespace

inline void UxTests::uxD04_modelSpectrumMapping() {
  // No GPU, model assets or source data are needed to compare request mapping.
  const auto fixtureProperty = qApp->property("enableDetectionTestFixture");
  const bool hadEnvironment =
      qEnvironmentVariableIsSet("SS_ENABLE_DETECTION_TEST_FIXTURES");
  const auto fixtureEnvironment = qgetenv("SS_ENABLE_DETECTION_TEST_FIXTURES");
  const auto restoreFixtures = qScopeGuard([&] {
    qApp->setProperty("enableDetectionTestFixture", fixtureProperty);
    if (hadEnvironment)
      qputenv("SS_ENABLE_DETECTION_TEST_FIXTURES", fixtureEnvironment);
    else
      qunsetenv("SS_ENABLE_DETECTION_TEST_FIXTURES");
  });
  qApp->setProperty("enableDetectionTestFixture", false);
  qunsetenv("SS_ENABLE_DETECTION_TEST_FIXTURES");
  Session session;
  QWidget root;
  root.resize(580, 1000);
  auto* layout = new QVBoxLayout(&root);
  auto* card = new DetectionCard(session, [] { return QString{}; }, &root);
  layout->addWidget(card);
  layout->addWidget(card->actionsWidget());
  card->actionsWidget()->show();
  ParameterInputPolicy policy(&root);
  root.show();
  QCoreApplication::processEvents();
  auto* model = card->findChild<QComboBox*>("detectionModel");
  auto* spectrum = card->findChild<QComboBox*>("detectionSpectrumMode");
  auto* score = card->findChild<AdaptiveValueEdit*>("detectionScoreThreshold");
  QVERIFY(model && spectrum && score);
  QVERIFY(model->isVisible() && spectrum->isVisible() && score->isVisible());
  QVERIFY(!card->findChild<QComboBox*>("detectionProfile"));
  QCOMPARE(model->count(), 3);
  QCOMPARE(model->findData("pipeline-fixture-v1"), -1);
  const auto initial = card->request();
  auto* analysisToggle =
      card->findChild<QToolButton*>("detectionAnalysisParametersToggle");
  QVERIFY(analysisToggle && analysisToggle->isVisible());
  QTest::mouseClick(analysisToggle, Qt::LeftButton);
  const auto commit = [card](const char* name, const QString& input) {
    auto* edit = card->findChild<QLineEdit*>(name);
    if (!edit || !edit->isVisible()) return false;
    QTest::mouseClick(edit, Qt::LeftButton);
    edit->selectAll();
    QTest::keyClicks(edit, input);
    QTest::keyClick(edit, Qt::Key_Return);
    return !edit->property("parameterDraft").toBool();
  };
  std::map<std::string, QJsonObject> committed;
  for (const auto* id : {"ffscn-gui-iq-v1", "scn-max-psd-v1"}) {
    model->setCurrentIndex(model->findData(id));
    QCOMPARE(spectrum->count(), 5);
    QVERIFY(spectrum->isEnabled());
    for (int mode = 0; mode <= 4; ++mode) {
      spectrum->setCurrentIndex(spectrum->findData(mode));
      auto expected = initial;
      expected.profile = *findDetectionProfile(id);
      expected.parameters = DetectionParameters{};
      if (expected.profile.backend == DetectionBackendKind::Scn)
        expected.parameters.timeStrategy = DetectionTimeStrategy::Coarse;
      expected.spectrumMode = static_cast<DetectionSpectrumMode>(mode);
      // Full serialized request comparison catches changes beyond the two
      // selectors.
      QCOMPARE(encodeDetectionRequest(card->request()),
               encodeDetectionRequest(expected));
      const double value =
          .20 + mode * .03 +
          (expected.profile.backend == DetectionBackendKind::Scn ? .25 : 0);
      QTest::mouseClick(score, Qt::LeftButton);
      score->selectAll();
      QTest::keyClicks(score, QString::number(value, 'g', 17));
      QTest::keyClick(score, Qt::Key_Return);
      QCOMPARE(card->request().parameters.scoreThreshold, value);
      QVERIFY(commit("detectionFftSize", QString::number(3001 + mode)));
      QVERIFY(commit("detectionHopMilliseconds", QString::number(2 + mode)));
      QVERIFY(commit("detectionModelFrames", QString::number(3 + mode)));
      QVERIFY(commit("detectionModelHopFrames", QString::number(7 + mode)));
      QCOMPARE(card->request().profile.fftSize, std::uint32_t(3001 + mode));
      QCOMPARE(card->request().profile.hopSeconds, (2 + mode) / 1000.);
      QCOMPARE(card->request().profile.modelFrames, std::uint32_t(3 + mode));
      QCOMPARE(card->request().profile.modelHopFrames, std::uint32_t(7 + mode));
      const auto key =
          std::string(id) + (mode ? ":" + std::to_string(mode) : "");
      committed.emplace(key, encodeDetectionRequest(card->request()));
    }
  }
  for (const auto* id : {"ffscn-gui-iq-v1", "scn-max-psd-v1"}) {
    model->setCurrentIndex(model->findData(id));
    for (int mode = 0; mode <= 4; ++mode) {
      spectrum->setCurrentIndex(spectrum->findData(mode));
      const auto key =
          std::string(id) + (mode ? ":" + std::to_string(mode) : "");
      QCOMPARE(encodeDetectionRequest(card->request()), committed.at(key));
    }
  }
  // Invalid numeric values retain their text and restore the accepted value.
  const auto accepted = card->request().parameters.scoreThreshold;
  QTest::mouseClick(score, Qt::LeftButton);
  score->selectAll();
  QTest::keyClicks(score, "1.234");
  QTest::keyClick(score, Qt::Key_Return);
  QCOMPARE(card->request().parameters.scoreThreshold, accepted);
  QCOMPARE(score->value(), accepted);
  QCOMPARE(score->text(), QString("1.234"));
  QVERIFY(score->property("parameterDraft").toBool());
  QVERIFY(score->property("invalid").toBool());
  card->refresh();
  QCOMPARE(score->text(), QString("1.234"));
  QTest::keyClick(score, Qt::Key_Escape);
  QVERIFY(!score->property("invalid").toBool());
  model->setCurrentIndex(model->findData("ffscn-17-experimental-v1"));
  QVERIFY(model->currentText().contains("实验"));
  QCOMPARE(spectrum->count(), 1);
  QCOMPARE(spectrum->currentData().toInt(), 0);
  QVERIFY(!spectrum->isEnabled());
  QCOMPARE(card->request().spectrumMode, DetectionSpectrumMode::ModelDefault);
  qputenv("SS_ENABLE_DETECTION_TEST_FIXTURES", "1");
  card->hide();
  card->actionsWidget()->hide();
  auto* fixtureCard =
      new DetectionCard(session, [] { return QString{}; }, &root);
  layout->addWidget(fixtureCard);
  fixtureCard->show();
  QCoreApplication::processEvents();
  auto* fixtureModel = fixtureCard->findChild<QComboBox*>("detectionModel");
  QVERIFY(fixtureModel && fixtureModel->isVisible());
  QVERIFY(fixtureModel->findData("pipeline-fixture-v1") >= 0);
  fixtureModel->setCurrentIndex(fixtureModel->findData("pipeline-fixture-v1"));
  QCOMPARE(fixtureCard->findChild<QComboBox*>("detectionSpectrumMode")->count(),
           1);
  QCOMPARE(fixtureCard->request().profile.backend,
           DetectionBackendKind::Fixture);
}

inline void UxTests::uxD05_foldedParametersAndFirstScreen() {
  QTemporaryDir directory;
  MainWindow window;
  QString issue;
  QVERIFY2(window.createProject(directory.path(), "detection-layout", &issue),
           qPrintable(issue));
  issue = addSource(window, directory.path(), "IQ_FS1Msps_FC100MHz.dat");
  QVERIFY2(issue.isEmpty(), qPrintable(issue));
  expose(window);
  QVERIFY(uxDetectionPage(window));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* scroll = window.findChild<QScrollArea*>("propScroll");
  QVERIFY(card && scroll);
  const auto firstRequest = encodeDetectionRequest(card->request());
  for (const QSize size : {QSize(1920, 1080), QSize(2560, 1440)}) {
  window.resize(size);
  QTest::qWait(30);
  for (const auto* name : {"detectionModel", "detectionSpectrumMode",
                           "detectionRangeOrigin", "detectionScoreThreshold",
                           "detectionTimeStrategy", "detectionPreflight"}) {
    auto* widget = card->findChild<QWidget*>(name);
    QVERIFY2(widget && widget->isVisible(), name);
    const QRect bounds(widget->mapTo(scroll->viewport(), QPoint()),
                       widget->size());
    QVERIFY2(scroll->viewport()->rect().contains(bounds), name);
  }
  auto* firstRun = window.findChild<QPushButton*>("runWidebandDetection");
  QVERIFY(firstRun && firstRun->isVisible());
  const QRect runBounds(firstRun->mapTo(&window, QPoint()), firstRun->size());
  QVERIFY(window.rect().contains(runBounds));
  auto* hit = window.childAt(runBounds.center());
  QVERIFY(hit == firstRun || (hit && firstRun->isAncestorOf(hit)));
  }
  for (const auto* name :
       {"detectionAnalysisParametersToggle",
        "detectionPostprocessParametersToggle",
        "detectionResourceParametersToggle",
        "detectionCalculationDetailsToggle", "detectionDiagnosticsToggle"}) {
    auto* toggle = card->findChild<QToolButton*>(name);
    QVERIFY(toggle && toggle->isVisible());
    QVERIFY(!toggle->isChecked());
  }
  for (const auto* name :
       {"detectionFftSize", "detectionModelFrames", "detectionNmsIou",
        "detectionBatchSize", "detectionDiagnostics"})
    QVERIFY(!card->findChild<QWidget*>(name)->isVisible());
  auto* draft = card->findChild<QLabel*>("detectionParameterDraftNote");
  QVERIFY(draft && !draft->isVisible() && draft->text().isEmpty());
  QVERIFY(!scroll->isAncestorOf(card->actionsWidget()));
  auto* run = window.findChild<QPushButton*>("runWidebandDetection");
  QVERIFY(run && run->isVisible());
  for (const auto* name : {"detectionAnalysisParametersToggle",
                           "detectionPostprocessParametersToggle",
                           "detectionResourceParametersToggle"})
    QVERIFY(uxDetectionFold(window, name));
  QCOMPARE(encodeDetectionRequest(card->request()), firstRequest);
  QTest::mouseClick(window.findChild<QAbstractButton*>("contextDisplay"), Qt::LeftButton);
  QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
  QVERIFY(uxDetectionPage(window));
  auto* fft = dynamic_cast<UnsignedValueEdit*>(card->findChild<QLineEdit*>("detectionFftSize"));
  QVERIFY(uxDetectionReveal(window, fft));
  QTest::mouseClick(fft, Qt::LeftButton);
  fft->selectAll();
  QTest::keyClicks(fft, "3001");
  QTest::keyClick(fft, Qt::Key_Return);
  QCOMPARE(card->request().profile.fftSize, std::uint32_t{3001});
  QVERIFY(uxDetectionReveal(window, fft));
  QVERIFY(fft->hasFocus());
  fft->selectAll();
  QTest::keyClicks(fft, "0");
  QTest::keyClick(fft, Qt::Key_Return);
  QCOMPARE(card->request().profile.fftSize, std::uint32_t{3001});
  QCOMPARE(fft->value(), std::uint64_t{3001});
  QCOMPARE(fft->text(), QString("0"));
  QVERIFY(fft->property("parameterDraft").toBool());
  QVERIFY(fft->property("invalid").toBool());
  card->refresh();
  QCOMPARE(fft->text(), QString("0"));
  QTest::keyClick(fft, Qt::Key_Escape);
  QVERIFY(!fft->property("invalid").toBool());
  auto* sample = card->findChild<QPushButton*>("detectionStrategySample");
  auto* coarse = card->findChild<QPushButton*>("detectionStrategyCoarse");
  auto* split = card->findChild<QCheckBox*>("detectionSplitBursts");
  auto* fine = card->findChild<QWidget*>("detectionSampleEdgeRatio");
  auto* minimum = card->findChild<QWidget*>("detectionMinOnMilliseconds");
  auto* gap = card->findChild<QWidget*>("detectionCoarseSplitGapMilliseconds");
  QVERIFY(sample && coarse && split && fine && minimum && gap);
  const auto toggleSplit = [split] {
    QStyleOptionButton option;
    option.initFrom(split); option.text = split->text();
    const auto indicator = split->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, split);
    QTest::mouseClick(split, Qt::LeftButton, Qt::NoModifier, indicator.center());
  };
  QVERIFY(sample->toolTip().contains("sample", Qt::CaseInsensitive));
  QVERIFY(uxDetectionReveal(window, coarse));
  auto* margin = card->findChild<AdaptiveValueEdit*>("detectionBandwidthMarginDb");
  QVERIFY(uxDetectionReveal(window, margin));
  QTest::mouseClick(margin, Qt::LeftButton);
  margin->selectAll(); QTest::keyClicks(margin, "bad");
  QTest::keyClick(margin, Qt::Key_Return);
  QVERIFY(margin->property("parameterDraft").toBool());
  QVERIFY(uxDetectionReveal(window, coarse));
  QTest::mouseClick(coarse, Qt::LeftButton);
  QVERIFY(!margin->property("parameterDraft").toBool());
  QVERIFY(!fine->isVisible());
  QVERIFY(!minimum->isVisible());
  QVERIFY(gap->isVisible());
  const auto gapValue = card->request().parameters.coarseSplitGapSeconds;
  QVERIFY(uxDetectionReveal(window, split));
  toggleSplit();
  QVERIFY(!gap->isVisible());
  QCOMPARE(card->request().parameters.coarseSplitGapSeconds, gapValue);
  QVERIFY(uxDetectionReveal(window, sample));
  QTest::mouseClick(sample, Qt::LeftButton);
  QVERIFY(fine->isVisible());
  QVERIFY(!minimum->isVisible());
  QVERIFY(uxDetectionReveal(window, split));
  toggleSplit();
  QVERIFY(minimum->isVisible());
  QVERIFY(!gap->isVisible());
  auto* score = card->findChild<AdaptiveValueEdit*>("detectionScoreThreshold");
  QVERIFY(uxDetectionReveal(window, score));
  const auto committed = card->request().parameters.scoreThreshold;
  QTest::mouseClick(score, Qt::LeftButton);
  score->selectAll();
  QTest::keyClicks(score, "0.83");
  card->refresh();
  QCOMPARE(score->text(), QString("0.83"));
  QCOMPARE(card->request().parameters.scoreThreshold, committed);
  QVERIFY(draft->isVisible() && !draft->text().contains('\n'));
  QTest::keyClick(score, Qt::Key_Escape);
  card->refresh();
  QVERIFY(!draft->isVisible());
}

inline void UxTests::uxD08_preflightDetails() {
  // This explicitly enabled fixture tests preflight and request plumbing only.
  const bool hadEnvironment =
      qEnvironmentVariableIsSet("SS_ENABLE_DETECTION_TEST_FIXTURES");
  const auto environment = qgetenv("SS_ENABLE_DETECTION_TEST_FIXTURES");
  const auto restoreEnvironment = qScopeGuard([&] {
    if (hadEnvironment)
      qputenv("SS_ENABLE_DETECTION_TEST_FIXTURES", environment);
    else
      qunsetenv("SS_ENABLE_DETECTION_TEST_FIXTURES");
  });
  qputenv("SS_ENABLE_DETECTION_TEST_FIXTURES", "1");
  QTemporaryDir directory;
  MainWindow window;
  QString issue;
  QVERIFY2(window.createProject(directory.path(), "preflight", &issue),
           qPrintable(issue));
  issue = addSource(window, directory.path(), "IQ_FS1Msps_FC100MHz.dat");
  QVERIFY2(issue.isEmpty(), qPrintable(issue));
  expose(window);
  QVERIFY(uxDetectionPage(window));
  auto* card = window.findChild<DetectionCard*>("detectionCard");
  auto* model = card->findChild<QComboBox*>("detectionModel");
  auto* preflight = card->findChild<QLabel*>("detectionPreflight");
  auto* diagnostics = card->findChild<QLabel*>("detectionDiagnostics");
  auto* detailsToggle =
      card->findChild<QToolButton*>("detectionDiagnosticsToggle");
  QVERIFY(model && preflight && diagnostics && detailsToggle);
  QVERIFY(!detailsToggle->isChecked() && !diagnostics->isVisible());
  QVERIFY(uxDetectionReveal(window, model));
  model->setCurrentIndex(model->findData("pipeline-fixture-v1"));
  QVERIFY2(card->validationError().isEmpty(),
           qPrintable(card->validationError()));
  QCOMPARE(preflight->text(), QString("配置就绪 · 可运行"));
  QVERIFY(!preflight->text().contains('\n'));
  QVERIFY(uxDetectionFold(window, "detectionCalculationDetailsToggle"));
  auto* calculation =
      card->findChild<QLabel*>("detectionCalculationDetailsText");
  QVERIFY(calculation && calculation->isVisible());
  for (const auto* word :
       {"FFT 窗", "谱步长", "模型上下文", "模型输入", "补零", "真实分辨率"})
    QTRY_VERIFY2(calculation->text().contains(QString::fromUtf8(word)), word);
  auto* times = card->findChild<QLabel*>("detectionPhysicalTimes");
  QVERIFY(times && times->text().count('\n') <= 1);
  QVERIFY(uxDetectionFold(window, "detectionDiagnosticsToggle"));
  QVERIFY(diagnostics->isVisible());
  QTRY_VERIFY(diagnostics->text().contains("只读必要上下文"));
  QTRY_VERIFY(diagnostics->text().contains("来源指纹"));
  const auto available = card->request().contextRange;
  auto invalid = available;
  invalid.time.end = window.session().activeFile()->metadata.sampleCount + 1;
  card->setRequestedRoi(invalid);
  QVERIFY(card->validationError().contains("前缀"));
  QVERIFY(preflight->text().contains("前缀") &&
          preflight->text().contains("请"));
  QTRY_VERIFY(diagnostics->text().contains(card->validationError()));
  card->useVisibleRange();
  // Experimental profile gives a deterministic model error without altering
  // assets.
  QVERIFY(uxDetectionReveal(window, model));
  model->setCurrentIndex(model->findData("ffscn-17-experimental-v1"));
  QVERIFY(preflight->text().contains("实验"));
  QVERIFY(preflight->text().contains("请选择") ||
          preflight->text().contains("选择已验证模型"));
  QTRY_VERIFY(diagnostics->text().contains("模型包"));
  QTRY_VERIFY(diagnostics->text().contains(card->validationError()));
  QVERIFY(uxDetectionReveal(window, model));
  model->setCurrentIndex(model->findData("pipeline-fixture-v1"));
  auto& metadata = window.session().activeFile()->metadata;
  const auto availability = metadata.availability;
  metadata.availability.status = LoadStatus::Partial;
  metadata.availability.availableSamples = 0;
  card->refresh();
  QVERIFY(!preflight->property("preflightOk").toBool());
  QTRY_VERIFY(diagnostics->text().contains("可用前缀"));
  metadata.availability = availability;
  card->refresh();
  QCOMPARE(preflight->text(), QString("配置就绪 · 可运行"));
  QVERIFY(window.session().activeFile()->detectionRuns.empty());
}
