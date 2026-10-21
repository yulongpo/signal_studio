#pragma once

// Include after UxTests declares uxS01_methodHistory,
// uxS02_conditionalRows and uxS03_spectralIndependence.
#include <QtTest>
#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QVBoxLayout>
#include "ui/parameter_input.h"
#include "ui/spectral_settings_widget.h"

namespace spectralUx {
using namespace signalstudio;

struct PersistenceEnabled {
    QVariant previous=qApp->property("uiStatePersistenceEnabled");
    PersistenceEnabled(){qApp->setProperty("uiStatePersistenceEnabled",true);}
    ~PersistenceEnabled(){qApp->setProperty("uiStatePersistenceEnabled",previous);}
};

struct Panel {
    QWidget root;
    ParameterInputPolicy policy{&root};
    SpectralSettingsWidget* psd;
    SpectralSettingsWidget* stft;
    Panel() {
        auto* layout=new QHBoxLayout(&root);
        psd=new SpectralSettingsWidget(true,&root);
        stft=new SpectralSettingsWidget(false,&root);
        layout->addWidget(psd);layout->addWidget(stft);
        root.resize(960,800);
        root.show();
    }
};

// Keyboard selection avoids an outside mouse click committing another draft
// before the method-switch handler can be exercised.
inline bool select(QComboBox* combo,int index) {
    if(!combo||!combo->isVisible()||!combo->isEnabled())return false;
    combo->setFocus();
    QTest::keyClick(combo,Qt::Key_Home);
    for(int i=0;i<index;++i)QTest::keyClick(combo,Qt::Key_Down);
    return combo->currentIndex()==index;
}
inline bool type(QWidget* field,const QString& text,bool commit=true) {
    if(!field||!field->isVisible()||!field->isEnabled())return false;
    auto* editor=field->findChild<QLineEdit*>();
    if(!editor||!editor->isVisible())return false;
    QTest::mouseClick(editor,Qt::LeftButton);
    QTest::keyClick(editor,Qt::Key_A,Qt::ControlModifier);
    QTest::keyClick(editor,Qt::Key_Backspace);
    QTest::keyClicks(editor,text);
    if(commit)QTest::keyClick(editor,Qt::Key_Return);
    return true;
}
inline bool rowVisible(SpectralSettingsWidget* panel,QWidget* field,bool visible) {
    auto* form=qobject_cast<QFormLayout*>(panel->layout());
    auto* label=form?form->labelForField(field):nullptr;
    return field&&label&&field->isVisible()==visible&&label->isVisible()==visible;
}
} // namespace spectralUx

inline void UxTests::uxS01_methodHistory() {
    using namespace spectralUx;
    PersistenceEnabled persistence;
    Panel ui;
    QVERIFY(QTest::qWaitForWindowExposed(&ui.root));
    auto* method=ui.psd->findChild<QComboBox*>("psdMethod");
    auto* window=ui.psd->findChild<QComboBox*>("psdWindow");
    auto* overlap=ui.psd->findChild<QComboBox*>("psdOverlap");
    auto* duration=ui.psd->findChild<QComboBox*>("psdSegmentMs");
    auto* beta=ui.psd->findChild<QDoubleSpinBox*>("psdKaiserBeta");
    auto* nw=ui.psd->findChild<QDoubleSpinBox*>("psdTimeBandwidth");
    auto* k=ui.psd->findChild<QSpinBox*>("psdTapers");
    auto* order=ui.psd->findChild<QSpinBox*>("psdBurgOrder");
    QVERIFY(method&&window&&overlap&&duration&&beta&&nw&&k&&order);
    QVERIFY(ui.psd->parameters()==SpectralParameters{});
    auto welch=SpectralParameters{};
    welch.window=SpectralWindow::Kaiser;welch.overlap=.65;
    welch.segmentMilliseconds=4;welch.kaiserBeta=11.12345;
    welch.timeBandwidth=8.123456;welch.tapers=9;welch.burgOrder=31;welch.removeMean=true;
    ui.psd->setParameters(welch,SpectrumStatistic::Minimum);
    // Simulate MainWindow's synchronous ordinary refresh after each edit.
    QObject::connect(ui.psd,&SpectralSettingsWidget::edited,ui.psd,[&]{
        ui.psd->setParameters(ui.psd->parameters(),SpectrumStatistic::Minimum);
    });
    QSignalSpy edits(ui.psd,&SpectralSettingsWidget::edited);
    const auto choose=[&](SpectralMethod m){return select(method,static_cast<int>(m));};
    const auto defaults=[](SpectralMethod m){auto p=SpectralParameters{};p.method=m;if(m==SpectralMethod::Bartlett)p.overlap=0;return p;};

    QVERIFY(choose(SpectralMethod::Periodogram));
    QVERIFY(ui.psd->parameters()==defaults(SpectralMethod::Periodogram));
    QVERIFY(choose(SpectralMethod::Bartlett));
    QVERIFY(ui.psd->parameters()==defaults(SpectralMethod::Bartlett));
    QVERIFY(choose(SpectralMethod::Welch));
    QVERIFY(ui.psd->parameters()==welch);
    QVERIFY(choose(SpectralMethod::Multitaper));
    QVERIFY(ui.psd->parameters()==defaults(SpectralMethod::Multitaper));
    QVERIFY(type(nw,"4.5"));QVERIFY(type(k,"7"));
    const auto multitaper=ui.psd->parameters();
    QCOMPARE(multitaper.timeBandwidth,4.5);QCOMPARE(multitaper.tapers,7);
    QVERIFY(choose(SpectralMethod::Burg));
    QVERIFY(ui.psd->parameters()==defaults(SpectralMethod::Burg));
    QVERIFY(type(order,"27"));
    QVERIFY(choose(SpectralMethod::Multitaper));
    QVERIFY(ui.psd->parameters()==multitaper);
    QVERIFY(choose(SpectralMethod::Burg));QCOMPARE(ui.psd->parameters().burgOrder,27);
    QVERIFY(choose(SpectralMethod::Welch));QVERIFY(ui.psd->parameters()==welch);
    QVERIFY(select(window,static_cast<int>(SpectralWindow::Hann)));
    QVERIFY(!beta->isVisible());
    QVERIFY(select(window,static_cast<int>(SpectralWindow::Kaiser)));
    QCOMPARE(ui.psd->parameters().kaiserBeta,welch.kaiserBeta);
    QVERIFY(type(beta,"12.5"));welch.kaiserBeta=12.5;
    QVERIFY(select(window,static_cast<int>(SpectralWindow::Hann)));
    QVERIFY(select(window,static_cast<int>(SpectralWindow::Kaiser)));
    QCOMPARE(beta->value(),12.5);

    // Uncommitted spin text survives refresh but cannot enter method history.
    QVERIFY(type(beta,"14.5",false));
    const auto betaDraft=beta->findChild<QLineEdit*>()->text();
    ui.psd->setParameters(welch,SpectrumStatistic::Minimum);
    QCOMPARE(beta->findChild<QLineEdit*>()->text(),betaDraft);
    QCOMPARE(ui.psd->parameters().kaiserBeta,12.5);
    QVERIFY(choose(SpectralMethod::Burg));
    QVERIFY(choose(SpectralMethod::Welch));QVERIFY(ui.psd->parameters()==welch);

    QVERIFY(type(overlap,"62",false));
    const int beforeDraft=edits.count();
    ui.psd->setParameters(welch,SpectrumStatistic::Minimum);
    QCOMPARE(overlap->currentText(),QString("62"));
    QVERIFY(overlap->property("parameterDraft").toBool());
    QCOMPARE(edits.count(),beforeDraft);
    QVERIFY(choose(SpectralMethod::Bartlett));QCOMPARE(ui.psd->parameters().overlap,0.0);
    QVERIFY(choose(SpectralMethod::Welch));QCOMPARE(ui.psd->parameters().overlap,.65);

    // Invalid custom commits must leave text, committed state and persistence
    // untouched. Check both parsing and finite/range validation boundaries.
    for(auto* combo:{overlap,duration}) {
        const auto accepted=combo->property("lastValidValue");
        const auto recent=combo->property("lastCustomValue");
        const auto key="customCombos/"+combo->objectName();
        const auto saved=QSettings().value(key);
        const QStringList invalid=combo==overlap?QStringList{"100","-1","nan",""}:QStringList{"-1","1000000001","nan",""};
        for(const auto& text:invalid) {
            const int before=edits.count();
            QVERIFY(type(combo,text));
            QCOMPARE(combo->currentText(),text);
            QVERIFY(combo->property("parameterDraft").toBool());
            QVERIFY(combo->property("invalid").toBool());
            QVERIFY(!combo->property("parameterError").toString().isEmpty());
            QCOMPARE(combo->toolTip(),combo->property("parameterError").toString());
            QCOMPARE(combo->property("lastValidValue"),accepted);
            QCOMPARE(combo->property("lastCustomValue"),recent);
            QCOMPARE(QSettings().value(key),saved);
            QCOMPARE(edits.count(),before);
            ui.psd->setParameters(welch,SpectrumStatistic::Minimum);
            QCOMPARE(combo->currentText(),text);
            QVERIFY(combo->property("invalid").toBool());
            QCOMPARE(edits.count(),before);
        }
        // Shared Escape handling restores the committed text and error state.
        QTest::keyClick(combo->lineEdit(),Qt::Key_Escape);
        QVERIFY(!combo->property("parameterDraft").toBool());
        QVERIFY(!combo->property("invalid").toBool());
        QCOMPARE(combo->currentText(),combo->property("parameterCommittedText").toString());
    }
    QVERIFY(type(overlap,"100"));
    QVERIFY(choose(SpectralMethod::Burg));
    QVERIFY(choose(SpectralMethod::Welch));QVERIFY(ui.psd->parameters()==welch);
    QVERIFY(type(overlap,"61"));
    QVERIFY(!overlap->property("invalid").toBool());
    QCOMPARE(overlap->property("lastCustomValue").toDouble(),61.0);
    QCOMPARE(QSettings().value("customCombos/psdOverlap").toDouble(),61.0);
    QVERIFY(type(duration,"-1"));
    QVERIFY(type(duration,"2.5"));
    QVERIFY(!duration->property("invalid").toBool());
    QCOMPARE(QSettings().value("customCombos/psdSegmentMs").toDouble(),2.5);
    welch.overlap=.61;welch.segmentMilliseconds=2.5;
    QVERIFY(choose(SpectralMethod::Bartlett));
    QVERIFY(choose(SpectralMethod::Welch));QVERIFY(ui.psd->parameters()==welch);
    QCOMPARE(ui.psd->statistic(),SpectrumStatistic::Minimum);
}

inline void UxTests::uxS02_conditionalRows() {
    using namespace spectralUx;
    Panel ui;
    QVERIFY(QTest::qWaitForWindowExposed(&ui.root));
    auto* method=ui.psd->findChild<QComboBox*>("psdMethod");
    auto* window=ui.psd->findChild<QComboBox*>("psdWindow");
    auto* overlap=ui.psd->findChild<QComboBox*>("psdOverlap");
    auto* duration=ui.psd->findChild<QComboBox*>("psdSegmentMs");
    auto* beta=ui.psd->findChild<QDoubleSpinBox*>("psdKaiserBeta");
    auto* nw=ui.psd->findChild<QDoubleSpinBox*>("psdTimeBandwidth");
    auto* k=ui.psd->findChild<QSpinBox*>("psdTapers");
    auto* order=ui.psd->findChild<QSpinBox*>("psdBurgOrder");
    auto* statistic=ui.psd->findChild<QComboBox*>("psdStatistic");
    QVERIFY(method&&window&&overlap&&duration&&beta&&nw&&k&&order&&statistic);
    for(int index=0;index<=4;++index) {
        QVERIFY(select(method,index));
        const bool mt=index==3,burg=index==4,period=index==0;
        QVERIFY(rowVisible(ui.psd,window,!mt&&!burg));
        QVERIFY(rowVisible(ui.psd,beta,false));
        QVERIFY(rowVisible(ui.psd,nw,mt));QVERIFY(rowVisible(ui.psd,k,mt));
        QVERIFY(rowVisible(ui.psd,order,burg));
        QVERIFY(rowVisible(ui.psd,overlap,!period&&index!=1));
        QVERIFY(rowVisible(ui.psd,duration,!period));
        QVERIFY(rowVisible(ui.psd,statistic,!period));
        if(index==1){QCOMPARE(ui.psd->parameters().overlap,0.0);QVERIFY(method->toolTip().contains("0%"));}
    }
    QVERIFY(select(method,static_cast<int>(SpectralMethod::Periodogram)));
    QVERIFY(select(window,static_cast<int>(SpectralWindow::Kaiser)));
    QVERIFY(rowVisible(ui.psd,beta,true));
    QVERIFY(select(window,static_cast<int>(SpectralWindow::Hann)));
    QVERIFY(rowVisible(ui.psd,beta,false));
    // Toolbar ownership removes this row. Subsequent refresh/method changes
    // must never address the removed row with QFormLayout::setRowVisible.
    statistic=ui.psd->takeStatisticControl();
    auto* form=qobject_cast<QFormLayout*>(ui.psd->layout());
    QVERIFY(form->indexOf(statistic)<0);
    statistic->setParent(&ui.root);statistic->show();
    statistic->move(20,ui.root.height()-40);
    ui.psd->setParameters(ui.psd->parameters(),SpectrumStatistic::Maximum);
    QCOMPARE(ui.psd->statistic(),SpectrumStatistic::Maximum);
    QVERIFY(select(method,static_cast<int>(SpectralMethod::Welch)));
    QCOMPARE(ui.psd->statistic(),SpectrumStatistic::Maximum);
    QVERIFY(statistic->isVisible());
    QCOMPARE(ui.psd->findChild<QDoubleSpinBox*>("psdKaiserBeta"),beta);
}

inline void UxTests::uxS03_spectralIndependence() {
    using namespace spectralUx;
    Panel ui;
    QVERIFY(QTest::qWaitForWindowExposed(&ui.root));
    auto* psdMethod=ui.psd->findChild<QComboBox*>("psdMethod");
    auto* stftMethod=ui.stft->findChild<QComboBox*>("stftMethod");
    auto* psdOverlap=ui.psd->findChild<QComboBox*>("psdOverlap");
    auto* stftOverlap=ui.stft->findChild<QComboBox*>("stftOverlap");
    auto* stftNw=ui.stft->findChild<QDoubleSpinBox*>("stftTimeBandwidth");
    QVERIFY(psdMethod&&stftMethod&&psdOverlap&&stftOverlap&&stftNw);
    QSignalSpy psdEdits(ui.psd,&SpectralSettingsWidget::edited);
    QSignalSpy stftEdits(ui.stft,&SpectralSettingsWidget::edited);
    QVERIFY(type(psdOverlap,"70"));
    const auto psd=ui.psd->parameters();const int psdCount=psdEdits.count();
    QVERIFY(type(stftOverlap,"25"));
    const auto stft=ui.stft->parameters();
    QVERIFY(select(stftMethod,1));
    auto expected=SpectralParameters{};expected.method=SpectralMethod::Multitaper;
    QVERIFY(ui.stft->parameters()==expected);
    QVERIFY(type(stftNw,"5.25"));
    const auto stftMt=ui.stft->parameters();
    QVERIFY(ui.psd->parameters()==psd);QCOMPARE(psdEdits.count(),psdCount);
    QVERIFY(select(stftMethod,0));QVERIFY(ui.stft->parameters()==stft);
    const int stftCount=stftEdits.count();
    QVERIFY(select(psdMethod,static_cast<int>(SpectralMethod::Bartlett)));
    QCOMPARE(ui.psd->parameters().overlap,0.0);
    ui.psd->setParameters(ui.psd->parameters(),SpectrumStatistic::Minimum);
    QVERIFY(ui.stft->parameters()==stft);QCOMPARE(stftEdits.count(),stftCount);
    QVERIFY(select(psdMethod,static_cast<int>(SpectralMethod::Welch)));
    QVERIFY(ui.psd->parameters()==psd);
    // Repeated domain refreshes update only the active method cache.
    for(int i=0;i<3;++i){ui.psd->setParameters(psd,SpectrumStatistic::Minimum);ui.stft->setParameters(stft);}
    QCOMPARE(ui.psd->statistic(),SpectrumStatistic::Minimum);
    QVERIFY(select(stftMethod,1));QVERIFY(ui.stft->parameters()==stftMt);
    QVERIFY(select(stftMethod,0));QVERIFY(ui.stft->parameters()==stft);
    QVERIFY(ui.psd->parameters()==psd);
}
