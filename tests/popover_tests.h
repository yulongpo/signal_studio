#pragma once

// Include after UxTests has declared these three slots. No test entry point or
// moc unit belongs here; ux_tests.cpp owns both.
#include <QtTest>
#include <QAbstractItemView>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include "ui/parameter_input.h"
#include "ui/settings_popover.h"

inline void UxTests::uxP01_popupDismissal() {
  using namespace signalstudio;
  QWidget root;
  root.resize(640, 480);
  QPushButton first("First", &root), second("Second", &root), outside("Outside", &root);
  first.setGeometry(20, 20, 90, 30);
  second.setGeometry(130, 20, 90, 30);
  outside.setGeometry(500, 420, 100, 30);
  ParameterInputPolicy policy(&root);
  SettingsPopoverHost host(&root);
  auto* firstFrame = host.add(&first, new QLabel("First settings"), "p01First");
  auto* secondFrame = host.add(&second, new QLabel("Second settings"), "p01Second");
  QSignalSpy outsideClicks(&outside, &QPushButton::clicked);
  root.show();
  QVERIFY(QTest::qWaitForWindowExposed(&root));
  QTest::mouseClick(&first, Qt::LeftButton);
  QCOMPARE(host.activePopover(), firstFrame);
  QCOMPARE(firstFrame->parentWidget(), &root);
  QTest::mouseClick(&first, Qt::LeftButton);
  QVERIFY(!host.activePopover());
  QTest::mouseClick(&first, Qt::LeftButton);
  QTest::mouseClick(&second, Qt::LeftButton);
  QCOMPARE(host.activePopover(), secondFrame);
  QVERIFY(!firstFrame->isVisible());
  QTest::mouseClick(&outside, Qt::LeftButton);
  QTRY_VERIFY(!host.activePopover());
  QCOMPARE(outsideClicks.count(), 1);
  QTest::mouseClick(&first, Qt::LeftButton);
  QTest::keyClick(&first, Qt::Key_Escape);
  QVERIFY(!host.activePopover());
  QTest::mouseClick(&first, Qt::LeftButton);
  first.hide();
  QTRY_VERIFY(!host.activePopover());

  // Overflow registration keeps using the original button, but visibility
  // and placement follow the visible overflow button.
  host.open(&first, &second);
  QCOMPARE(host.activePopover(), firstFrame);
  QCOMPARE(firstFrame->parentWidget(), &root);
  second.hide();
  QTRY_VERIFY(!host.activePopover());
}

inline void UxTests::uxP02_popupChildrenAndGeometry() {
  using namespace signalstudio;
  QWidget root;
  root.resize(640, 480);
  QWidget toolbar(&root);
  toolbar.setGeometry(500, 410, 130, 60);
  QPushButton anchor("Settings", &toolbar);
  anchor.setGeometry(10, 10, 100, 30);
  auto* content = new QWidget;
  auto* layout = new QVBoxLayout(content);
  auto* combo = new QComboBox(content);
  combo->addItems({"One", "Two", "Three"});
  layout->addWidget(combo);
  auto* extra = new QLabel("Extra settings", content);
  layout->addWidget(extra);
  ParameterInputPolicy policy(&root);
  SettingsPopoverHost host(&root);
  auto* frame = host.add(&anchor, content, "p02Settings");
  root.show();
  QVERIFY(QTest::qWaitForWindowExposed(&root));
  QTest::mouseClick(&anchor, Qt::LeftButton);
  QCOMPARE(host.activePopover(), frame);
  QVERIFY(root.rect().contains(frame->geometry()));
  QVERIFY(root.windowHandle());
  QTest::mouseClick(root.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                    combo->mapTo(&root, combo->rect().center()));
  QTRY_VERIFY(QApplication::activePopupWidget());
  QCOMPARE(host.activePopover(), frame);
  QTest::keyClick(combo->view(), Qt::Key_Escape);
  QTest::mouseClick(combo, Qt::LeftButton);
  QTRY_VERIFY(QApplication::activePopupWidget());
  QCOMPARE(host.activePopover(), frame);
  QTest::keyClick(combo->view(), Qt::Key_Escape);
  QTRY_VERIFY(!QApplication::activePopupWidget());
  QCOMPARE(host.activePopover(), frame);
  QVERIFY(frame->isVisible());
  QTest::mouseClick(combo, Qt::LeftButton);
  QTRY_VERIFY(QApplication::activePopupWidget());
  QTest::qWait(QApplication::doubleClickInterval() + 50);
  const QModelIndex second = combo->model()->index(1, combo->modelColumn());
  combo->view()->scrollTo(second);
  QTRY_VERIFY(combo->view()->visualRect(second).isValid());
  QTest::mouseClick(combo->view()->viewport(), Qt::LeftButton, Qt::NoModifier,
                    combo->view()->visualRect(second).center());
  QTRY_COMPARE(combo->currentIndex(), 1);
  QCOMPARE(host.activePopover(), frame);
  QTest::keyClick(combo, Qt::Key_Escape);
  QVERIFY(!host.activePopover());

  host.open(&anchor);
  const int initialHeight = frame->height();
  extra->setMinimumHeight(1000);
  QTRY_VERIFY(frame->height() > initialHeight);
  QVERIFY(root.rect().contains(frame->geometry()));
  auto* scroll = frame->findChild<QScrollArea*>();
  QVERIFY(scroll);
  QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
  root.resize(370, 240);
  toolbar.move(230, 180);
  QTRY_VERIFY(root.rect().contains(frame->geometry()));
  auto* stableParent = frame->parentWidget();
  combo->setFocus();
  QTRY_VERIFY(combo->hasFocus());
  host.reposition();
  QCOMPARE(frame->parentWidget(), stableParent);
  QVERIFY(combo->hasFocus());
  root.move(root.pos() + QPoint(10, 10));
  QTRY_VERIFY(root.rect().contains(frame->geometry()));
  toolbar.hide();
  QTRY_VERIFY(!host.activePopover());
}

inline void UxTests::uxS06_popoverDraftPolicy() {
  using namespace signalstudio;
  QWidget root;
  root.resize(640, 480);
  QPushButton anchor("Settings", &root), outside("Outside", &root);
  anchor.setGeometry(20, 20, 100, 30);
  outside.setGeometry(500, 420, 100, 30);
  auto* content = new QWidget;
  auto* layout = new QVBoxLayout(content);
  auto* spin = new ParameterIntSpinBox(content);
  spin->setRange(0, 1000);
  spin->setValue(12);
  layout->addWidget(spin);
  auto* combo = new QComboBox(content);
  combo->setEditable(true);
  combo->addItems({"Accepted", "Other"});
  layout->addWidget(combo);
  int comboCommits = 0;
  bindParameterCombo(combo, [&] {
    ++comboCommits;
    combo->setProperty("parameterCommittedText", combo->currentText());
  });
  // Install policy before the host to exercise newest-first application
  // filters: Outside must still commit before the deferred layer hide.
  ParameterInputPolicy policy(&root);
  SettingsPopoverHost host(&root);
  auto* frame = host.add(&anchor, content, "s06Settings");
  auto* editor = spin->findChild<QLineEdit*>();
  QVERIFY(editor);
  QSignalSpy values(spin, &QSpinBox::valueChanged);
  root.show();
  QVERIFY(QTest::qWaitForWindowExposed(&root));
  QTest::mouseClick(&anchor, Qt::LeftButton);
  QTest::mouseClick(editor, Qt::LeftButton);
  editor->selectAll();
  QTest::keyClicks(editor, "34");
  QCOMPARE(spin->value(), 12);
  QVERIFY(spin->property("parameterDraft").toBool());
  QEvent deactivate(QEvent::ApplicationDeactivate);
  QCoreApplication::sendEvent(qApp, &deactivate);
  QVERIFY(!host.activePopover());
  QCOMPARE(spin->value(), 12);
  QCOMPARE(values.count(), 0);
  QCOMPARE(editor->text(), QString("34"));
  QVERIFY(spin->property("parameterDraft").toBool());
  QTest::mouseClick(&anchor, Qt::LeftButton);
  QCOMPARE(host.activePopover(), frame);
  QCOMPARE(editor->text(), QString("34"));
  QCOMPARE(spin->value(), 12);
  QTest::keyClick(editor, Qt::Key_Escape);
  QVERIFY(!host.activePopover());
  QCOMPARE(spin->value(), 12);
  QCOMPARE(editor->text(), QString("12"));
  QVERIFY(!spin->property("parameterDraft").toBool());
  host.open(&anchor);
  QTest::mouseClick(editor, Qt::LeftButton);
  editor->selectAll();
  QTest::keyClicks(editor, "56");
  QTest::mouseClick(&outside, Qt::LeftButton);
  QTRY_VERIFY(!host.activePopover());
  QCOMPARE(spin->value(), 56);
  QCOMPARE(values.count(), 1);
  host.open(&anchor);
  QTest::mouseClick(combo->lineEdit(), Qt::LeftButton);
  combo->lineEdit()->selectAll();
  QTest::keyClicks(combo->lineEdit(), "Draft");
  QCOMPARE(comboCommits, 0);
  host.closeForContextChange();
  QVERIFY(!host.activePopover());
  QCOMPARE(combo->currentText(), QString("Accepted"));
  QCOMPARE(comboCommits, 0);
  QVERIFY(!combo->property("parameterDraft").toBool());
}
