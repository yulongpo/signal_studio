#pragma once

#include <QLayout>
#include <QLineEdit>
#include "ui/chart_control_strip.h"

inline void UxTests::uxR01_chartToolbarLayout() {
  MainWindow window;
  window.session().addDemoFile();
  expose(window);
  for (const QSize size : {QSize(2560, 1440), QSize(1920, 1080)}) {
    window.resize(size);
    QTest::qWait(30);
    for (const auto* mode : {"waveMode", "psdMode"}) {
      auto* button = window.findChild<QAbstractButton*>(mode);
      QVERIFY(button && button->isVisible());
      QTest::mouseClick(button, Qt::LeftButton);
      QTest::qWait(30);
      for (const auto* name : {"auxChartHead", "specChartHead"}) {
        auto* header = window.findChild<QWidget*>(name);
        QVERIFY(header && header->isVisible());
        QVERIFY(header->height() <= 35);
        QList<QWidget*> controls;
        const auto collect = [&](auto&& self, QWidget* owner) -> void {
          if (!owner->layout()) { controls.append(owner); return; }
          for (int i = 0; i < owner->layout()->count(); ++i)
            if (auto* widget = owner->layout()->itemAt(i)->widget();
                widget && widget->isVisible()) self(self, widget);
        };
        collect(collect, header);
        for (int i = 0; i < controls.size(); ++i) {
          auto* control = controls[i];
          const auto rect = QRect(control->mapTo(header, QPoint()), control->size());
          QVERIFY2(header->rect().contains(rect), qPrintable(control->objectName()));
          for (int j = 0; j < i; ++j)
            QVERIFY2(!rect.intersects(QRect(controls[j]->mapTo(header, QPoint()), controls[j]->size())),
                     qPrintable(control->objectName() + " / " + controls[j]->objectName()));
          if (qobject_cast<QAbstractButton*>(control) || qobject_cast<QComboBox*>(control)) {
            auto* hit = window.childAt(control->mapTo(&window, control->rect().center()));
            QVERIFY2(hit && (hit == control || control->isAncestorOf(hit)),
                     qPrintable(control->objectName()));
          }
        }
      }
      QCOMPARE(window.findChild<QComboBox*>("waveformMode")->isVisible(), QString(mode) == "waveMode");
      if (QString(mode) == "psdMode") QVERIFY(window.findChild<QComboBox*>("psdFft")->isVisible());
    }
  }
  QCOMPARE(window.findChildren<QComboBox*>("colormap").size(), 1);
  QVERIFY(!window.findChild<QComboBox*>("palette"));

  // Force the reusable strip's overflow without changing the application
  // acceptance size or moving/reparenting any editor.
  QWidget root;
  root.resize(500, 300);
  SettingsPopoverHost host(&root);
  ChartControlStrip strip(&host, "testStrip", &root);
  strip.setGeometry(10, 10, 90, 28);
  QToolButton first, second;
  first.setObjectName("first"); first.setText("First settings");
  second.setObjectName("second"); second.setText("Other settings");
  int invoked = 0;
  QObject::connect(&first,&QToolButton::clicked,[&]{++invoked;});
  strip.addControl(&first, "First", 10);
  strip.addControl(&second, "Second", 1);
  root.show();
  QTest::qWait(30);
  QVERIFY(strip.overflowButton()->isVisible());
  QTest::mouseClick(strip.overflowButton(), Qt::LeftButton);
  QVERIFY(first.isVisible());
  QVERIFY(first.parentWidget()!=&strip);
  QVERIFY(!first.menu());
  QTest::mouseClick(&first,Qt::LeftButton);
  QCOMPARE(invoked,1);
  QVERIFY(host.activePopover());
  QTest::keyClick(&first,Qt::Key_Escape);
  QVERIFY(!host.activePopover());
  // Stack-owned controls must leave the strip before its destructor.
  first.setParent(nullptr); second.setParent(nullptr);
}

inline void UxTests::uxR02_chartSettingsStayStable() {
  MainWindow window;
  const auto first = window.session().addDemoFile();
  window.session().addDemoFile();
  window.session().activateFile(first);
  expose(window);
  window.session().setAuxiliaryMode(AuxiliaryMode::Psd);window.refresh();
  auto* power = chartSettingsEntry(window,"psdSettings");
  QVERIFY(power && power->isVisible());
  QTest::mouseClick(power, Qt::LeftButton);
  auto* frame = chartSettingsFrame(window,"psdSettings");
  QVERIFY(frame && frame->isVisible());
  auto* reference = window.findChild<QComboBox*>("psdSegmentMs");
  QVERIFY(reference && reference->isVisible() && reference->isEditable());
  const auto accepted = window.session().activeFile()->display.psd.parameters.segmentMilliseconds;
  auto* parent = reference->parentWidget();
  reference->lineEdit()->setFocus();
  reference->lineEdit()->selectAll();
  QTest::keyClicks(reference->lineEdit(), "3.3");
  QVERIFY(reference->property("parameterDraft").toBool());
  for (int i = 0; i < 5; ++i) window.refresh();
  QCOMPARE(reference->parentWidget(), parent);
  QCOMPARE(reference->currentText(), QString("3.3"));
  QCOMPARE(window.session().activeFile()->display.psd.parameters.segmentMilliseconds, accepted);
  QVERIFY(reference->lineEdit()->hasFocus());
  QTest::keyClick(reference->lineEdit(), Qt::Key_Escape);
  QVERIFY(!frame->isVisible());
  QVERIFY(!reference->property("parameterDraft").toBool());
  QTest::mouseClick(power, Qt::LeftButton);
  QTest::keyClicks(reference->lineEdit(), "9");
  window.session().activateFile(window.session().project().files.back().metadata.id);
  window.refresh();
  QVERIFY(!frame->isVisible());
  QVERIFY(!reference->property("parameterDraft").toBool());
  QTest::mouseClick(window.findChild<QAbstractButton*>("psdMode"),Qt::LeftButton);QCoreApplication::processEvents();
  QVERIFY(!window.findChild<QWidget*>("chartSettingsLinks"));
  for(const auto* category:{"psdSettings","stftSettings"}){
    auto* entry=chartSettingsEntry(window,category);auto* layer=chartSettingsFrame(window,category);
    QVERIFY(entry&&entry->isVisible()&&layer);
    QTest::mouseClick(entry,Qt::LeftButton);QTRY_VERIFY(layer->isVisible());
    QTest::mouseClick(entry,Qt::LeftButton);QTRY_VERIFY(!layer->isVisible());
  }
}
