#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFontMetricsF>
#include <QSet>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <cmath>
#include <QPushButton>
#include <QToolButton>
#include <QWheelEvent>
#include <QtTest>
#include "app/main_window.h"
#include "ui/narrowband_workspace.h"
#include "ui/import/signal_import_dialog.h"
#include "ui/controls/adaptive_value_edit.h"
#include "ui/chart_control_strip.h"
#include "infrastructure/project_store.h"
#include "domain/constellation_observation.h"

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
  static QRectF layoutRect(const QJsonValue& value) {
    const auto r=value.toObject();return {r["x"].toDouble(),r["y"].toDouble(),r["width"].toDouble(),r["height"].toDouble()};
  }
  void verifyCompactLayout(const QJsonObject& layout,NarrowbandWorkspace& workspace,bool requireGain=true) {
    const auto name=layout["chart"].toString();auto* chart=workspace.findChild<QWidget*>(name);QVERIFY(chart);
    const QRectF canvas(QPointF(),chart->size());const auto plot=layoutRect(layout["plotRect"]);
    QVERIFY2(canvas.contains(plot),qPrintable(name));QCOMPARE(plot,chart->property("plotRect").toRectF());
    const auto old=layoutRect(layout["baselinePlotRect"]);
    QCOMPARE(old.width()*old.height(),layout["baselineArea"].toDouble());
    QCOMPARE(plot.width()*plot.height()-old.width()*old.height(),layout["areaGain"].toDouble());
    if(requireGain)QVERIFY2(layout["areaGain"].toDouble()>0,qPrintable(name));
    QCOMPARE(layout["xTitleBand"].toInt(),0);QCOMPARE(layout["yTitleBand"].toInt(),0);
    QFont tickFont;QVERIFY(tickFont.fromString(layout["tickFont"].toString()));const QFontMetricsF metrics(tickFont,chart);
    QList<QRectF> occupied;
    for(const auto* axis:{"xTicks","yTicks"}) {
      const auto ticks=layout[axis].toArray();QVERIFY(ticks.size()>=2);QString previous;
      for(const auto value:ticks) {
        const auto tick=value.toObject();const auto ink=layoutRect(tick["inkRect"]);const auto text=tick["text"].toString();
        const QPointF baseline(tick["baselineX"].toDouble(),tick["baselineY"].toDouble());
        QVERIFY(ink.contains(metrics.tightBoundingRect(text).translated(baseline)));
        QVERIFY2(canvas.contains(ink) && !plot.intersects(ink),qPrintable(name+": "+text));
        for(const auto& other:occupied)QVERIFY2(!ink.intersects(other),qPrintable(name+": overlapping ticks"));
        occupied.append(ink);QVERIFY(!text.isEmpty() && text!=previous);previous=text;
      }
    }
    if(layout["equalAxisScale"].toBool()){QCOMPARE(plot.width(),plot.height());QCOMPARE(plot,chart->property("iqPlotRect").toRectF());}
    if(!chart->property("heatmapDrawRect").toRectF().isEmpty())QCOMPARE(plot,chart->property("heatmapDrawRect").toRectF());
    auto* label=workspace.findChild<QLabel*>(name+"AxisHeader");QVERIFY(label && label->isVisible());
    QCOMPARE(label->parentWidget()->height(),30);QCOMPARE(label->toolTip(),chart->property("axisDescriptionText").toString());
    QVERIFY(!label->toolTip().isEmpty());QVERIFY(label->width()>=label->minimumSizeHint().width());
    const QFontMetricsF headerMetrics(label->font(),label);
    QVERIFY2(headerMetrics.horizontalAdvance(label->text())<=label->contentsRect().width()+.5,qPrintable(name+": "+label->text()));
    QVERIFY(headerMetrics.tightBoundingRect(label->text()).height()<=label->contentsRect().height()+.5);
    const QRect labelRect(label->mapTo(label->parentWidget(),QPoint()),label->size());
    for(auto* sibling:label->parentWidget()->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly))
      if(sibling!=label && sibling->isVisible())QVERIFY(!labelRect.intersects(QRect(sibling->mapTo(label->parentWidget(),QPoint()),sibling->size())));
  }
 private slots:
  void initTestCase() {
    qInfo("NARROWBAND_VALIDATION_NODE=%s",qPrintable(qEnvironmentVariable("SS_NARROWBAND_NODE")));
    qApp->setProperty("softwareRenderer",true);
    qApp->setProperty("uiStatePersistenceEnabled",false);
  }
  void compactAxesCoverEveryPageAndPreserveAnalysis() {
    MainWindow window;populate(window,"qpsk-a35-awgn20.raw");
    const auto id=window.session().createChannel("compact",window.session().activeFile()->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);
    QVERIFY(!id.empty());window.refresh();auto* workspace=window.findChild<NarrowbandWorkspace*>();QVERIFY(workspace);
    workspace->activatePage(NarrowbandPage::Constellation);
    window.findChild<QComboBox*>("analysisSymbolRateMode")->setCurrentIndex(1);
    window.findChild<QDoubleSpinBox*>("symbolRate")->setValue(625000);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->eyeStatus==AnalysisStatus::Ready,15000);
    const auto snapshot=workspace->analysisResult();const auto generation=workspace->renderStatistics().value("analysisGeneration");
    QSet<QString> seen;QSet<int> modes;
    for(const auto page:{NarrowbandPage::Observe,NarrowbandPage::Constellation,NarrowbandPage::Eye,NarrowbandPage::Recognition,NarrowbandPage::Demodulation}) {
      workspace->activatePage(page);QTest::qWait(100);workspace->grab();
      for(const auto value:workspace->renderStatistics()["chartLayouts"].toArray()) {
        const auto layout=value.toObject();if(!layout["visible"].toBool())continue;
        verifyCompactLayout(layout,*workspace);seen.insert(layout["chart"].toString());modes.insert(layout["mode"].toInt());
      }
      QCOMPARE(workspace->analysisResult().get(),snapshot.get());QCOMPARE(workspace->renderStatistics()["analysisGeneration"],generation);
    }
    QCOMPARE(seen.size(),10);QCOMPARE(modes.size(),6);
    QVERIFY(window.findChild<QLabel*>("demodulationUnavailable")->text().contains("合成"));
  }
  void compactRfTicksKeepNarrowRangePrecision() {
    MainWindow window;populate(window,"qpsk-a35-awgn20.raw");
    const auto id=window.session().createChannel("rf-ticks",window.session().activeFile()->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);
    QVERIFY(!id.empty());window.refresh();
    auto* workspace=window.findChild<NarrowbandWorkspace*>();QVERIFY(workspace);
    workspace->activatePage(NarrowbandPage::Observe);
    auto* frequency=window.findChild<QComboBox*>("channelFrequencyMode");QVERIFY(frequency);frequency->setCurrentIndex(1);
    window.session().activeChannel()->visibleBasebandFrequency={-.0005,.0005};window.refresh();
    QCoreApplication::processEvents();workspace->grab();
    int checked=0;
    for(const auto value:workspace->renderStatistics()["chartLayouts"].toArray()) {
      const auto layout=value.toObject();if(!layout["visible"].toBool())continue;
      const bool spectrum=layout["chart"].toString()=="narrowbandPsdPanelChart";
      const bool heatmap=layout["chart"].toString()=="narrowbandStftPanelChart";
      if(!spectrum && !heatmap)continue;
      const auto ticks=layout[spectrum?"xTicks":"yTicks"].toArray();QVERIFY(ticks.size()>=2);
      QSet<QString> labels;
      const double step=std::abs(ticks.last().toObject()["value"].toDouble()-ticks.first().toObject()["value"].toDouble())/(ticks.size()-1);
      QVERIFY(step>0 && step<1e-8);
      for(const auto tickValue:ticks) {
        const auto tick=tickValue.toObject();const auto text=tick["text"].toString();
        QVERIFY2(!labels.contains(text),qPrintable(text));labels.insert(text);
        bool ok=false;const auto parsed=text.toDouble(&ok);QVERIFY(ok);
        QVERIFY(std::abs(parsed-tick["value"].toDouble())<step*.1);
      }
      ++checked;
    }
    QCOMPARE(checked,2);
  }

  void compactAxesFontFrequencyAndTimeLabelsReuseCaches() {
    MainWindow window;populate(window,"qpsk-a35-awgn20.raw");
    const auto id=window.session().createChannel("compact-labels",window.session().activeFile()->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);
    QVERIFY(!id.empty());window.refresh();auto* workspace=window.findChild<NarrowbandWorkspace*>();QVERIFY(workspace);
    workspace->activatePage(NarrowbandPage::Constellation);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult()!=nullptr,15000);
    workspace->activatePage(NarrowbandPage::Observe);
    QTRY_VERIFY_WITH_TIMEOUT(!workspace->renderStatistics()["pending"].toBool() && window.session().spectrogram(id)!=nullptr,15000);
    QTest::qWait(200);
    const auto before=workspace->renderStatistics();const auto snapshot=workspace->analysisResult();const auto spectrum=window.session().spectrogram(id);
    auto* frequency=window.findChild<QComboBox*>("channelFrequencyMode");QVERIFY(frequency);frequency->setCurrentIndex(1);
    auto* channel=window.session().activeChannel();channel->relativeTime=true;window.refresh();
    auto* wave=window.findChild<QWidget*>("narrowbandWaveformPanelChart");auto* heat=window.findChild<QWidget*>("narrowbandStftPanelChart");QVERIFY(wave && heat);
    const auto oldPlot=wave->property("plotRect").toRectF();
    wave->setStyleSheet("background:#111e2e;font-family:Consolas;font-size:18px;");
    QTest::qWait(100);workspace->grab();
    QVERIFY(wave->property("plotRect").toRectF().left()>oldPlot.left());
    QVERIFY(wave->property("axisDescriptionText").toString().contains("源样本 #1000"));
    QVERIFY(wave->property("xAxisLabel").toString().contains("相对时间"));
    QVERIFY(heat->property("yAxisLabel").toString().contains("RF"));
    for(const auto value:workspace->renderStatistics()["chartLayouts"].toArray())
      if(value.toObject()["visible"].toBool())verifyCompactLayout(value.toObject(),*workspace,false);
    const auto after=workspace->renderStatistics();
    for(const auto* key:{"requestGeneration","frameGeneration","psdGeneration","analysisGeneration","iqCacheHits","iqCacheMisses","iqCacheBytes"})QCOMPARE(after[key],before[key]);
    QCOMPARE(workspace->analysisResult().get(),snapshot.get());QCOMPARE(window.session().spectrogram(id).get(),spectrum.get());
    const auto plot=heat->property("plotRect").toRectF();QVERIFY(plot.width()>10);
    const auto point=QPointF(plot.left()+plot.width()*.37,plot.top()+plot.height()*.61).toPoint();
    const double x=(point.x()-plot.left())/plot.width(),y=(plot.bottom()-point.y())/plot.height();
    const auto time=channel->visibleSourceTime;const auto frequencyRange=channel->visibleBasebandFrequency;
    QTest::mouseClick(heat,Qt::LeftButton,Qt::NoModifier,point);
    const auto cursor=window.session().linkedCursor(id);QVERIFY(cursor.pinned);
    QCOMPARE(cursor.sourceSample,time.begin+static_cast<SampleIndex>(x*(time.end-time.begin)));
    QVERIFY(std::abs(cursor.frequencyHz-(frequencyRange.lowerHz+y*(frequencyRange.upperHz-frequencyRange.lowerHz)))<1e-6);
    const auto a=QPointF(plot.left()+plot.width()*.2,plot.top()+plot.height()*.25).toPoint();
    const auto b=QPointF(plot.left()+plot.width()*.8,plot.top()+plot.height()*.75).toPoint();
    QTest::mousePress(heat,Qt::LeftButton,Qt::NoModifier,a);QTest::mouseMove(heat,b);QTest::mouseRelease(heat,Qt::LeftButton,Qt::NoModifier,b);
    const auto selected=window.session().activeChannel()->visibleSourceTime;
    const auto expectedBegin=time.begin+static_cast<SampleIndex>((a.x()-plot.left())/plot.width()*(time.end-time.begin));
    const auto expectedEnd=time.begin+static_cast<SampleIndex>((b.x()-plot.left())/plot.width()*(time.end-time.begin));
    QVERIFY(std::abs(double(selected.begin)-double(expectedBegin))<=1);QVERIFY(std::abs(double(selected.end)-double(expectedEnd))<=1);
    const auto selectedFrequency=window.session().activeChannel()->visibleBasebandFrequency;
    QVERIFY(std::abs(selectedFrequency.lowerHz-(frequencyRange.upperHz-(b.y()-plot.top())/plot.height()*(frequencyRange.upperHz-frequencyRange.lowerHz)))<1e-6);
    QVERIFY(std::abs(selectedFrequency.upperHz-(frequencyRange.upperHz-(a.y()-plot.top())/plot.height()*(frequencyRange.upperHz-frequencyRange.lowerHz)))<1e-6);
  }
  void independentPagesShareAnalysisAndKeepDisplayState() {
    MainWindow window; populate(window,"qpsk-a35-awgn20.raw");
    auto* file=window.session().activeFile();
    const auto id=window.session().createChannel("pages",file->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);
    QVERIFY(!id.empty()); window.refresh();
    auto* workspace=window.findChild<NarrowbandWorkspace*>(); QVERIFY(workspace);
    auto* stack=window.findChild<QStackedWidget*>("narrowbandPageStack"); QVERIFY(stack); QCOMPARE(stack->count(),5);
    auto* rateMode=window.findChild<QComboBox*>("analysisSymbolRateMode");
    auto* rate=window.findChild<QDoubleSpinBox*>("symbolRate");
    auto* pointBudget=window.findChild<QComboBox*>("constellationPointBudget");
    auto* autoScale=window.findChild<QCheckBox*>("constellationAutoScale");
    auto* axisMinimum=window.findChild<QDoubleSpinBox*>("constellationAxisMinimum");
    auto* axisMaximum=window.findChild<QDoubleSpinBox*>("constellationAxisMaximum");
    auto* eyeComponent=window.findChild<QComboBox*>("eyeComponent");
    auto* eyePeriods=window.findChild<QComboBox*>("eyePeriods");
    auto* eyeTraces=window.findChild<QComboBox*>("eyeTraces");
    auto* eyeDensity=window.findChild<QComboBox*>("eyeDrawingDensity");
    auto* runAnalysis=window.findChild<QPushButton*>("runNarrowbandAnalysis");
    auto* runRecognition=window.findChild<QPushButton*>("runRealRecognition");
    auto* recognitionUnavailable=window.findChild<QLabel*>("recognitionUnavailable");
    auto* demodulationUnavailable=window.findChild<QLabel*>("demodulationUnavailable");
    auto* summary=window.findChild<QLabel*>("narrowbandAnalysisSummary");
    QVERIFY(rateMode && rate && pointBudget && autoScale && axisMinimum && axisMaximum);
    QVERIFY2(eyeComponent && eyePeriods && eyeTraces && eyeDensity,
             "Eye editors must belong to the window and remain discoverable when hosted in chart settings");
    QVERIFY(runAnalysis && runRecognition && recognitionUnavailable && demodulationUnavailable && summary);
    workspace->activatePage(NarrowbandPage::Constellation);
    workspace->setSyncPanelExpanded(true);
    rateMode->setCurrentIndex(1);
    rate->setValue(625000);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->eyeStatus==AnalysisStatus::Ready,15000);
    const auto snapshot=workspace->analysisResult();
    const auto generation=workspace->renderStatistics()["analysisGeneration"].toString();
    auto* constellation=window.findChild<QWidget*>("narrowbandConstellationLargeChart");
    auto* eye=window.findChild<QWidget*>("narrowbandEyeLargeChart");
    auto* panel=window.findChild<QScrollArea*>("narrowbandSharedSyncPanel");
    QVERIFY(constellation && eye && panel);
    const std::array<std::pair<const char*,NarrowbandPage>,5> destinations{{
      {"eyePage",NarrowbandPage::Eye},{"observePage",NarrowbandPage::Observe},
      {"deepLearningPage",NarrowbandPage::Recognition},{"demodulationPage",NarrowbandPage::Demodulation},
      {"modulationPage",NarrowbandPage::Constellation}}};
    for(const auto& [button,page]:destinations) {
      auto* navigation=window.findChild<QPushButton*>(button);QVERIFY(navigation);
      QTest::mouseClick(navigation,Qt::LeftButton);
      QTRY_COMPARE(workspace->currentPageIndex(),static_cast<int>(page));
      QCOMPARE(window.session().activeChannel()->page,page);
      QCOMPARE(constellation->isVisible(),page==NarrowbandPage::Constellation);
      QCOMPARE(eye->isVisible(),page==NarrowbandPage::Eye);
      QVERIFY(panel->isVisible());
      QCOMPARE(workspace->analysisResult().get(),snapshot.get());
      QCOMPARE(workspace->renderStatistics()["analysisGeneration"].toString(),generation);
    }
    QVERIFY(!runRecognition->isEnabled());
    QVERIFY(recognitionUnavailable->text().contains("不可用"));
    QVERIFY(demodulationUnavailable->text().contains("不可用"));
    pointBudget->setCurrentIndex(0);
    autoScale->setChecked(false);
    axisMinimum->setValue(-2);
    axisMaximum->setValue(2);
    workspace->activatePage(NarrowbandPage::Eye);
    eyeComponent->setCurrentIndex(2);
    eyeTraces->setCurrentIndex(0);
    eyeDensity->setCurrentIndex(0);
    QTest::qWait(50);
    QCOMPARE(workspace->analysisResult().get(),snapshot.get());
    QCOMPARE(workspace->renderStatistics()["analysisGeneration"].toString(),generation);
    QCOMPARE(eye->property("eyeTraceCount").toULongLong(),qulonglong{32});
    QCOMPARE(eye->property("eyePointsPerUi").toInt(),4);
    QCOMPARE(eye->property("eyeComponentCount").toInt(),1);
    workspace->setSyncPanelExpanded(false); workspace->activatePage(NarrowbandPage::Constellation);
    QVERIFY(!panel->isVisible()); QCOMPARE(constellation->property("iqAxisMinimum").toDouble(),-2.);
    QCOMPARE(constellation->property("iqAxisMaximum").toDouble(),2.);
    workspace->activatePage(NarrowbandPage::Recognition);
    rate->setValue(500000);
    QVERIFY(!workspace->analysisResult()); // Old graph cannot remain current while a request is pending.
    workspace->activatePage(NarrowbandPage::Eye);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->symbolRateHz==500000,15000);
    QVERIFY(workspace->analysisResult()->identity.requestGeneration>snapshot->identity.requestGeneration);
    runAnalysis->click();
    workspace->activatePage(NarrowbandPage::Constellation);
    workspace->cancelAnalysis(true);
    workspace->activatePage(NarrowbandPage::Eye);window.refresh();QTest::qWait(100);
    QVERIFY(!workspace->analysisResult());QCOMPARE(eye->property("eyeTraceCount").toULongLong(),qulonglong{0});
    QCOMPARE(constellation->property("constellationPointCount").toULongLong(),qulonglong{0});
    QVERIFY(summary->text().contains("取消"));
  }
  void conditionalDecisionsReferenceAndIqCoordinates() {
    MainWindow window;populate(window,"qpsk-a35-awgn20.raw");auto* file=window.session().activeFile();
    const auto id=window.session().createChannel("decision",file->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);
    QVERIFY(!id.empty());window.refresh();
    auto* workspace=window.findChild<NarrowbandWorkspace*>();workspace->activatePage(NarrowbandPage::Constellation);
    auto* stage=window.findChild<QComboBox*>("constellationAnalysisStage");
    auto* reference=window.findChild<QCheckBox*>("constellationReferenceRegions");
    auto* chart=window.findChild<QWidget*>("narrowbandConstellationLargeChart");QVERIFY(stage && reference && chart);
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult()!=nullptr,15000);
    const auto raw=workspace->analysisResult();const auto rawGeneration=workspace->renderStatistics()["analysisGeneration"].toString();
    stage->setCurrentIndex(5);reference->setChecked(true);window.refresh();QTest::qWait(50);
    QCOMPARE(workspace->analysisResult().get(),raw.get());QCOMPARE(workspace->renderStatistics()["analysisGeneration"].toString(),rawGeneration);
    QVERIFY(chart->property("analysisStageFallback").toBool());QVERIFY(!chart->property("decisionAvailable").toBool());
    QCOMPARE(chart->property("referenceCellCount").toInt(),0);QVERIFY(chart->property("decisionMessage").toString().contains("假设"));
    window.findChild<QComboBox*>("modulationFormat")->setCurrentIndex(static_cast<int>(ModulationHypothesis::Qpsk));
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult()!=nullptr,15000);
    QVERIFY(!chart->property("decisionAvailable").toBool());QVERIFY(!chart->property("decisionVisible").toBool());
    window.findChild<QPushButton*>("recoverNarrowbandSignal")->click();
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->carrierLocked,20000);
    const auto snapshot=workspace->analysisResult();const auto generation=workspace->renderStatistics()["analysisGeneration"].toString();
    stage->setCurrentIndex(5);reference->setChecked(true);
    QVERIFY(chart->property("decisionVisible").toBool());QCOMPARE(chart->property("referenceTemplateCount").toInt(),4);
    QTRY_COMPARE_WITH_TIMEOUT(chart->property("referenceCellCount").toInt(),4,5000);
    const auto observed=observeConstellation(*snapshot,window.session().activeChannel()->analysis);
    QVERIFY(observed.decisionAvailable);QVERIFY(!observed.decisions.empty());
    QCOMPARE(observed.samples.front().output,snapshot->synchronizedSymbols.front().output);
    QCOMPARE(observed.decisions.front().source,snapshot->synchronizedSymbols.front().source);
    const auto input=observed.samples.front().value;const auto decision=observed.decisions.front().value;
    QCOMPARE(chart->property("decisionInput").toPointF(),QPointF(input.real(),input.imag()));
    QCOMPARE(chart->property("decisionOutput").toPointF(),QPointF(decision.real(),decision.imag()));
    QVERIFY(chart->property("decisionMessage").toString().contains("绝对相位"));
    for(int i=0;i<6;++i){stage->setCurrentIndex(i);reference->setChecked(i%2==0);window.refresh();}
    stage->setCurrentIndex(5);reference->setChecked(true);
    auto* minimum=window.findChild<QDoubleSpinBox*>("constellationAxisMinimum");auto* maximum=window.findChild<QDoubleSpinBox*>("constellationAxisMaximum");
    window.findChild<QCheckBox*>("constellationAutoScale")->setChecked(false);minimum->setValue(-2);maximum->setValue(2);
    QCoreApplication::processEvents();
    const auto plot=chart->property("iqPlotRect").toRectF();QVERIFY(plot.width()>10);QCOMPARE(plot.width(),plot.height());
    const auto checkPointer=[&](QPoint pixel) {
      QTest::mouseMove(chart,pixel);QCoreApplication::processEvents();
      if(!chart->property("iqReadoutValid").toBool()) {
        qWarning()<<"IQ pointer event missing"<<pixel<<chart->property("iqPointerPixel")<<chart->property("iqPlotRect");return false;
      }
      const auto value=chart->property("iqReadout").toPointF();const auto low=chart->property("iqAxisMinimum").toDouble(),high=chart->property("iqAxisMaximum").toDouble();
      const auto currentPlot=chart->property("iqPlotRect").toRectF();
      // QWidget QTest mouse movement uses the physical cursor. At DPR 1.5 an
      // integer requested point can arrive at x+1/3. Validate that delivery,
      // then retain the exact numeric inverse-mapping check on the actual event.
      const auto delivered=chart->property("iqPointerPixel").toPointF();
      if(QLineF(delivered,QPointF(pixel)).length()>1.1)return false;
      const bool matched=std::abs(value.x()-(low+(delivered.x()-currentPlot.left())/currentPlot.width()*(high-low)))<1e-9 &&
             std::abs(value.y()-(high-(delivered.y()-currentPlot.top())/currentPlot.height()*(high-low)))<1e-9;
      if(!matched)qWarning()<<"IQ pointer mapping"<<pixel<<chart->property("iqPointerPixel")<<value<<currentPlot<<low<<high;
      return matched;
    };
    const auto pixel=QPointF(plot.left()+plot.width()*.25,plot.top()+plot.height()*.75).toPoint();QVERIFY(checkPointer(pixel));
    QWheelEvent wheel(QPointF(pixel),QPointF(chart->mapToGlobal(pixel)),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QCoreApplication::sendEvent(chart,&wheel);QVERIFY(chart->property("iqAxisMaximum").toDouble()<2);
    QVERIFY(checkPointer(pixel+QPoint(2,2)));QTest::mouseMove(chart,QPoint(1,1));QVERIFY(!chart->property("iqReadoutValid").toBool());
    QCOMPARE(workspace->analysisResult().get(),snapshot.get());QCOMPARE(workspace->renderStatistics()["analysisGeneration"].toString(),generation);
    workspace->cancelAnalysis(true);reference->setChecked(false);reference->setChecked(true);stage->setCurrentIndex(0);window.refresh();QTest::qWait(50);
    QVERIFY(!workspace->analysisResult());QCOMPARE(chart->property("referenceCellCount").toInt(),0);QVERIFY(!chart->property("decisionAvailable").toBool());
  }
  void pagePersistenceAndLegacyNames() {
    MainWindow window;populate(window);
    const auto id=window.session().createChannel("persist",window.session().activeFile()->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);
    QVERIFY(!id.empty());window.refresh();
    QTemporaryDir directory;const auto path=directory.filePath("page-v6.json");QString error;
    const std::array<std::pair<NarrowbandPage,const char*>,5> pages{{
      {NarrowbandPage::Observe,"observe"},{NarrowbandPage::Constellation,"constellation"},
      {NarrowbandPage::Eye,"eye"},{NarrowbandPage::Recognition,"recognition"},{NarrowbandPage::Demodulation,"demodulation"}}};
    for(const auto& [page,name]:pages) {
      auto* c=window.session().activeChannel();c->page=page;c->syncPanelExpanded=true;
      c->constellationDecisions=true;c->constellationReference=true;
      c->constellationMinimum=-2;c->constellationMaximum=3;c->eyePeriods=3;c->eyeComponent=2;c->eyeTraces=128;
      const auto analysis=c->analysis;
      QVERIFY2(ProjectStore::save(path,window.session().project(),error),qPrintable(error));
      Project loaded;QVERIFY2(ProjectStore::load(path,loaded,error),qPrintable(error));
      const auto& restored=loaded.files.front().channels.front();
      QCOMPARE(restored.page,page);QVERIFY(restored.syncPanelExpanded);QCOMPARE(restored.analysis,analysis);
      QVERIFY(restored.constellationDecisions && restored.constellationReference);
      QCOMPARE(restored.constellationMinimum,-2.);QCOMPARE(restored.constellationMaximum,3.);
      QCOMPARE(restored.eyePeriods,3);QCOMPARE(restored.eyeComponent,2);QCOMPARE(restored.eyeTraces,128);
      QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));
      const auto encoded=QJsonDocument::fromJson(file.readAll()).object();file.close();
      QCOMPARE(encoded["files"].toArray()[0].toObject()["channels"].toArray()[0].toObject()["page"].toString(),QString(name));
    }
    QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto baseline=QJsonDocument::fromJson(file.readAll()).object();file.close();
    const auto writeChannel=[&](QJsonObject channel){
      auto document=baseline;auto files=document["files"].toArray();auto f=files[0].toObject();
      auto channels=f["channels"].toArray();channels[0]=channel;f["channels"]=channels;files[0]=f;document["files"]=files;
      if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate))return false;
      const auto bytes=QJsonDocument(document).toJson();const bool ok=file.write(bytes)==bytes.size();file.close();return ok;
    };
    auto channel=baseline["files"].toArray()[0].toObject()["channels"].toArray()[0].toObject();
    for(const auto& [legacy,page]:std::array<std::pair<const char*,NarrowbandPage>,2>{{{"modulation",NarrowbandPage::Constellation},{"deepLearning",NarrowbandPage::Recognition}}}) {
      channel["page"]=legacy;channel.remove("narrowbandDisplay");QVERIFY(writeChannel(channel));
      Project loaded;QVERIFY2(ProjectStore::load(path,loaded,error),qPrintable(error));
      QCOMPARE(loaded.files.front().channels.front().page,page);QVERIFY(!loaded.files.front().channels.front().syncPanelExpanded);
      QVERIFY(!loaded.files.front().channels.front().constellationDecisions && !loaded.files.front().channels.front().constellationReference);
      QCOMPARE(loaded.files.front().channels.front().analysis,window.session().activeChannel()->analysis);
    }
    channel["page"]="unrecognized-page";QVERIFY(writeChannel(channel));
    Project sentinel;sentinel.name="unchanged";QVERIFY(!ProjectStore::load(path,sentinel,error));QCOMPARE(sentinel.name,std::string("unchanged"));
    channel["page"]="eye";channel["narrowbandDisplay"]=QJsonObject{{"version",2},{"syncPanelExpanded",true}};
    QVERIFY(writeChannel(channel));QVERIFY(!ProjectStore::load(path,sentinel,error));QCOMPARE(sentinel.name,std::string("unchanged"));
    channel["narrowbandDisplay"]=QJsonObject{{"version",1},{"syncPanelExpanded",true}};
    QVERIFY(writeChannel(channel));Project optional;QVERIFY(ProjectStore::load(path,optional,error));
    QVERIFY(!optional.files.front().channels.front().constellationDecisions && !optional.files.front().channels.front().constellationReference);
    for(const auto* key:{"constellationDecisions","constellationReference"}) {
      channel["narrowbandDisplay"]=QJsonObject{{"version",1},{"syncPanelExpanded",true},{key,"invalid"}};
      QVERIFY(writeChannel(channel));QVERIFY(!ProjectStore::load(path,sentinel,error));QCOMPARE(sentinel.name,std::string("unchanged"));
    }
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
    QTRY_COMPARE(chart->property("constellationPointCount").toULongLong(),qulonglong{256});
    QCOMPARE(workspace->analysisResult().get(),result.get());
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
    workspace->setSyncPanelExpanded(true);
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
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->eyePeriods==3 && chart->property("eyeTraceCount").toULongLong()==128 && chart->property("eyePointsPerUi").toInt()==8,15000);
    QCOMPARE(chart->property("eyeComponentCount").toInt(),1);QCOMPARE(chart->property("eyePointsPerTrace").toInt(),25);
    window.findChild<QComboBox*>("eyeAnalysisStage")->setCurrentIndex(static_cast<int>(AnalysisStage::MatchedFiltered));
    QTRY_VERIFY_WITH_TIMEOUT(workspace->analysisResult() && workspace->analysisResult()->filterDiscardedSamples>0 && workspace->analysisResult()->eyeStatus==AnalysisStatus::Ready,15000);
    auto* eyeStrip=dynamic_cast<ChartControlStrip*>(window.findChild<QWidget*>("narrowbandEyeControls"));
    auto* constellationStrip=dynamic_cast<ChartControlStrip*>(window.findChild<QWidget*>("narrowbandConstellationControls"));
    QVERIFY(eyeStrip && constellationStrip);
    for(auto* strip:{eyeStrip,constellationStrip}) {
      workspace->activatePage(strip==eyeStrip?NarrowbandPage::Eye:NarrowbandPage::Constellation);
      QCoreApplication::processEvents();
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
    workspace->activatePage(NarrowbandPage::Eye);
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
    workspace->activatePage(NarrowbandPage::Constellation);
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
    workspace->setSyncPanelExpanded(true);
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
      QCOMPARE(workspace->analysisResult().get(),result.get());
      QCOMPARE(window.findChild<QWidget*>("narrowbandConstellationLargeChart")->property("analysisStage").toInt(),static_cast<int>(stage));
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
      expanded.refresh();auto* work=expanded.findChild<NarrowbandWorkspace*>();work->activatePageForAcceptance(1);work->setSyncPanelExpanded(true);
      expanded.findChild<QComboBox*>("modulationFormat")->setCurrentIndex(static_cast<int>(family.first));
      auto* order=expanded.findChild<QComboBox*>("analysisModulationOrder");QVERIFY(order->isVisible());
      QCOMPARE(order->currentData().toInt(),family.first==ModulationHypothesis::Psk?8:family.first==ModulationHypothesis::Qam?32:16);
      if(family.first==ModulationHypothesis::Qam)QVERIFY(order->findData(4096)>=0);
      expanded.findChild<QPushButton*>("recoverNarrowbandSignal")->click();
      QTRY_VERIFY_WITH_TIMEOUT(work->analysisResult() && work->analysisResult()->carrierLocked,20000);
      QVERIFY(work->analysisResult()->constellationStage==AnalysisStage::CarrierRecovered);
      auto* chart=expanded.findChild<QWidget*>("narrowbandConstellationLargeChart");QVERIFY(!chart->property("analysisStageFallback").toBool());
      const auto familySnapshot=work->analysisResult();
      expanded.findChild<QComboBox*>("constellationAnalysisStage")->setCurrentIndex(5);
      expanded.findChild<QCheckBox*>("constellationReferenceRegions")->setChecked(true);
      QVERIFY(chart->property("decisionVisible").toBool());
      QTRY_COMPARE_WITH_TIMEOUT(chart->property("referenceCellCount").toInt(),expanded.session().activeChannel()->analysis.modulationOrder,5000);
      QCOMPARE(work->analysisResult().get(),familySnapshot.get());
      if(family.first==ModulationHypothesis::Apsk) {
        expanded.findChild<QToolButton*>("narrowbandSyncSettingsToggle")->setChecked(true);
        auto* rings=expanded.findChild<QLineEdit*>("analysisApskRings");QVERIFY(rings->isEnabled());rings->setFocus();
        QTest::keyClicks(rings,"4:1:45,12:2.85:15");expanded.refresh();
        QVERIFY(expanded.session().activeChannel()->analysis.apskRings.empty());QVERIFY(rings->text().contains("2.85"));
        QTest::keyClick(rings,Qt::Key_Return);QCOMPARE(expanded.session().activeChannel()->analysis.apskRings,std::string("4:1:45,12:2.85:15"));
        QTRY_VERIFY_WITH_TIMEOUT(work->analysisResult() && work->analysisResult()->carrierLocked,20000);
        QTRY_COMPARE_WITH_TIMEOUT(chart->property("referenceCellCount").toInt(),16,5000);
        QVERIFY(chart->property("decisionVisible").toBool());
      }
      QTemporaryDir saved;const auto expected=expanded.session().activeChannel()->analysis;
      QVERIFY(expanded.saveProject(saved.filePath("project.ssproj")));QVERIFY(expanded.openProject(saved.filePath("project.ssproj")));
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
  void projectWriteGuardFreezesNarrowbandChannelEdits() {
    MainWindow window;
    FileMetadata source;
    source.name="guard-tone.iq";
    source.sampleRateHz=20e6;
    source.centerFrequencyHz=100e6;
    source.sampleCount=200000;
    source.demo=true;
    QVERIFY(!window.session().addDemoFile(source).empty());
    window.session().addMark({{1000,200000},{101.5e6,102.5e6}});
    window.resize(1280,800);
    window.show();
    const auto id=window.session().createChannel("guard",window.session().activeFile()->activeMarkId,102e6,1e6,2e6,{1000,200000},ChannelFilter::Standard,false,true);
    QVERIFY(!id.empty());
    window.refresh();
    QTest::qWait(120);
    auto* workspace=window.findChild<NarrowbandWorkspace*>();
    QVERIFY(workspace);
    auto* waveform=window.findChild<QComboBox*>("narrowbandWaveformMode");
    auto* psd=window.findChild<QComboBox*>("narrowbandPsdFft");
    auto* stft=window.findChild<QComboBox*>("narrowbandStftFft");
    auto* frequency=window.findChild<QComboBox*>("channelFrequencyMode");
    auto* sync=window.findChild<QToolButton*>("narrowbandSyncSettingsToggle");
    auto* constellationPage=window.findChild<QPushButton*>("modulationPage");
    auto* eyePage=window.findChild<QPushButton*>("eyePage");
    auto* eyeComponent=window.findChild<QComboBox*>("eyeComponent");
    auto* eyePeriods=window.findChild<QComboBox*>("eyePeriods");
    auto* stage=window.findChild<QComboBox*>("constellationAnalysisStage");
    auto* removeDc=window.findChild<QCheckBox*>("analysisRemoveDc");
    auto* modulation=window.findChild<QComboBox*>("modulationFormat");
    auto* symbolRate=window.findChild<QDoubleSpinBox*>("symbolRate");
    auto* recover=window.findChild<QPushButton*>("recoverNarrowbandSignal");
    auto* status=window.findChild<QLabel*>("modulationAnalysisStatus");
    QVERIFY(waveform && psd && stft && frequency && sync && constellationPage && eyePage);
    QVERIFY(eyeComponent && eyePeriods && stage && removeDc && modulation && symbolRate && recover && status);
    const auto* channel=window.session().activeChannel();
    QVERIFY(channel);
    const auto token=ProjectStore::stateToken(window.session().project());
    const auto epoch=window.session().contentEpoch();
    const auto generation=window.session().projectGeneration();
    const auto analysisGeneration=workspace->renderStatistics().value("analysisGeneration").toString();
    const auto analysisResult=workspace->analysisResult();
    const auto statusText=status->text();
    const auto page=channel->page;
    const auto syncExpanded=channel->syncPanelExpanded;
    const auto waveformMode=channel->waveform;
    const auto autoScale=channel->waveformAutoScale;
    const auto psdSize=channel->psdFftSize;
    const auto stftSize=channel->stftFftSize;
    const auto absolute=channel->absoluteFrequencyLabels;
    const auto hypothesis=channel->analysis.hypothesis;
    const auto rateMode=channel->analysis.symbolRateMode;
    const auto rate=channel->symbolRate;
    const auto rateHz=channel->analysis.symbolRateHz;
    const auto component=channel->eyeComponent;
    const auto periods=channel->eyePeriods;
    const auto dc=channel->analysis.removeDc;
    const auto matched=channel->analysis.matchedFilter;
    const auto timing=channel->analysis.timingRecovery;
    const auto carrier=channel->analysis.carrierRecovery;
    const auto cfo=channel->analysis.cfoMode;
    const auto decisions=channel->constellationDecisions;
    const auto constellationStage=channel->analysis.constellationStage;
    int guardCalls=0;
    window.session().setProjectWriteGuard([&](std::string& error) {
      ++guardCalls;
      error="publication commit";
      return false;
    });
    const auto driveRejected=[&] {
      waveform->setCurrentIndex(2);
      psd->setCurrentIndex(9);
      stft->setCurrentIndex(4);
      frequency->setCurrentIndex(1);
      constellationPage->click();
      workspace->activatePage(NarrowbandPage::Eye);
      sync->setChecked(true);
      workspace->setSyncPanelExpanded(true);
      eyeComponent->setCurrentIndex(2);
      eyePeriods->setCurrentIndex(3);
      stage->setCurrentIndex(2);
      removeDc->setChecked(true);
      modulation->setCurrentIndex(static_cast<int>(ModulationHypothesis::Qpsk));
      symbolRate->setValue(625000);
      recover->click();
    };
    driveRejected();
    QCoreApplication::processEvents();
    channel=window.session().activeChannel();
    QVERIFY(channel);
    QVERIFY(guardCalls>0);
    QCOMPARE(ProjectStore::stateToken(window.session().project()),token);
    QCOMPARE(window.session().contentEpoch(),epoch);
    QCOMPARE(window.session().projectGeneration(),generation);
    QCOMPARE(workspace->renderStatistics().value("analysisGeneration").toString(),analysisGeneration);
    QCOMPARE(workspace->analysisResult().get(),analysisResult.get());
    QVERIFY(status->text().contains(QStringLiteral("无法同步")));
    QCOMPARE(channel->page,page);
    QCOMPARE(channel->syncPanelExpanded,syncExpanded);
    QCOMPARE(channel->waveform,waveformMode);
    QCOMPARE(channel->waveformAutoScale,autoScale);
    QCOMPARE(channel->psdFftSize,psdSize);
    QCOMPARE(channel->stftFftSize,stftSize);
    QCOMPARE(channel->absoluteFrequencyLabels,absolute);
    QCOMPARE(channel->analysis.hypothesis,hypothesis);
    QCOMPARE(channel->analysis.symbolRateMode,rateMode);
    QCOMPARE(channel->symbolRate,rate);
    QCOMPARE(channel->analysis.symbolRateHz,rateHz);
    QCOMPARE(channel->eyeComponent,component);
    QCOMPARE(channel->eyePeriods,periods);
    QCOMPARE(channel->analysis.removeDc,dc);
    QCOMPARE(channel->analysis.matchedFilter,matched);
    QCOMPARE(channel->analysis.timingRecovery,timing);
    QCOMPARE(channel->analysis.carrierRecovery,carrier);
    QCOMPARE(channel->analysis.cfoMode,cfo);
    QCOMPARE(channel->constellationDecisions,decisions);
    QCOMPARE(channel->analysis.constellationStage,constellationStage);
    QCOMPARE(waveform->currentIndex(),static_cast<int>(waveformMode));
    QCOMPARE(psd->currentData().toInt(),psdSize);
    QCOMPARE(stft->currentData().toInt(),stftSize);
    QCOMPARE(frequency->currentIndex(),absolute?1:0);
    QCOMPARE(workspace->currentPageIndex(),static_cast<int>(page));
    QVERIFY(window.findChild<QPushButton*>("observePage")->isChecked());
    QVERIFY(!constellationPage->isChecked());
    QVERIFY(!eyePage->isChecked());
    QCOMPARE(sync->isChecked(),syncExpanded);
    QCOMPARE(eyeComponent->currentIndex(),component);
    QCOMPARE(eyePeriods->currentData().toInt(),periods);
    QCOMPARE(stage->currentIndex(),decisions?5:static_cast<int>(constellationStage));
    QCOMPARE(removeDc->isChecked(),dc);
    QCOMPARE(modulation->currentIndex(),static_cast<int>(hypothesis));
    QCOMPARE(symbolRate->value(),rateHz>0?rateHz:rate);
    window.session().setProjectWriteGuard({});
    const auto epochBeforeAccept=window.session().contentEpoch();
    const auto analysisBeforeAccept=workspace->renderStatistics().value("analysisGeneration").toString();
    const auto resultBeforeAccept=workspace->analysisResult();
    waveform->setCurrentIndex(2);
    psd->setCurrentIndex(9);
    stft->setCurrentIndex(4);
    frequency->setCurrentIndex(1);
    constellationPage->click();
    sync->setChecked(true);
    eyeComponent->setCurrentIndex(2);
    stage->setCurrentIndex(2);
    QCoreApplication::processEvents();
    channel=window.session().activeChannel();
    QVERIFY(channel);
    QCOMPARE(channel->waveform,NarrowbandWaveform::Phase);
    QCOMPARE(channel->waveformAutoScale,true);
    QCOMPARE(channel->psdFftSize,psd->currentData().toInt());
    QCOMPARE(channel->stftFftSize,stft->currentData().toInt());
    QVERIFY(channel->psdFftSize!=psdSize);
    QVERIFY(channel->stftFftSize!=stftSize);
    QVERIFY(channel->absoluteFrequencyLabels);
    QCOMPARE(channel->page,NarrowbandPage::Constellation);
    QVERIFY(channel->syncPanelExpanded);
    QCOMPARE(channel->eyeComponent,2);
    QCOMPARE(channel->analysis.constellationStage,AnalysisStage::MatchedFiltered);
    QCOMPARE(workspace->currentPageIndex(),static_cast<int>(NarrowbandPage::Constellation));
    QCOMPARE(sync->isChecked(),true);
    QCOMPARE(frequency->currentIndex(),1);
    QCOMPARE(workspace->renderStatistics().value("analysisGeneration").toString(),analysisBeforeAccept);
    QCOMPARE(workspace->analysisResult().get(),resultBeforeAccept.get());
    QVERIFY(status->text().contains(QStringLiteral("来源没有可用的真实通道 IQ")));
    QVERIFY(window.session().contentEpoch()>epochBeforeAccept);
    QCOMPARE(window.session().projectGeneration(),generation);
    const auto committedEpoch=window.session().contentEpoch();
    const auto committedToken=ProjectStore::stateToken(window.session().project());
    const auto committedAnalysis=workspace->renderStatistics().value("analysisGeneration").toString();
    modulation->setCurrentIndex(static_cast<int>(ModulationHypothesis::Qpsk));
    QCoreApplication::processEvents();
    channel=window.session().activeChannel();
    QCOMPARE(channel->analysis.hypothesis,ModulationHypothesis::Qpsk);
    QVERIFY(status->text().contains(QStringLiteral("来源没有可用的真实通道 IQ")));
    QCOMPARE(workspace->renderStatistics().value("analysisGeneration").toString(),committedAnalysis);
    QCOMPARE(workspace->analysisResult().get(),resultBeforeAccept.get());
    const auto qpskToken=ProjectStore::stateToken(window.session().project());
    const auto qpskEpoch=window.session().contentEpoch();
    const auto qpskStatus=status->text();
    QVERIFY(qpskToken!=committedToken);
    QVERIFY(qpskEpoch>committedEpoch);
    guardCalls=0;
    window.session().setProjectWriteGuard([&](std::string& error) {
      ++guardCalls;
      error="publication commit";
      return false;
    });
    recover->click();
    psd->setCurrentIndex(8);
    eyePage->click();
    sync->setChecked(false);
    eyePeriods->setCurrentIndex(0);
    symbolRate->setValue(500000);
    removeDc->setChecked(true);
    QCoreApplication::processEvents();
    channel=window.session().activeChannel();
    QVERIFY(guardCalls>0);
    QCOMPARE(ProjectStore::stateToken(window.session().project()),qpskToken);
    QCOMPARE(window.session().contentEpoch(),qpskEpoch);
    QCOMPARE(window.session().projectGeneration(),generation);
    QCOMPARE(workspace->renderStatistics().value("analysisGeneration").toString(),committedAnalysis);
    QCOMPARE(workspace->analysisResult().get(),resultBeforeAccept.get());
    QCOMPARE(status->text(),qpskStatus);
    QCOMPARE(channel->analysis.hypothesis,ModulationHypothesis::Qpsk);
    QVERIFY(!channel->analysis.matchedFilter);
    QVERIFY(!channel->analysis.timingRecovery);
    QVERIFY(!channel->analysis.carrierRecovery);
    QCOMPARE(channel->analysis.cfoMode,CfoMode::Off);
    QCOMPARE(channel->psdFftSize,psd->itemData(9).toInt());
    QCOMPARE(channel->page,NarrowbandPage::Constellation);
    QVERIFY(channel->syncPanelExpanded);
    QCOMPARE(channel->eyePeriods,periods);
    QCOMPARE(channel->symbolRate,rate);
    QCOMPARE(channel->analysis.symbolRateMode,rateMode);
    QVERIFY(!channel->analysis.removeDc);
    QCOMPARE(psd->currentData().toInt(),channel->psdFftSize);
    QCOMPARE(workspace->currentPageIndex(),static_cast<int>(NarrowbandPage::Constellation));
    QCOMPARE(sync->isChecked(),true);
    QCOMPARE(eyePeriods->currentData().toInt(),periods);
    QCOMPARE(symbolRate->value(),rateHz>0?rateHz:rate);
    QCOMPARE(removeDc->isChecked(),false);
    window.session().setProjectWriteGuard({});
    const auto epochBeforeRecover=window.session().contentEpoch();
    recover->click();
    eyePeriods->setCurrentIndex(3);
    removeDc->setChecked(true);
    symbolRate->setValue(625000);
    eyePage->click();
    QCoreApplication::processEvents();
    channel=window.session().activeChannel();
    QVERIFY(channel->analysis.matchedFilter);
    QVERIFY(channel->analysis.timingRecovery);
    QVERIFY(channel->analysis.carrierRecovery);
    QCOMPARE(channel->analysis.cfoMode,CfoMode::Automatic);
    QCOMPARE(channel->analysis.constellationStage,AnalysisStage::CarrierRecovered);
    QCOMPARE(channel->analysis.eyeStage,AnalysisStage::CarrierRecovered);
    QCOMPARE(channel->eyePeriods,4);
    QVERIFY(channel->analysis.removeDc);
    QCOMPARE(channel->symbolRate,625000.0);
    QCOMPARE(channel->analysis.symbolRateHz,625000.0);
    QCOMPARE(channel->analysis.symbolRateMode,SymbolRateMode::Manual);
    QCOMPARE(channel->page,NarrowbandPage::Eye);
    QCOMPARE(workspace->currentPageIndex(),static_cast<int>(NarrowbandPage::Eye));
    QCOMPARE(eyePage->isChecked(),true);
    QCOMPARE(removeDc->isChecked(),true);
    QCOMPARE(symbolRate->value(),625000.0);
    QVERIFY(window.session().contentEpoch()>epochBeforeRecover);
    QCOMPARE(window.session().projectGeneration(),generation);
    QVERIFY(ProjectStore::stateToken(window.session().project())!=qpskToken);
    QCOMPARE(channel->analysis.hypothesis,ModulationHypothesis::Qpsk);
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
    const auto saved=*window.session().activeChannel();const auto projectPath=directory.filePath("real-narrowband.ssproj");
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
