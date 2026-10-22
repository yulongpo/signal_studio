#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QtTest>
#include "app/main_window.h"
#include "ui/narrowband_workspace.h"

using namespace signalstudio;
class NarrowbandAnalysisUiTests : public QObject {
  Q_OBJECT
  void populate(MainWindow& window) {
    FileMetadata source;source.name="tone.raw";
    source.path=(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")+"/tone.raw").toStdString();
    source.sampleRateHz=20e6;source.centerFrequencyHz=100e6;source.sampleCount=400000;source.demo=false;
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
};
QTEST_MAIN(NarrowbandAnalysisUiTests)
#include "narrowband_analysis_ui_tests.moc"
