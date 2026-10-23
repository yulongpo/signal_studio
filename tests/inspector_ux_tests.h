#pragma once
#include "ui/narrowband_workspace.h"

inline void UxTests::uxR02_inspectorRouting() {
  MainWindow window;
  QTemporaryDir directory;
  QString issue;
  QVERIFY(window.createProject(directory.path(), "inspector-routing", &issue));
  issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
  QVERIFY2(issue.isEmpty(), qPrintable(issue));
  const auto first = window.session().project().activeFileId;
  const auto view = window.session().activeFile()->view;
  const auto span = view.time.end - view.time.begin;
  const ViewRange firstRange{{view.time.begin + span / 5, view.time.begin + span * 2 / 5},
                            {99.7e6, 99.9e6}};
  const ViewRange secondRange{{view.time.begin + span * 3 / 5, view.time.begin + span * 4 / 5},
                             {100.1e6, 100.3e6}};
  const auto mark1 = window.session().addMark(firstRange);
  const auto mark2 = window.session().addMark(secondRange);
  window.session().selectMarks({});
  issue = addSource(window, directory.path(), "IQ_B_FS1Msps_FC200MHz.dat");
  QVERIFY2(issue.isEmpty(), qPrintable(issue));
  const auto second = window.session().project().activeFileId;
  window.session().activateFile(first);
  expose(window);
  const auto page = [&](const char* id) { return window.findChild<QAbstractButton*>(id)->isChecked(); };
  QVERIFY(page("contextDisplay"));
  QVERIFY(!window.findChild<QAbstractButton*>("contextChannel")->isEnabled());
  QVERIFY(!window.findChild<QPushButton*>("runWidebandDetection")->isVisible());
  auto* plot = window.findChild<PlotWidget*>("mainPlot");
  const auto select = [&](const ViewRange& range) {
    QTest::mouseClick(plot, Qt::LeftButton, Qt::NoModifier,
        plot->toPixel((range.time.begin + range.time.end) / 2,
                      (range.frequency.lowerHz + range.frequency.upperHz) / 2).toPoint());
  };
  select(firstRange);
  QCOMPARE(window.session().activeFile()->activeMarkId, mark1);
  QVERIFY(page("contextAnnotation"));
  QTest::mouseClick(window.findChild<QAbstractButton*>("contextDisplay"), Qt::LeftButton);
  select(secondRange);
  QCOMPARE(window.session().activeFile()->activeMarkId, mark2);
  QVERIFY(page("contextDisplay"));
  for (int i = 0; i < 3; ++i) window.refresh();
  QVERIFY(page("contextDisplay"));
  QTest::mouseClick(window.findChild<QAbstractButton*>("widebandDetect"), Qt::LeftButton);
  QVERIFY(page("contextDetection"));
  QVERIFY(window.findChild<QPushButton*>("runWidebandDetection")->isVisible());
  select(firstRange);
  QVERIFY(page("contextDetection"));
  window.session().activateFile(second); window.refresh();
  window.session().activateFile(first); window.refresh();
  select(secondRange);
  QVERIFY(page("contextAnnotation"));
  for (const auto* legacy : {"fileSection", "viewSection", "markSection", "psdSection",
                              "specSection", "detectionSection", "narrowbandChannelSection"})
    QVERIFY2(!window.findChild<QWidget*>(legacy), legacy);
}

inline void UxTests::uxR02_fileProperties() {
  MainWindow window;
  const auto first = window.session().addDemoFile();
  const auto second = window.session().addDemoFile();
  const auto otherBandwidth = window.session().activeFile()->metadata.effectiveBandwidthHz;
  window.session().activateFile(first);
  expose(window);
  auto* entry = window.findChild<QAbstractButton*>("fileProperties");
  QVERIFY(entry && entry->isVisible());
  QTest::mouseClick(entry, Qt::LeftButton);
  auto* frame = window.findChild<QFrame*>("filePropertiesPopover");
  auto* bandwidth = window.findChild<QDoubleSpinBox*>("effectiveBandwidthMHz");
  QVERIFY(frame && frame->isVisible() && bandwidth && bandwidth->isVisible());
  auto* owner = bandwidth->parentWidget();
  auto* editor = bandwidth->findChild<QLineEdit*>();
  QTest::mouseClick(editor, Qt::LeftButton);
  editor->selectAll(); QTest::keyClicks(editor, "10");
  QTest::keyClick(editor, Qt::Key_Return);
  QCOMPARE(window.session().activeFile()->metadata.effectiveBandwidthHz, 10e6);
  window.refresh();
  QVERIFY(frame->isVisible());
  QCOMPARE(bandwidth->parentWidget(), owner);
  editor->selectAll(); QTest::keyClicks(editor, "12");
  window.session().activateFile(second); window.refresh();
  QVERIFY(!frame->isVisible());
  QVERIFY(!bandwidth->property("parameterDraft").toBool());
  QCOMPARE(window.session().activeFile()->metadata.effectiveBandwidthHz, otherBandwidth);
  window.session().activateFile(first); window.refresh();
  QCOMPARE(window.session().activeFile()->metadata.effectiveBandwidthHz, 10e6);
}

inline void UxTests::uxR02_paletteWorkspaceOwnership() {
  MainWindow window;
  window.openNarrowbandDemoProject();
  expose(window);
  QVERIFY(window.findChild<QAbstractButton*>("contextChannel")->isChecked());
  QTest::mouseClick(window.findChild<QAbstractButton*>("contextDisplay"), Qt::LeftButton);
  auto* palette = window.findChild<QComboBox*>("colormap");
  QVERIFY(palette && palette->isVisible());
  auto* owner = palette->parentWidget();
  QCOMPARE(owner->objectName(), QString("narrowbandStftPanelControls"));
  palette->setFocus();
  QTest::keyClick(palette, Qt::Key_Home);
  QCOMPARE(window.session().activeFile()->display.palette, Palette::Turbo);
  for (int i = 0; i < 3; ++i) window.refresh();
  QCOMPARE(palette->parentWidget(), owner);
  QVERIFY(palette->hasFocus());
  auto* mode = window.findChild<QAbstractButton*>("workspaceMode");
  QTest::mouseClick(mode, Qt::LeftButton);
  QVERIFY(!window.session().project().narrowbandWorkspaceOpen);
  QVERIFY(palette->isVisible());
  QCOMPARE(palette->parentWidget()->objectName(), QString("specChartControls"));
  QTest::mouseClick(mode, Qt::LeftButton);
  QVERIFY(window.session().project().narrowbandWorkspaceOpen);
  QTest::mouseClick(window.findChild<QAbstractButton*>("contextDisplay"), Qt::LeftButton);
  QVERIFY(palette->isVisible());
  QCOMPARE(palette->parentWidget(), owner);
  QCOMPARE(palette->currentIndex(), static_cast<int>(Palette::Turbo));
  QCOMPARE(window.findChildren<QComboBox*>("colormap").size(), 1);
}
