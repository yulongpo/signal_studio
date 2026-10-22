#pragma once
#include <QMenu>
#include <QSplitter>
#include "ui/narrowband_workspace.h"
inline void UxTests::roundTwoFlatResponsiveControls() {
  QWidget root;root.resize(1000,450);
  ParameterInputPolicy policy(&root);SettingsPopoverHost host(&root);
  ChartControlStrip strip(&host,"roundTwoStrip",&root);strip.setGeometry(10,10,850,28);
  auto* points=new QComboBox;points->setObjectName("roundTwoPoints");points->addItems({"1024","2048","4096"});points->setFixedWidth(90);
  auto* method=new QComboBox;method->setObjectName("roundTwoMethod");method->addItems({"Welch","Bartlett"});method->setFixedWidth(110);
  strip.addControl(points,"N",100);strip.addControl(method,"方法",10);strip.addPanel(new QLabel("高级参数"));
  root.show();QTest::qWait(40);
  QVERIFY(points->isVisible()&&method->isVisible());
  QVERIFY(points->mapTo(&strip,QPoint()).x()>400);
  QVERIFY(strip.rect().contains(QRect(strip.overflowButton()->mapTo(&strip,QPoint()),strip.overflowButton()->size())));
  QCOMPARE(root.findChildren<QMenu*>().size(),0);
  // This scenario tests idle reflow. An editor retaining focus deliberately
  // postpones rehosting to protect its input transaction.
  root.setFocusPolicy(Qt::StrongFocus);root.setFocus();QVERIFY(root.hasFocus());
  const int crampedWidth=std::max(72,strip.overflowButton()->sizeHint().width())+10;
  strip.resize(crampedWidth,28);QTest::qWait(40);
  QTest::mouseClick(strip.overflowButton(),Qt::LeftButton);
  QVERIFY(host.activePopover());QVERIFY(points->isVisible()&&method->isVisible());
  QVERIFY(strip.popoverFrame()->isAncestorOf(points));
  QCOMPARE(root.findChild<QComboBox*>("roundTwoPoints"),points);
  method->setFocus();QTest::keyClick(method,Qt::Key_End);QCOMPARE(method->currentText(),QString("Bartlett"));
  QTest::keyClick(method,Qt::Key_Escape);QTRY_VERIFY(!host.activePopover());
  strip.resize(850,28);QTest::qWait(40);
  QCOMPARE(points->parentWidget(),&strip);QCOMPARE(method->parentWidget(),&strip);
  QCOMPARE(method->currentText(),QString("Bartlett"));
  for(auto* widget:{static_cast<QWidget*>(points),static_cast<QWidget*>(method),static_cast<QWidget*>(strip.overflowButton())}){
    const QRect rect(widget->mapTo(&strip,QPoint()),widget->size());QVERIFY(strip.rect().contains(rect));
  }
}

inline void UxTests::roundTwoNarrowbandPsdAndPages() {
  MainWindow window;window.openNarrowbandDemoProject();expose(window);
  auto* workspace=window.findChild<NarrowbandWorkspace*>();QVERIFY(workspace);
  QVERIFY(!window.findChild<QWidget*>("narrowbandChartToolbar"));
  QVERIFY(!window.findChild<QWidget*>("narrowbandConstellationPreview"));
  auto* splitter=window.findChild<QSplitter*>("narrowbandObserveSplitter");QVERIFY(splitter&&splitter->count()==2);
  QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(),20000);
  auto* points=window.findChild<QComboBox*>("narrowbandPsdFft");
  auto* source=window.findChild<QComboBox*>("psdSource");
  auto* stft=window.findChild<QComboBox*>("narrowbandStftFft");QVERIFY(points&&source&&stft);
  QVERIFY(points->isVisible()&&source->isVisible());
  const int stftN=window.session().activeChannel()->stftFftSize;
  points->setFocus();QTest::keyClick(points,Qt::Key_Home);
  for(int i=0;i<16&&points->currentData().toInt()!=8192;++i)QTest::keyClick(points,Qt::Key_Down);
  QCOMPARE(window.session().activeChannel()->psdFftSize,8192);
  QCOMPARE(window.session().activeChannel()->stftFftSize,stftN);
  QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(),20000);
  auto* heat=window.findChild<QWidget*>("narrowbandStftPanelChart");
  const QRect plot(66,14,heat->width()-82,heat->height()-55);
  QTest::mouseClick(heat,Qt::LeftButton,Qt::NoModifier,plot.center());
  QTRY_COMPARE(source->currentIndex(),1);
  QVERIFY(!points->isEnabled());QCOMPARE(points->currentData().toInt(),stftN);
  QVERIFY(points->currentText().contains("STFT"));
  source->setFocus();QTest::keyClick(source,Qt::Key_Home);
  QTRY_VERIFY(points->isEnabled());QCOMPARE(points->currentData().toInt(),8192);
  QCOMPARE(window.session().activeChannel()->psdFftSize,8192);
  QVERIFY(!points->itemText(points->findData(stftN)).contains("STFT"));
  points->setFocus();QTest::keyClick(points,Qt::Key_Home);
  for(int i=0;i<16&&points->currentData().toInt()!=stftN;++i)QTest::keyClick(points,Qt::Key_Down);
  QCOMPARE(points->currentData().toInt(),stftN);
  QTest::mouseClick(heat,Qt::LeftButton,Qt::NoModifier,plot.center());
  QTRY_COMPARE(source->currentIndex(),1);
  source->setFocus();QTest::keyClick(source,Qt::Key_Home);
  QTRY_VERIFY(points->isEnabled());QCOMPARE(points->currentData().toInt(),stftN);
  QVERIFY(!points->currentText().contains("STFT"));
  window.session().activeChannel()->psdFftSize=16;window.refresh();
  QCOMPARE(points->currentData().toInt(),16);QVERIFY(!points->currentText().isEmpty());
  QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(),20000);
  for(const auto* page:{"modulationPage","deepLearningPage","demodulationPage","observePage"}){
    auto* button=window.findChild<QAbstractButton*>(page);QVERIFY(button&&button->isVisible());
    QTest::mouseClick(button,Qt::LeftButton);window.refresh();QVERIFY(button->isChecked());
    if(QString(page)=="deepLearningPage"){
      auto* state=window.findChild<QLabel*>("recognitionStatus");QVERIFY(state&&!state->text().contains("synthetic="));
      for(auto* card:window.findChildren<QLabel*>("recognitionStatCard"))QVERIFY(!card->text().contains("0.82"));
      const auto recognition=state->text();window.refresh();QCOMPARE(state->text(),recognition);
    }
  }
  QTemporaryDir directory;const auto project=directory.filePath("project.json");
  QVERIFY(window.saveProject(project));QVERIFY(window.openProject(project));
  QTRY_VERIFY_WITH_TIMEOUT(workspace->visibleChartsSettled(),20000);
  QCOMPARE(window.session().activeChannel()->psdFftSize,16);
  QCOMPARE(points->currentData().toInt(),16);QCOMPARE(window.session().activeChannel()->stftFftSize,stftN);
}
