#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QFileInfo>
#include <cmath>
#include <QPushButton>
#include <QtTest>
#include "app/main_window.h"
#include "ui/narrowband_workspace.h"

using namespace signalstudio;
class NarrowbandAnalysisUiTests : public QObject {
  Q_OBJECT
  void populate(MainWindow& window,const QString& name="tone.raw") {
    FileMetadata source;source.name=name.toStdString();
    source.path=(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/"+name).toStdString();
    source.sampleRateHz=20e6;source.centerFrequencyHz=100e6;source.sampleCount=static_cast<SampleIndex>(QFileInfo(QString::fromStdString(source.path)).size()/4);source.demo=false;
    window.session().addDemoFile(source);
    window.session().addMark({{1000,200000},{101.5e6,102.5e6}});
    window.resize(2560,1440);window.show();window.refresh();
    QCoreApplication::processEvents();
  }
 private slots:
  void initTestCase() {
    qApp->setProperty("softwareRenderer",true);
    qApp->setProperty("uiStatePersistenceEnabled",false);
  }
  void samplingTransactionAndActualCreation() {
    MainWindow window;populate(window);
    auto* extract=window.findChild<QPushButton*>("extract");QVERIFY(extract);extract->click();
    auto* dialog=window.findChild<QDialog*>("channelConfigDialog");QVERIFY(dialog);
    auto* mode=dialog->findChild<QComboBox*>("channelSamplingMode");
    auto* factor=dialog->findChild<QComboBox*>("channelBandwidthFactor");
    auto* rate=dialog->findChild<QDoubleSpinBox*>("channelOutputRateMSps");
    auto* band=dialog->findChild<QDoubleSpinBox*>("channelBandwidthMHz");
    auto* preview=dialog->findChild<QLabel*>("channelDspPlanPreview");
    auto* confirm=dialog->findChild<QPushButton*>("confirmChannelConfig");
    QVERIFY(mode && factor && rate && band && preview && confirm);
    QCOMPARE(mode->currentIndex(),0);QCOMPARE(factor->currentData().toDouble(),2.);
    QCOMPARE(preview->property("realizedRateHz").toDouble(),2e6);QVERIFY(confirm->isEnabled());
    mode->setCurrentIndex(1);rate->setValue(1.5);band->setValue(.8);
    QCOMPARE(rate->value(),1.5);QVERIFY(confirm->isEnabled());
    rate->setValue(.5);QVERIFY(!confirm->isEnabled());QCOMPARE(rate->value(),.5);
    rate->setValue(1.5);mode->setCurrentIndex(0);factor->setCurrentIndex(3);
    QCOMPARE(preview->property("targetRateHz").toDouble(),6.4e6);
    factor->setCurrentIndex(1);QCOMPARE(preview->property("realizedRateHz").toDouble(),1.6e6);
    mode->setCurrentIndex(1);QCOMPARE(rate->value(),1.5);mode->setCurrentIndex(0);
    dialog->findChild<QCheckBox*>("channelAutoOpen")->setChecked(false);
    confirm->click();
    QTRY_COMPARE_WITH_TIMEOUT(window.session().activeFile()->channels.size(),std::size_t{1},15000);
    const auto& c=window.session().activeFile()->channels[0];
    QCOMPARE(c.outputSampleRateHz,1.6e6);QCOMPARE(c.sampling.mode,ChannelSamplingMode::RecommendedFactor);
  }
  void legacyEditKeepsRateAndDraft() {
    MainWindow window;populate(window);
    auto* file=window.session().activeFile();
    const auto id=window.session().createChannel("legacy",file->activeMarkId,102e6,1e6,4e6,{1000,200000},ChannelFilter::Standard,false,true);
    QVERIFY(!id.empty());window.refresh();
    auto* edit=window.findChild<QPushButton*>("narrowbandEditChannel");QVERIFY(edit);edit->click();
    auto* dialog=window.findChild<QDialog*>("channelConfigDialog");QVERIFY(dialog);
    auto* mode=dialog->findChild<QComboBox*>("channelSamplingMode");
    auto* rate=dialog->findChild<QDoubleSpinBox*>("channelOutputRateMSps");
    QCOMPARE(mode->currentIndex(),1);QCOMPARE(rate->value(),4.);
    auto* editor=rate->findChild<QLineEdit*>();editor->setFocus();editor->selectAll();QTest::keyClicks(editor,"3.2");
    window.refresh();QCOMPARE(window.session().activeChannel()->outputSampleRateHz,4e6);
    QVERIFY(editor->text().startsWith("3.2"));
    QTest::keyClick(editor,Qt::Key_Return);
    QCOMPARE(rate->value(),3.2);QCOMPARE(window.session().activeChannel()->outputSampleRateHz,4e6);
    dialog->reject();QCOMPARE(window.session().activeChannel()->outputSampleRateHz,4e6);
  }
  void realRawPointsAndCancellation() {
    if(qEnvironmentVariable("SS_NARROWBAND_NODE")=="W1")return;
    MainWindow window;populate(window);auto* file=window.session().activeFile();
    const auto id=window.session().createChannel("real",file->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);
    QVERIFY(!id.empty());window.refresh();
    auto* workspace=window.findChild<NarrowbandWorkspace*>();QVERIFY(workspace);
    workspace->activatePageForAcceptance(1);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult()!=nullptr,15000);
    const auto result=workspace->analysisResult();QVERIFY(result->status==AnalysisStatus::Ready);
    QVERIFY(!result->constellation.empty());QVERIFY(result->constellation.front().value==result->source->samples.front());
    auto* chart=window.findChild<QWidget*>("narrowbandConstellationLargeChart");QVERIFY(chart);
    QVERIFY(chart->property("realIqSource").toBool());QCOMPARE(chart->property("actualSampleRateHz").toULongLong(),qulonglong{2000000});
    QVERIFY(chart->property("iqAxisMaximum").toDouble()<.3);QVERIFY(chart->property("equalAxisScale").toBool());
    auto* points=window.findChild<QComboBox*>("constellationPointBudget");QVERIFY(points);points->setCurrentIndex(0);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->constellation.size()==256,15000);
    window.findChild<QPushButton*>("cancelNarrowbandAnalysis")->click();
    QVERIFY(!workspace->analysisResult());QCOMPARE(chart->property("constellationPointCount").toULongLong(),qulonglong{0});
    window.refresh();QTest::qWait(50);QVERIFY(!workspace->analysisResult());
    window.findChild<QPushButton*>("runNarrowbandAnalysis")->click();
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult()!=nullptr,15000);
    window.session().newProject();window.refresh();QVERIFY(!workspace->analysisResult());
    QCOMPARE(chart->property("constellationPointCount").toULongLong(),qulonglong{0});
  }
  void realEyeSymbolRateAndControls() {
    const auto node=qEnvironmentVariable("SS_NARROWBAND_NODE");if(node=="W1" || node=="W3")return;
    MainWindow window;populate(window);auto* file=window.session().activeFile();
    const auto id=window.session().createChannel("eye",file->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);
    QVERIFY(!id.empty());window.refresh();auto* workspace=window.findChild<NarrowbandWorkspace*>();workspace->activatePageForAcceptance(1);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult()!=nullptr,15000);
    QVERIFY(workspace->analysisResult()->eyeStatus==AnalysisStatus::NeedSymbolRate);
    auto* mode=window.findChild<QComboBox*>("analysisSymbolRateMode");auto* rs=window.findChild<QDoubleSpinBox*>("symbolRate");
    mode->setCurrentIndex(1);rs->setValue(625000);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->eyeStatus==AnalysisStatus::Ready && workspace->analysisResult()->symbolRateHz==625000,15000);
    auto* chart=window.findChild<QWidget*>("narrowbandEyeLargeChart");QVERIFY(chart->property("realEyeSource").toBool());
    QCOMPARE(chart->property("physicalSamplesPerSymbol").toDouble(),3.2);QVERIFY(!workspace->analysisResult()->timingLocked);
    window.findChild<QComboBox*>("eyePeriods")->setCurrentIndex(2);
    window.findChild<QComboBox*>("eyeTraces")->setCurrentIndex(2);
    window.findChild<QComboBox*>("eyeComponent")->setCurrentIndex(2);
    window.findChild<QComboBox*>("eyeDrawingDensity")->setCurrentIndex(1);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->eyes.size()==128 && workspace->analysisResult()->eyePeriods==3 && workspace->analysisResult()->eyePointsPerUi==8,15000);
    QCOMPARE(chart->property("eyeComponentCount").toInt(),1);QCOMPARE(workspace->analysisResult()->eyes[0].i.size(),std::size_t{25});
    window.findChild<QComboBox*>("eyeAnalysisStage")->setCurrentIndex(static_cast<int>(AnalysisStage::MatchedFiltered));
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->filterDiscardedSamples>0 && workspace->analysisResult()->eyeStatus==AnalysisStatus::Ready,15000);
    mode->setCurrentIndex(0);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->eyeStatus==AnalysisStatus::NeedSymbolRate,15000);
    QVERIFY(!chart->property("realEyeSource").toBool());QCOMPARE(chart->property("eyeTraceCount").toULongLong(),qulonglong{0});
  }
  void estimatedCandidatesAndManualTakeover() {
    const auto node=qEnvironmentVariable("SS_NARROWBAND_NODE");if(node=="W1" || node=="W3" || node=="W4")return;
    MainWindow window;populate(window,"qpsk-a35-awgn20.raw");auto* file=window.session().activeFile();
    const auto id=window.session().createChannel("rate",file->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);QVERIFY(!id.empty());
    window.refresh();auto* workspace=window.findChild<NarrowbandWorkspace*>();workspace->activatePageForAcceptance(1);
    auto* mode=window.findChild<QComboBox*>("analysisSymbolRateMode");mode->setCurrentIndex(2);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && !workspace->analysisResult()->rateCandidates.empty(),15000);
    auto* candidates=window.findChild<QComboBox*>("analysisRateCandidates");int selected=-1;
    for(int i=0;i<candidates->count();++i)if(std::abs(candidates->itemData(i).toDouble()-625000)/625000<.01){selected=i;break;}
    QVERIFY(selected>=0);candidates->setCurrentIndex(selected);window.findChild<QPushButton*>("adoptSymbolRateCandidate")->click();
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->eyeStatus==AnalysisStatus::Ready,15000);
    QCOMPARE(mode->currentIndex(),1);QCOMPARE(window.session().activeChannel()->analysis.symbolRateMode,SymbolRateMode::Manual);
    auto* rs=window.findChild<QDoubleSpinBox*>("symbolRate");rs->setValue(500000);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->symbolRateHz==500000,15000);
    QCOMPARE(window.session().activeChannel()->analysis.symbolRateHz,500000.0);
    window.findChild<QComboBox*>("modulationFormat")->setCurrentIndex(static_cast<int>(ModulationHypothesis::Ofdm));
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->eyeStatus==AnalysisStatus::NotApplicable,15000);
    QVERIFY(!workspace->analysisResult()->constellation.empty());
  }
  void realSynchronizationStagesAndManualCfo() {
    const auto node=qEnvironmentVariable("SS_NARROWBAND_NODE");if(node!="W6" && node!="W7" && node!="W8" && !node.isEmpty())return;
    MainWindow window;populate(window,"qpsk-a35-awgn20.raw");auto* file=window.session().activeFile();
    const auto id=window.session().createChannel("sync",file->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);QVERIFY(!id.empty());
    window.refresh();auto* workspace=window.findChild<NarrowbandWorkspace*>();workspace->activatePageForAcceptance(1);
    window.findChild<QComboBox*>("modulationFormat")->setCurrentIndex(static_cast<int>(ModulationHypothesis::Qpsk));
    window.findChild<QPushButton*>("recoverNarrowbandSignal")->click();
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->carrierLocked && workspace->analysisResult()->eyeStatus==AnalysisStatus::Ready,20000);
    const auto result=workspace->analysisResult();QVERIFY(result->timingLocked);QVERIFY(result->conditionalEvm<.18);
    QVERIFY(result->constellationStage==AnalysisStage::CarrierRecovered);QVERIFY(result->eyeStage==AnalysisStage::CarrierRecovered);
    auto* constellation=window.findChild<QComboBox*>("constellationAnalysisStage");
    for(const auto stage:{AnalysisStage::RawIq,AnalysisStage::CfoCorrected,AnalysisStage::MatchedFiltered,AnalysisStage::TimingAligned,AnalysisStage::CarrierRecovered}) {
      constellation->setCurrentIndex(static_cast<int>(stage));
      QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->constellationStage==stage,20000);
      QVERIFY(!workspace->analysisResult()->constellation.empty());
    }
    window.findChild<QDoubleSpinBox*>("analysisManualCfoHz")->setValue(15625);
    window.findChild<QComboBox*>("analysisCfoMode")->setCurrentIndex(static_cast<int>(CfoMode::Manual));
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->carrierLocked &&
      std::abs(workspace->analysisResult()->estimatedCfoHz-15625)<625,20000);
    QCOMPARE(window.session().activeChannel()->analysis.cfoMode,CfoMode::Manual);
  }
};
QTEST_MAIN(NarrowbandAnalysisUiTests)
#include "narrowband_analysis_ui_tests.moc"
