#include <QtTest>
#include <QPushButton>
#include <QStyleOptionSpinBox>
#include <QVBoxLayout>
#include <QWindow>
#include "ui/parameter_input.h"
using namespace signalstudio;
class ParameterInputTests : public QObject {
    Q_OBJECT
private slots:
    void mouseSteps_data() {
        QTest::addColumn<bool>("styled");
        QTest::newRow("native-style") << false;
        QTest::newRow("application-style") << true;
    }
    void mouseSteps() {
        QFETCH(bool,styled);
        QWidget root;root.resize(500,220);QVBoxLayout layout(&root);
        if(styled)root.setStyleSheet("QWidget{background:#101d2f;color:#dbe8f8;font-size:12px;} QDoubleSpinBox,QLineEdit{border:1px solid #314963;background:#0e1a2a;border-radius:4px;padding:5px 7px;font-size:11px;}");
        ParameterDoubleSpinBox real;real.setRange(-20,20);real.setDecimals(4);real.setSingleStep(.1);real.setValue(.35);
        ParameterIntSpinBox integer;integer.setRange(2,32);integer.setValue(10);
        QPushButton outside("outside");layout.addWidget(&real);layout.addWidget(&integer);layout.addWidget(&outside);
        ParameterInputPolicy policy(&root);root.show();QVERIFY(QTest::qWaitForWindowExposed(&root));
        const auto click=[&](QAbstractSpinBox* spin,QStyle::SubControl control) {
            QStyleOptionSpinBox option;option.initFrom(spin);option.rect=spin->rect();option.frame=true;
            option.buttonSymbols=QAbstractSpinBox::UpDownArrows;
            option.stepEnabled=QAbstractSpinBox::StepUpEnabled|QAbstractSpinBox::StepDownEnabled;
            const auto button=spin->style()->subControlRect(QStyle::CC_SpinBox,&option,control,spin);
            QVERIFY2(!button.isEmpty(),"step button must have a hit area");
            qInfo()<<control<<button<<"child"<<spin->childAt(button.center());
            QTest::mouseClick(root.windowHandle(),Qt::LeftButton,Qt::NoModifier,spin->mapTo(&root,button.center()));
        };
        click(&real,QStyle::SC_SpinBoxUp);QCOMPARE(real.value(),.45);
        click(&real,QStyle::SC_SpinBoxDown);QCOMPARE(real.value(),.35);
        click(&integer,QStyle::SC_SpinBoxUp);QCOMPARE(integer.value(),11);
        click(&integer,QStyle::SC_SpinBoxDown);QCOMPARE(integer.value(),10);
        auto* edit=real.findChild<QLineEdit*>();edit->setFocus();edit->selectAll();QTest::keyClicks(edit,"0.65");
        QCOMPARE(real.value(),.35);click(&real,QStyle::SC_SpinBoxUp);QCOMPARE(real.value(),.75);
        edit->setFocus();QTest::keyClick(edit,Qt::Key_Up);QCOMPARE(real.value(),.75);
        QTest::keyClick(edit,Qt::Key_Return);QCOMPARE(real.value(),.85);
    }
    void textDraftPolicy() {
        QWidget root;QVBoxLayout layout(&root);ParameterTextEdit text;QPushButton outside("outside");layout.addWidget(&text);layout.addWidget(&outside);
        ParameterInputPolicy policy(&root);QString accepted="old";int commits=0;text.displayText(accepted);
        text.apply=[&](const QString& value){if(value=="bad"){setParameterError(&text,"invalid rings");return false;}accepted=value;++commits;return true;};
        root.show();QVERIFY(QTest::qWaitForWindowExposed(&root));text.setFocus();text.selectAll();QTest::keyClicks(&text,"draft");
        text.displayText("background");QCOMPARE(text.text(),QString("draft"));QCOMPARE(commits,0);
        QFocusEvent blur(QEvent::FocusOut,Qt::ActiveWindowFocusReason);QCoreApplication::sendEvent(&text,&blur);QCOMPARE(commits,0);
        QTest::keyClick(&text,Qt::Key_Return);QCOMPARE(accepted,QString("draft"));QCOMPARE(commits,1);
        text.selectAll();QTest::keyClicks(&text,"outside");QTest::mouseClick(&outside,Qt::LeftButton);QCOMPARE(accepted,QString("outside"));
        text.setFocus();text.selectAll();QTest::keyClicks(&text,"bad");QTest::keyClick(&text,Qt::Key_Return);QCOMPARE(accepted,QString("outside"));QVERIFY(text.property("parameterDraft").toBool());
        policy.setContext("another-channel");QCOMPARE(text.text(),QString("outside"));QVERIFY(!text.property("parameterDraft").toBool());
    }
};
QTEST_MAIN(ParameterInputTests)
#include "parameter_input_tests.moc"
