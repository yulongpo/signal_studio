#pragma once
#include <QMenu>
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
  strip.resize(165,28);QTest::qWait(40);
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
