#include <QApplication>
#include <QJsonArray>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QtTest>
#include <cmath>

#include "ui/charts/axis_layout.h"
#include "ui/charts/interaction_feedback.h"
#include "ui/charts/plot_widget.h"

using namespace signalstudio;

class UxR3AxisTests : public QObject {
  Q_OBJECT
 private slots:
  void initTestCase() { qApp->setProperty("softwareRenderer", true); }
  void measuredInkAndIndependentBands();
  void largerFontsAndScientificTicksGrowBudget();
  void sharedGeometryAndModeSwitches();
  void compactHeaderRecoversActualPlotArea();
  void compactHeaderRetainsPhysicalDescription();
  void physicalAxesAndSourceUnits();
  void coordinateAndHitZones_data();
  void coordinateAndHitZones();
  void auxiliaryWheelAndEscape();
};

void UxR3AxisTests::measuredInkAndIndependentBands() {
  using namespace axis_layout;
  QFont font("Consolas");
  font.setPixelSize(12);
  const QFontMetricsF metrics(font);
  const Labels labels{{"0", "1", "2"}, {"-32768.0", "0.0", "32768.0"},
                       "时间 (ms)", "幅值（ADC 计数）", "ADC 计数"};
  const auto a = measure(labels, metrics, TitleMode::UnitAtTop, 1200, 300);
  const auto c = measure(labels, metrics, TitleMode::Rotated, 1200, 300);
  const auto layout = arrange(QSizeF(1200, 300), a);
  QVERIFY(a.left < 88);
  QVERIFY(c.left > a.left);
  for (const auto& tick : labels.yTicks)
    QVERIFY(a.left - 7 - textWidth(metrics, tick) >= 8);
  QVERIFY(layout.header.bottom() < layout.plot.top() - a.tickHeight / 2);
  QVERIFY(layout.xTicks.top() > layout.plot.bottom() + a.tickHeight / 2);
  QVERIFY(layout.xTicks.bottom() < layout.xTitle.top());
  QVERIFY(layout.xTitle.bottom() <= 300);
}

void UxR3AxisTests::largerFontsAndScientificTicksGrowBudget() {
  using namespace axis_layout;
  QFont small("Consolas");
  small.setPixelSize(10);
  QFont large("Microsoft YaHei UI");
  large.setPixelSize(24);
  const Labels shortTicks{{"0", "1"}, {"-1", "1"}, "时间 (s)",
                           "幅值（原始幅度）", "原始幅度"};
  auto longTicks = shortTicks;
  longTicks.yTicks = {"-9.87654321e+123", "9.87654321e+123"};
  const auto a = measure(shortTicks, QFontMetricsF(small), TitleMode::UnitAtTop, 1200, 400);
  const auto b = measure(longTicks, QFontMetricsF(large), TitleMode::UnitAtTop, 1200, 400);
  QVERIFY(b.left > a.left);
  QVERIFY(b.left > 88);  // Long labels win over any 48–64 px target.
  QVERIFY(b.bottom > a.bottom);
  const auto shared = sharedBudget(a, b);
  QCOMPARE(arrange(QSizeF(1200, 400), shared).plot.left(),
           arrange(QSizeF(1200, 700), sharedBudget(b, a)).plot.left());
}

void UxR3AxisTests::sharedGeometryAndModeSwitches() {
  Session session;
  session.addDemoFile();
  QWidget host;
  PlotWidget auxiliary(session, PlotWidget::Kind::Auxiliary, &host);
  PlotWidget main(session, PlotWidget::Kind::Main, &host);
  auxiliary.resize(1200, 280);
  main.resize(1200, 700);
  QFont font("Consolas"); font.setPixelSize(12);
  auxiliary.setFont(font);
  main.setFont(font);
  session.setAuxiliaryRange(-1234567, 9876543);
  const auto view = session.activeFile()->view;
  for (auto mode : {PlotWidget::AxisTitleMode::UnitAtTop,
                    PlotWidget::AxisTitleMode::Header,
                    PlotWidget::AxisTitleMode::Rotated}) {
    main.setAxisTitleMode(mode);
    QCOMPARE(auxiliary.axisTitleMode(), mode);
    QCOMPARE(main.plotRect().left(), auxiliary.plotRect().left());
    QCOMPARE(main.plotRect().top(), auxiliary.plotRect().top());
    QCOMPARE(session.activeFile()->view, view);
  }
  main.setAxisTitleMode(PlotWidget::AxisTitleMode::UnitAtTop);
  const auto before = main.plotRect().left();
  font.setPixelSize(24);
  auxiliary.setFont(font);
  QVERIFY(main.plotRect().left() > before);
  QCOMPARE(main.plotRect().left(), auxiliary.plotRect().left());
  session.activeFile()->display.colorScale = true;
  QCOMPARE(main.axisLayout().plot.right(), main.width() - 70.0);
}

void UxR3AxisTests::physicalAxesAndSourceUnits() {
  Session session;
  session.addDemoFile("GHz", 4e9, 10e9, .02);
  PlotWidget main(session, PlotWidget::Kind::Main);
  PlotWidget aux(session, PlotWidget::Kind::Auxiliary);
  main.resize(1200, 500);
  aux.resize(1200, 280);
  auto* file = session.activeFile();
  file->display.mainMode = MainMode::TimeFrequency;
  QCOMPARE(main.axisLayoutStatistics()["yUnit"].toString(), "GHz");
  file->display.mainMode = MainMode::Waterfall;
  QVERIFY(main.axisLayoutStatistics()["xTitle"].toString().contains("GHz"));
  QVERIFY(main.axisLayoutStatistics()["yTitle"].toString().contains("时间"));
  file->metadata.demo = false;
  file->metadata.sampleFormat.componentEncoding = ComponentEncoding::Float32;
  file->display.auxiliaryMode = AuxiliaryMode::Psd;
  QCOMPARE(aux.axisLayoutStatistics()["yUnit"].toString(), "dB(unit^2/Hz)");
  file->display.auxiliaryMode = AuxiliaryMode::Waveform;
  QCOMPARE(aux.axisLayoutStatistics()["yUnit"].toString(), "原始幅度");
}

void UxR3AxisTests::coordinateAndHitZones_data() {
  QTest::addColumn<bool>("waterfall");
  QTest::addColumn<int>("mode");
  for (int mode = 0; mode < 4; ++mode)
    for (bool waterfall : {false, true})
      QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(waterfall)))
          << waterfall << mode;
}

void UxR3AxisTests::coordinateAndHitZones() {
  QFETCH(bool, waterfall);
  QFETCH(int, mode);
  Session session;
  session.addDemoFile();
  PlotWidget main(session, PlotWidget::Kind::Main);
  main.resize(1200, 700);
  main.setAxisTitleMode(static_cast<PlotWidget::AxisTitleMode>(mode));
  auto* file = session.activeFile();
  file->display.mainMode = waterfall ? MainMode::Waterfall : MainMode::TimeFrequency;
  const auto view = file->view;
  const auto rect = main.plotRect();
  QCOMPARE(main.toPixel(view.time.begin, view.frequency.lowerHz),
           waterfall ? rect.topLeft() : rect.bottomLeft());
  QCOMPARE(main.toPixel(view.time.end, view.frequency.upperHz),
           waterfall ? rect.bottomRight() : rect.topRight());
  QCOMPARE(chart_feedback::zoneAt(QPointF(rect.left() - 10, rect.center().y()), rect),
           chart_feedback::Zone::YAxis);
  QCOMPARE(chart_feedback::zoneAt(QPointF(rect.center().x(), rect.bottom() + 10), rect),
           chart_feedback::Zone::XAxis);
}

void UxR3AxisTests::auxiliaryWheelAndEscape() {
  Session session;
  session.addDemoFile();
  QWidget host;
  PlotWidget auxiliary(session, PlotWidget::Kind::Auxiliary, &host);
  PlotWidget main(session, PlotWidget::Kind::Main, &host);
  auxiliary.resize(1200, 280);
  main.resize(1200, 700);
  const auto view = session.activeFile()->view;
  const auto previous = session.activeFile()->display.auxiliaryMax -
                         session.activeFile()->display.auxiliaryMin;
  const auto rect = auxiliary.plotRect();
  const QPointF point(rect.left() - 10, rect.center().y());
  QWheelEvent wheel(point, auxiliary.mapToGlobal(point.toPoint()), {}, QPoint(0, 120),
                     Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(&auxiliary, &wheel);
  const auto next = session.activeFile()->display.auxiliaryMax -
                     session.activeFile()->display.auxiliaryMin;
  QVERIFY(next < previous);
  QCOMPARE(session.activeFile()->view, view);
  QCOMPARE(auxiliary.plotRect(), rect); // The wheel batch freezes both axes.
  QCOMPARE(main.plotRect().left(), rect.left());
  auxiliary.cancelGesture();
  const auto minimum = session.activeFile()->display.auxiliaryMin;
  const auto maximum = session.activeFile()->display.auxiliaryMax;
  const auto dragRect = auxiliary.plotRect();
  const auto position = QPointF(dragRect.left() - 10, dragRect.center().y());
  QMouseEvent press(QEvent::MouseButtonPress, position, position,
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&auxiliary, &press);
  QMouseEvent move(QEvent::MouseMove, position + QPointF(0, 25),
                    position + QPointF(0, 25), Qt::NoButton,
                    Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&auxiliary, &move);
  QVERIFY(session.activeFile()->display.auxiliaryMin != minimum);
  QCOMPARE(auxiliary.plotRect(), dragRect);
  QTest::keyClick(&auxiliary, Qt::Key_Escape);
  QCOMPARE(session.activeFile()->display.auxiliaryMin, minimum);
  QCOMPARE(session.activeFile()->display.auxiliaryMax, maximum);
  QCOMPARE(session.activeFile()->view, view);
}

void UxR3AxisTests::compactHeaderRecoversActualPlotArea() {
  Session session;
  session.addDemoFile();
  QWidget host;
  PlotWidget main(session, PlotWidget::Kind::Main, &host);
  PlotWidget auxiliary(session, PlotWidget::Kind::Auxiliary, &host);
  main.resize(1200, 700); auxiliary.resize(1200, 170);
  QFont font("Consolas"); font.setPixelSize(12);
  main.setFont(font); auxiliary.setFont(font);
  QCOMPARE(main.axisTitleMode(), PlotWidget::AxisTitleMode::CompactHeader);
  const auto state = session.activeFile()->view;
  main.setAxisTitleMode(PlotWidget::AxisTitleMode::UnitAtTop);
  const auto before = auxiliary.plotRect();
  const auto line = auxiliary.axisLayout().budget.tickHeight;
  main.setAxisTitleMode(PlotWidget::AxisTitleMode::CompactHeader);
  const auto after = auxiliary.plotRect();
  QVERIFY(after.height() - before.height() >= 2 * line);
  QCOMPARE(after.width(), before.width());
  QCOMPARE(session.activeFile()->view, state);
  QVERIFY(auxiliary.axisLayout().header.isEmpty());
  QVERIFY(auxiliary.axisLayout().xTitle.isEmpty());
  QVERIFY(auxiliary.axisLayout().xTicks.bottom() < auxiliary.height());
  font.setPixelSize(28); main.setFont(font);
  QCOMPARE(auxiliary.plotRect().top(), after.top());
  QCOMPARE(auxiliary.plotRect().height(), after.height());
  QCOMPARE(main.plotRect().left(), auxiliary.plotRect().left());
  QVERIFY(main.plotRect().top() > auxiliary.plotRect().top());
}

void UxR3AxisTests::compactHeaderRetainsPhysicalDescription() {
  Session session; session.addDemoFile("RF", 20e6, 10e9, 2);
  PlotWidget main(session, PlotWidget::Kind::Main);
  main.resize(1200, 500);
  session.activeFile()->display.absoluteFrequency = true;
  session.activeFile()->view.frequency = {10e9, 10e9 + 20};
  auto description = main.axisDescription();
  QVERIFY(description.fullText().contains("RF"));
  QVERIFY(description.fullText().contains("F₀"));
  QVERIFY(description.fullText().contains("Hz"));
  AxisHeaderLabel header;
  header.setDescription(description);
  header.resize(header.minimumSizeHint()); header.show();
  QCoreApplication::processEvents();
  QCOMPARE(header.text(), description.shortText());
  QCOMPARE(header.toolTip(), description.fullText());
  QVERIFY(header.width() >= header.fontMetrics().horizontalAdvance(header.text()));
  session.activeFile()->display.absoluteFrequency = false;
  QVERIFY(main.axisDescription().fullText().contains("基带"));
}

QTEST_MAIN(UxR3AxisTests)
#include "ux_r3_axis_tests.moc"
