#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>
#include <cmath>
#include <QPushButton>
#include <QToolButton>
#include <QtTest>
#include "app/main_window.h"
#include "ui/narrowband_workspace.h"
#include "ui/import/signal_import_dialog.h"
#include "ui/controls/adaptive_value_edit.h"
#include "ui/chart_control_strip.h"

using namespace signalstudio;
class NarrowbandAnalysisUiTests : public QObject {
  Q_OBJECT
  void populate(MainWindow& window,const QString& name="tone.raw") {
    FileMetadata source;source.name=QFileInfo(name).fileName().toStdString();
    source.path=(QFileInfo(name).isAbsolute()?name:qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/"+name).toStdString();
    source.sampleRateHz=20e6;source.centerFrequencyHz=100e6;source.sampleCount=static_cast<SampleIndex>(QFileInfo(QString::fromStdString(source.path)).size()/4);source.demo=false;
    window.session().addDemoFile(source);
    window.session().addMark({{1000,200000},{101.5e6,102.5e6}});
    window.resize(2560,1440);window.show();window.refresh();
    QCoreApplication::processEvents();
  }
 private slots:
  void initTestCase() {
    qInfo("NARROWBAND_VALIDATION_NODE=%s",qPrintable(qEnvironmentVariable("SS_NARROWBAND_NODE")));
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
    auto* eyeStrip=dynamic_cast<ChartControlStrip*>(window.findChild<QWidget*>("narrowbandEyeControls"));
    auto* constellationStrip=dynamic_cast<ChartControlStrip*>(window.findChild<QWidget*>("narrowbandConstellationControls"));
    QVERIFY(eyeStrip && constellationStrip);
    for(auto* strip:{eyeStrip,constellationStrip}) {
      auto* head=strip->parentWidget();QCOMPARE(head->height(),30);QVERIFY(strip->overflowButton()->isVisible());
      QList<QRect> occupied;
      for(auto* field:strip->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly)) {
        if(!field->isVisible())continue;
        const QRect rectangle(field->mapTo(head,QPoint()),field->size());
        QVERIFY2(head->rect().contains(rectangle),qPrintable(field->objectName()));
        for(const auto& previous:occupied)QVERIFY2(!rectangle.intersects(previous),qPrintable(field->objectName()));
        occupied.append(rectangle);
      }
    }
    auto* rolloff=window.findChild<QDoubleSpinBox*>("analysisRrcRolloff");
    auto* offset=window.findChild<QDoubleSpinBox*>("eyeTimingOffsetUi");
    QVERIFY(rolloff && offset && !rolloff->isVisible());
    QTest::mouseClick(eyeStrip->overflowButton(),Qt::LeftButton);QTRY_VERIFY(rolloff->isVisible());
    for(auto* spin:{rolloff,offset}) {
      auto* editor=spin->findChild<QLineEdit*>();QVERIFY(editor);
      QVERIFY(editor->contentsRect().width()>=editor->fontMetrics().horizontalAdvance(editor->displayText()));
    }
    auto* editor=rolloff->findChild<QLineEdit*>();const auto accepted=window.session().activeChannel()->analysis.rolloff;
    auto* originalParent=rolloff->parentWidget();editor->setFocus();editor->selectAll();QTest::keyClicks(editor,"0.40");
    window.refresh();QCOMPARE(rolloff->parentWidget(),originalParent);QVERIFY(editor->text().contains("0.40"));
    QCOMPARE(window.session().activeChannel()->analysis.rolloff,accepted);
    QTest::keyClick(editor,Qt::Key_Return);QCOMPARE(window.session().activeChannel()->analysis.rolloff,.4);
    QTest::mouseClick(constellationStrip->overflowButton(),Qt::LeftButton);
    QTRY_VERIFY(constellationStrip->popoverFrame()->isVisible());QVERIFY(!eyeStrip->popoverFrame()->isVisible());
    auto* axis=window.findChild<QDoubleSpinBox*>("constellationAxisMinimum");QVERIFY(axis && axis->isVisible() && !axis->isEnabled());
    QTest::mouseClick(window.findChild<QCheckBox*>("constellationAutoScale"),Qt::LeftButton);QVERIFY(axis->isEnabled());
    QTRY_VERIFY(!constellationStrip->popoverFrame()->isVisible());
    QTest::mouseClick(constellationStrip->overflowButton(),Qt::LeftButton);QTRY_VERIFY(axis->isVisible());
    editor=axis->findChild<QLineEdit*>();editor->setFocus();editor->selectAll();QTest::keyClicks(editor,"-1.5");
    window.refresh();QVERIFY(editor->text().startsWith("-1.5"));QTest::keyClick(editor,Qt::Key_Return);
    QCOMPARE(window.session().activeChannel()->constellationMinimum,-1.5);
    editor->selectAll();QTest::keyClicks(editor,"-2.5");
    const auto other=window.session().createChannel("other",window.session().activeFile()->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);
    QVERIFY(!other.empty());window.refresh();
    QTRY_VERIFY(!constellationStrip->popoverFrame()->isVisible());QVERIFY(!axis->property("parameterDraft").toBool());
    QVERIFY(window.session().activateChannel(id));window.refresh();
    QCOMPARE(window.session().activeChannel()->constellationMinimum,-1.5);
    workspace->activatePageForAcceptance(1);
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
    window.findChild<QPushButton*>("recoverNarrowbandSignal")->click();
    QVERIFY(window.findChild<QLabel*>("modulationAnalysisStatus")->text().contains("请先选择"));
    QCOMPARE(window.findChild<QPushButton*>("recoverNarrowbandSignal")->text(),QString("同步恢复"));
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
    auto* mode=window.findChild<QComboBox*>("analysisSymbolRateMode");mode->setCurrentIndex(static_cast<int>(SymbolRateMode::Manual));
    window.findChild<QDoubleSpinBox*>("symbolRate")->setValue(625000);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->carrierLocked,20000);
    mode->setCurrentIndex(static_cast<int>(SymbolRateMode::Unknown));
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->eyeStatus==AnalysisStatus::NeedSymbolRate,15000);
    QVERIFY(!workspace->analysisResult()->timingLocked && !workspace->analysisResult()->carrierLocked);
    QVERIFY(workspace->analysisResult()->synchronizedSymbols.empty());QVERIFY(!workspace->analysisResult()->constellation.empty());
    mode->setCurrentIndex(static_cast<int>(SymbolRateMode::Manual));
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->carrierLocked && !workspace->analysisResult()->eyes.empty(),20000);
    const auto before=workspace->analysisResult()->eyes[0].outputStart;
    window.findChild<QDoubleSpinBox*>("eyeTimingOffsetUi")->setValue(.25);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && !workspace->analysisResult()->eyes.empty() &&
      (workspace->analysisResult()->eyes[0].outputStart.integer!=before.integer || std::abs(workspace->analysisResult()->eyes[0].outputStart.fraction-before.fraction)>.1),20000);
    QCOMPARE(window.session().activeChannel()->analysis.eyeTimingOffsetUi,.25);
    window.findChild<QCheckBox*>("analysisTimingRecovery")->setChecked(false);
    QVERIFY(!window.session().activeChannel()->analysis.carrierRecovery);
    QVERIFY(window.session().activeChannel()->analysis.constellationStage<AnalysisStage::TimingAligned);
    window.findChild<QCheckBox*>("analysisMatchedFilter")->setChecked(false);
    QVERIFY(window.session().activeChannel()->analysis.constellationStage==AnalysisStage::RawIq);
    QVERIFY(window.session().activeChannel()->analysis.eyeStage==AnalysisStage::RawIq);
    for(const auto& family:std::array<std::pair<ModulationHypothesis,const char*>,3>{{
      {ModulationHypothesis::Psk,"psk8-awgn20"},{ModulationHypothesis::Qam,"qam32-awgn20"},{ModulationHypothesis::Apsk,"apsk16-awgn20"}}}) {
      MainWindow expanded;const auto path=qEnvironmentVariable("SS_MODULATION_FIXTURES")+'/'+family.second+".raw";
      populate(expanded,path);auto* f=expanded.session().activeFile();
      const auto channelId=expanded.session().createChannel("family",f->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);QVERIFY(!channelId.empty());
      expanded.refresh();auto* work=expanded.findChild<NarrowbandWorkspace*>();work->activatePageForAcceptance(1);
      expanded.findChild<QComboBox*>("modulationFormat")->setCurrentIndex(static_cast<int>(family.first));
      auto* order=expanded.findChild<QComboBox*>("analysisModulationOrder");QVERIFY(order->isVisible());
      QCOMPARE(order->currentData().toInt(),family.first==ModulationHypothesis::Psk?8:family.first==ModulationHypothesis::Qam?32:16);
      if(family.first==ModulationHypothesis::Qam)QVERIFY(order->findData(4096)>=0);
      expanded.findChild<QPushButton*>("recoverNarrowbandSignal")->click();
      QTRY_VERIFY_WITH_TIMEOUT(work->analysisResult() && work->analysisResult()->carrierLocked,20000);
      QVERIFY(work->analysisResult()->constellationStage==AnalysisStage::CarrierRecovered);
      auto* chart=expanded.findChild<QWidget*>("narrowbandConstellationLargeChart");QVERIFY(!chart->property("analysisStageFallback").toBool());
      if(family.first==ModulationHypothesis::Apsk) {
        expanded.findChild<QToolButton*>("narrowbandSyncSettingsToggle")->setChecked(true);
        auto* rings=expanded.findChild<QLineEdit*>("analysisApskRings");QVERIFY(rings->isEnabled());rings->setFocus();
        QTest::keyClicks(rings,"4:1:45,12:2.85:15");expanded.refresh();
        QVERIFY(expanded.session().activeChannel()->analysis.apskRings.empty());QVERIFY(rings->text().contains("2.85"));
        QTest::keyClick(rings,Qt::Key_Return);QCOMPARE(expanded.session().activeChannel()->analysis.apskRings,std::string("4:1:45,12:2.85:15"));
        QTRY_VERIFY_WITH_TIMEOUT(work->analysisResult() && work->analysisResult()->carrierLocked,20000);
      }
      QTemporaryDir saved;const auto expected=expanded.session().activeChannel()->analysis;
      QVERIFY(expanded.saveProject(saved.filePath("project.json")));QVERIFY(expanded.openProject(saved.filePath("project.json")));
      QCOMPARE(expanded.session().activeChannel()->analysis,expected);
    }
  }
  void importToAnalysisSaveReopen_data() {
    QTest::addColumn<QString>("suffix");QTest::newRow("CI16-RAW")<<QString("raw");QTest::newRow("CF32-SigMF")<<QString("sigmf-meta");
  }
  void externalSourceChangeInvalidatesIdleAndPending() {
    const auto node=qEnvironmentVariable("SS_NARROWBAND_NODE");if(node!="W7" && node!="W8" && !node.isEmpty())return;
    QTemporaryDir directory;const auto path=directory.filePath("mutable.raw");
    QVERIFY(QFile::copy(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/qpsk-a35-awgn20.raw",path));
    MainWindow window;populate(window,path);auto* file=window.session().activeFile();
    const auto id=window.session().createChannel("mutable",file->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);QVERIFY(!id.empty());window.refresh();
    auto* workspace=window.findChild<NarrowbandWorkspace*>();workspace->activatePageForAcceptance(1);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult()!=nullptr,15000);QVERIFY(!workspace->analysisResult()->constellation.empty());
    const auto change=[&]{QFile out(path);if(!out.open(QIODevice::Append))return false;return out.write(QByteArray(4,'\0'))==4;};
    QVERIFY(change());
    QTRY_VERIFY_WITH_TIMEOUT(!workspace->analysisResult(),2000);
    QCOMPARE(window.findChild<QWidget*>("narrowbandConstellationLargeChart")->property("constellationPointCount").toULongLong(),qulonglong{0});
    QVERIFY(window.findChild<QLabel*>("modulationAnalysisStatus")->text().contains("失效"));
    window.findChild<QPushButton*>("runNarrowbandAnalysis")->click();QVERIFY(change());
    QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(),15000);
    QVERIFY(!workspace->analysisResult());QVERIFY(window.findChild<QLabel*>("modulationAnalysisStatus")->text().contains("失效"));
    // Ordinary refresh must catch the change synchronously, before the 20 ms
    // result timer can invalidate either a published or an in-flight result.
    window.findChild<QPushButton*>("runNarrowbandAnalysis")->click();
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult()!=nullptr,15000);
    QVERIFY(change());window.refresh();
    QVERIFY(!workspace->analysisResult());
    QVERIFY(window.findChild<QLabel*>("modulationAnalysisStatus")->text().contains("失效"));
    window.refresh();QTest::qWait(50);QVERIFY(!workspace->analysisResult());
    window.findChild<QPushButton*>("runNarrowbandAnalysis")->click();
    QVERIFY(change());window.refresh();
    QVERIFY(!workspace->analysisResult());
    QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(),15000);
    QVERIFY(!workspace->analysisResult());
    QVERIFY(window.findChild<QLabel*>("modulationAnalysisStatus")->text().contains("失效"));
  }
  void importToAnalysisSaveReopen() {
    const auto node=qEnvironmentVariable("SS_NARROWBAND_NODE");if(node!="W7" && node!="W8" && !node.isEmpty())return;
    QFETCH(QString,suffix);QTemporaryDir directory;MainWindow window;QString error;
    QVERIFY(window.createProject(directory.path(),"real-narrowband",&error));window.resize(2560,1440);window.show();
    SignalImportDialog import("real-narrowband",&window);import.show();
    QVERIFY(import.addPath(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/qpsk-a35-awgn20."+suffix));
    if(suffix=="raw") {
      for(const auto& entry:std::array<std::pair<const char*,const char*>,3>{{{"importSampleRate","20 MS/s"},{"importCenterFrequency","100 MHz"},{"importBandwidth","20 MHz"}}}) {
        auto* edit=import.findChild<AdaptiveValueEdit*>(entry.first);QVERIFY(edit);edit->setFocus();edit->selectAll();QTest::keyClicks(edit,entry.second);QTest::keyClick(edit,Qt::Key_Return);
      }
      import.findChild<QCheckBox*>("importConfirmFormat")->setChecked(true);
    }
    import.commitSource=[&](FileState file,QString& e){return window.addImportedSource(std::move(file),e);};import.startImport();
    auto* finish=import.findChild<QPushButton*>("importProgressFinish");QTRY_VERIFY_WITH_TIMEOUT(finish->isEnabled(),15000);finish->click();
    auto* file=window.session().activeFile();QVERIFY(file && !file->metadata.demo);QCOMPARE(file->metadata.sampleRateHz,20e6);
    QCOMPARE(file->metadata.sampleFormat.componentEncoding,suffix=="raw"?ComponentEncoding::Int16:ComponentEncoding::Float32);
    QVERIFY(!file->navigationEnvelope.empty());QVERIFY(availableSamples(file->metadata)==file->metadata.sampleCount);
    window.session().addMark({{0,file->metadata.sampleCount},{101.5e6,102.5e6}});window.refresh();
    window.findChild<QPushButton*>("extract")->click();auto* dialog=window.findChild<QDialog*>("channelConfigDialog");QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QLabel*>("channelDspPlanPreview")->property("realizedRateHz").toDouble(),2e6);
    dialog->findChild<QPushButton*>("confirmChannelConfig")->click();
    QTRY_VERIFY_WITH_TIMEOUT(window.session().activeChannel()!=nullptr,15000);
    auto* workspace=window.findChild<NarrowbandWorkspace*>();workspace->activatePageForAcceptance(1);
    window.findChild<QComboBox*>("modulationFormat")->setCurrentIndex(static_cast<int>(ModulationHypothesis::Qpsk));
    window.findChild<QPushButton*>("recoverNarrowbandSignal")->click();
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->carrierLocked,20000);
    const auto saved=*window.session().activeChannel();const auto projectPath=directory.filePath("real-narrowband/project.json");
    QVERIFY(window.saveProject(projectPath));QVERIFY(window.openProject(projectPath));workspace->activatePageForAcceptance(1);
    QCOMPARE(window.session().activeChannel()->analysis,saved.analysis);
    QCOMPARE(window.session().activeChannel()->sampling,saved.sampling);
    QCOMPARE(window.session().activeChannel()->sourceTime,saved.sourceTime);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->carrierLocked,20000);
    const auto second=window.session().createChannel("second",window.session().activeFile()->activeMarkId,102e6,1e6,2e6,saved.sourceTime,ChannelFilter::Standard,false,true);
    QVERIFY(!second.empty());window.refresh();workspace->activatePageForAcceptance(1);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->identity.channelId==second,15000);
    QVERIFY(!workspace->analysisResult()->timingLocked);QVERIFY(workspace->analysisResult()->eyes.empty());
    QVERIFY(window.session().activateChannel(saved.id));window.refresh();workspace->activatePageForAcceptance(1);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->identity.channelId==saved.id && workspace->analysisResult()->carrierLocked,20000);
  }
};
QTEST_MAIN(NarrowbandAnalysisUiTests)
#include "narrowband_analysis_ui_tests.moc"
