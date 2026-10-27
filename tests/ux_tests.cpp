#include "chart_settings_helpers.h"
#include <QtTest>
#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QListView>
#include <QMessageBox>
#include <QLabel>
#include <QScrollArea>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>
#include <QClipboard>
#include <QDialog>
#include <QInputDialog>
#include <QMenu>
#include <QPlainTextEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTableView>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include "ui/annotations/annotation_panel.h"
#include "infrastructure/label_import.h"
#include "ui/annotations/annotation_evidence_widget.h"

#include "app/main_window.h"
#include "infrastructure/annotation_json.h"
#include "ui/controls/adaptive_value_edit.h"
#include "ui/detection/detection_card.h"
#include "ui/detection/detection_panel.h"
#include "ui/charts/plot_widget.h"

using namespace signalstudio;

namespace {
QString addSource(MainWindow& window, const QString& directory, const QString& name) {
  QFile source(directory + '/' + name);
  if (!source.open(QIODevice::WriteOnly) ||
      source.write(QByteArray(80000, '\0')) != 80000)
    return "Cannot create the IQ source";
  source.close();
  QString issue;
  if (!window.addIqFile(source.fileName(), &issue)) return issue;
  QElapsedTimer timer;
  timer.start();
  while (window.session().activeFile()->metadata.availability.status == LoadStatus::Loading &&
         timer.elapsed() < 10000)
    QTest::qWait(10);
  return window.session().activeFile()->metadata.availability.status == LoadStatus::Ready
             ? QString{} : "IQ source did not finish loading";
}

DetectionRun historyFor(MainWindow& window) {
  const auto& file = *window.session().activeFile();
  DetectionRun run;
  run.lifecycle = DetectionLifecycle::Completed;
  run.request.runId = "ux-history";
  run.request.sourceIdentity = annotationSourceIdentity(file.metadata, window.session().projectGeneration());
  run.request.profile = *findDetectionProfile("ffscn-gui-iq-v1");
  run.request.profile.fftSize = 8192;
  run.request.profile.hopSeconds = .004;
  run.request.profile.modelFrames = 6;
  run.request.profile.modelHopFrames = 3;
  run.request.spectrumMode = DetectionSpectrumMode::Mean;
  run.request.parameters.scoreThreshold = .42;
  run.request.parameters.timeStrategy = DetectionTimeStrategy::Roi;
  run.request.requestedRange = fullRange(file.metadata);
  run.request.requestedRange.time = {100, 1100};
  run.request.rangeOrigin = "visible";
  return run;
}

void expose(MainWindow& window) {
  window.resize(2560, 1440);
  window.show();
  QTest::qWait(30);
  window.refresh();
}

void focusHistory(MainWindow& window, const QString& id) {
  auto* list = window.findChild<QListView*>("detectionRunList");
  for (int row = 0; row < list->model()->rowCount(); ++row) {
    const auto index = list->model()->index(row, 0);
    if (index.data(Qt::UserRole).toString() == id) {
      list->setCurrentIndex(index);
      break;
    }
  }
  if (auto* tab = window.findChild<QAbstractButton*>("taskTab"))
    QTest::mouseClick(tab, Qt::LeftButton);
}
}  // namespace

class UxTests : public QObject {
  Q_OBJECT
 private:
  QTemporaryDir settings_;
 private slots:
  void uxR5_threeTabsReadOnlyAndConversion();
  void uxR5_layersAndIndependentFilters();
  void uxR5_readOnlyMenusAndCommandGuards();
  void uxR5_crossViewAndUnknownObservation();
  void uxP01_popupDismissal();
  void uxP02_popupChildrenAndGeometry();
  void uxS06_popoverDraftPolicy();
  void uxR01_chartToolbarLayout();
  void uxR02_chartSettingsStayStable();
  void uxS01_methodHistory();
  void uxS02_conditionalRows();
  void uxS03_spectralIndependence();
  void uxS03_framePsdPreservesSettings();
  void uxS06_invalidDraftRecovery();
  void uxR02_inspectorRouting();
  void uxR02_fileProperties();
  void uxR02_paletteWorkspaceOwnership();
  void uxD04_modelSpectrumMapping();
  void uxD05_foldedParametersAndFirstScreen();
  void uxD08_preflightDetails();
  void uxR03_taskKeyboardAndDetails();
  void uxR03_taskRowActions();
  void uxD03_activeTaskCancellationAndFrozenDisplay();
  void uxD09_intermediatePreviewAndRedetectClear();
  void uxD01_topDerivationDiscardsCrossFileDraft();
  void uxR01_lateRunFilterPopupAndRefresh();
  void roundTwoFlatResponsiveControls();
  void roundTwoNarrowbandPsdAndPages();
  void initTestCase() {
    QCoreApplication::setOrganizationName("SignalStudioTests");
    QCoreApplication::setApplicationName("WidebandUx");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    QApplication::setFont(QFont("Microsoft YaHei UI", 10));
  }
  void init() { QSettings().clear(); }

  void uxR4_evidenceDisclosureAndFullCopy() {
    MainWindow window;
    window.session().addDemoFile();
    auto* file = window.session().activeFile();
    const auto id = window.session().addMark({{1000, 4000}, {99.9e6,100.1e6}});
    auto* mark = findAnnotation(*file,id);
    QVERIFY(mark);
    auto origin = std::make_shared<OriginEvidence>();
    origin->originalFieldsJson = "{\"payload\":\"" + std::string(16000,'x') + "END_OF_ORIGINAL\"}";
    origin->originalSegments = mark->segments;
    origin->evidenceRefs = {"first", "last-complete-reference"};
    mark->originEvidence = origin;
    for(auto mode:{AnnotationEvidenceWidget::Mode::Inspector,
                   AnnotationEvidenceWidget::Mode::Auxiliary}) {
      AnnotationEvidenceWidget evidence(mode);
      evidence.resize(360,200);
      evidence.setAnnotation(mark,file);
      evidence.show();
      QTest::qWait(20);
      auto* dialog = evidence.findChild<QDialog*>("annotationEvidenceDetailsDialog");
      auto* raw = evidence.findChild<QPlainTextEdit*>("annotationEvidence");
      QVERIFY(dialog && raw && !dialog->isVisible() && !raw->isVisible());
      QVERIFY(!evidence.findChild<QLabel*>("annotationEvidenceSummary")->text().contains(QString::fromStdString(id)));
      evidence.findChild<QToolButton*>("annotationEvidenceDetailsToggle")->click();
      QVERIFY(dialog->isVisible());
      evidence.findChild<QPushButton*>("annotationCopyEvidence")->click();
      const auto copied=QApplication::clipboard()->text();
      QVERIFY(copied.contains("END_OF_ORIGINAL"));
      QVERIFY(copied.contains("last-complete-reference"));
      QVERIFY(copied.contains(QString::fromStdString(id)));
      QVERIFY(copied.contains("revisions"));
      QCOMPARE(mark->originEvidence,origin);
      evidence.setAnnotation(nullptr,file);
      QVERIFY(!dialog->isVisible());
    }
  }

  void uxR4_currentObjectAndActionStates() {
    MainWindow window;
    window.session().addDemoFile();
    auto* file=window.session().activeFile();
    const auto id=window.session().addMark({{1000,4000},{99.9e6,100.1e6}});
    QVERIFY(window.session().annotations().addSegment(*file,id,{{5000,8000},{99.9e6,100.1e6}}).ok);
    const auto other=window.session().addMark({{9000,11000},{100e6,100.2e6}});
    const auto* mark=findAnnotation(*file,id);
    QVERIFY(window.session().annotations().select(*file,{id},id,mark->segments.front().id).ok);
    expose(window);
    window.findChild<QPushButton*>("contextAnnotation")->click();
    QVERIFY(window.findChild<QWidget*>("annotationPage")->isVisible());
    auto* previous=window.findChild<QPushButton*>("annotationPreviousSegment");
    auto* next=window.findChild<QPushButton*>("annotationNextSegment");
    auto* create=window.findChild<QPushButton*>("annotationInspectorCreateChannel");
    QVERIFY(!previous->isEnabled() && next->isEnabled() && create->isEnabled());
    auto* identity=window.findChild<QLabel*>("annotationIdentity");
    QVERIFY(!identity->text().contains(QString::fromStdString(id)));
    const auto view=file->view;
    next->click();
    QVERIFY(previous->isEnabled() && !next->isEnabled());
    QCOMPARE(file->view.time.begin,view.time.begin);
    QCOMPARE(file->view.time.end,view.time.end);
    QCOMPARE(file->view.frequency.lowerHz,view.frequency.lowerHz);
    QCOMPARE(file->view.frequency.upperHz,view.frequency.upperHz);
    window.session().selectMarks({id,other},id);
    window.refresh();
    QVERIFY(!create->isEnabled());
    QVERIFY(identity->text().contains("已选 2"));
    window.session().selectMarks({});
    window.refresh();
    QVERIFY(!create->isEnabled());
    QVERIFY(!window.findChild<QPushButton*>("annotationInspectorLocate")->isEnabled());
  }

  void uxR4_supplementalSourceAndPagedCopy() {
    QTemporaryDir directory;
    const auto base=directory.filePath("0000000");
    const auto read=[](const QString& name) {
      QFile f(QString::fromUtf8(SS_SOURCE_DIR)+"/tests/fixtures/wideband/"+name);
      if(!f.open(QIODevice::ReadOnly))return QByteArray{};
      return f.readAll();
    };
    auto primary=read("torchsig-0000000.sigmf-meta");
    auto extra=QJsonDocument::fromJson(read("torchsig-0000000.json")).object();
    // Ordinary-sized real parser input. Large RFbench validation is explicitly
    // excluded by the user's latest instruction.
    extra["unknown_header_field"]="SUPPLEMENT_HEADER_PRESERVED";
    const auto supplemental=QJsonDocument(extra).toJson();
    for(const auto& suffix:{QString(".sigmf-meta"),QString(".json")}) {
      QFile f(base+suffix);QVERIFY(f.open(QIODevice::WriteOnly));
      const auto bytes=suffix==".json"?supplemental:primary;
      QCOMPARE(f.write(bytes),bytes.size());
    }
    QFile iq(base+".sigmf-data");QVERIFY(iq.open(QIODevice::WriteOnly));
    QVERIFY(iq.resize(3200000));iq.close();
    FileState file;
    file.metadata.id="supplemental-ui";file.metadata.path=(base+".sigmf-data").toStdString();
    file.metadata.sampleRateHz=20e6;file.metadata.centerFrequencyHz=2500e6;
    file.metadata.sampleCount=400000;file.metadata.effectiveBandwidthHz=20e6;
    file.metadata.sampleFormat.componentEncoding=ComponentEncoding::Float32;
    file.metadata.sampleFormat.normalizeIntegerAdc=false;
    const auto imported=importTorchSigLabels(base+".sigmf-meta",file.metadata);
    QVERIFY2(imported.ok(),qPrintable(imported.error));
    QVERIFY(!imported.annotations.empty());
    file.sourceSets={imported.sourceSet};file.marks=imported.annotations;
    const auto sourceBefore=encodeSourceSet(file.sourceSets.front());
    const auto markBefore=encodeAnnotation(file.marks.front());
    for(auto mode:{AnnotationEvidenceWidget::Mode::Inspector,AnnotationEvidenceWidget::Mode::Auxiliary}) {
      AnnotationEvidenceWidget evidence(mode);
      evidence.resize(380,230);evidence.setAnnotation(&file.marks.front(),&file);evidence.show();
      evidence.findChild<QToolButton*>("annotationEvidenceDetailsToggle")->click();
      evidence.findChild<QToolButton*>("annotationOriginalFieldsToggle")->click();
      auto* raw=evidence.findChild<QPlainTextEdit*>("annotationEvidence");
      QVERIFY(raw->isVisible());
      QVERIFY(raw->toPlainText().contains("SUPPLEMENT_HEADER_PRESERVED"));
      evidence.findChild<QPushButton*>("annotationCopyEvidence")->click();
      const auto copied=QApplication::clipboard()->text();
      QVERIFY(copied.contains(QString::fromUtf8(primary)));
      QVERIFY(copied.contains(QString::fromUtf8(supplemental)));
      QVERIFY(copied.contains(QString::fromStdString(imported.sourceSet.supplementalHash)));
      QVERIFY(copied.contains("supplementalOriginalText"));
      QCOMPARE(encodeSourceSet(file.sourceSets.front()),sourceBefore);
      QCOMPARE(encodeAnnotation(file.marks.front()),markBefore);
    }
    // Small synthetic text exercises paging independently of RFbench events.
    auto origin=std::make_shared<OriginEvidence>(*file.marks.front().originEvidence);
    origin->originalFieldsJson="{\"paging\":\""+std::string(140000,'p')+"PAGE_TAIL\"}";
    file.marks.front().originEvidence=origin;
    AnnotationEvidenceWidget evidence;
    evidence.setAnnotation(&file.marks.front(),&file);evidence.show();
    evidence.findChild<QToolButton*>("annotationEvidenceDetailsToggle")->click();
    evidence.findChild<QToolButton*>("annotationOriginalFieldsToggle")->click();
    auto* raw=evidence.findChild<QPlainTextEdit*>("annotationEvidence");
    QVERIFY(raw->toPlainText().size()<=65536);
    auto* search=evidence.findChild<QLineEdit*>("annotationEvidenceSearch");
    search->setText("PAGE_TAIL");QTest::keyClick(search,Qt::Key_Return);
    QVERIFY(raw->toPlainText().contains("PAGE_TAIL"));
    QCOMPARE(raw->textCursor().selectedText(),QString("PAGE_TAIL"));
    evidence.findChild<QPushButton*>("annotationCopyEvidence")->click();
    QVERIFY(QApplication::clipboard()->text().contains(std::string(140000,'p').c_str()));
  }

  void uxR4_auxiliaryDetailsCloseWithHiddenContext() {
    MainWindow window;
    window.session().addDemoFile();
    const auto id=window.session().addMark({{1000,4000},{99.9e6,100.1e6}});
    window.session().selectMarks({id},id);expose(window);
    window.findChild<QPushButton*>("detectionEvidenceMode")->click();
    auto* evidence=window.findChild<QWidget*>("auxiliaryDetectionEvidence");
    evidence->findChild<QToolButton*>("annotationEvidenceDetailsToggle")->click();
    auto* dialog=evidence->findChild<QDialog*>("annotationEvidenceDetailsDialog");
    QVERIFY(dialog->isVisible());
    window.findChild<QPushButton*>("waveMode")->click();
    QVERIFY(!dialog->isVisible());
    window.session().addDemoFile("other",1e6,101e6,.02);window.refresh();
    QVERIFY(!dialog->isVisible());
    window.findChild<QPushButton*>("detectionEvidenceMode")->click();
    QVERIFY(evidence->findChild<QPlainTextEdit*>("auxiliaryAnnotationEvidence")->toPlainText().isEmpty());
  }

  void uxR4_searchBoundaryAndOriginalNewlines() {
    MainWindow window;window.session().addDemoFile();
    auto* file=window.session().activeFile();
    const auto id=window.session().addMark({{1000,4000},{99.9e6,100.1e6}});
    auto* mark=findAnnotation(*file,id);QVERIFY(mark);
    auto origin=std::make_shared<OriginEvidence>(*mark->originEvidence);
    QString payload(140000,QLatin1Char('x'));
    payload.replace(0,13,"PAYLOAD_BEGIN");
    origin->originalFieldsJson=("{\r\n\"payload\":\""+payload+"\"\r\n}").toStdString();
    mark->originEvidence=origin;
    AnnotationEvidenceWidget evidence;
    const auto open=[&] {
      evidence.findChild<QToolButton*>("annotationEvidenceDetailsToggle")->click();
      auto* toggle=evidence.findChild<QToolButton*>("annotationOriginalFieldsToggle");
      if(!toggle->isChecked())toggle->click();
    };
    const auto fullCopy=[&] {
      evidence.findChild<QPushButton*>("annotationCopyEvidence")->click();
      return QApplication::clipboard()->text();
    };
    evidence.setAnnotation(mark,file);evidence.show();open();
    const auto initial=fullCopy();
    const auto payloadBegin=initial.indexOf("PAYLOAD_BEGIN");
    QVERIFY(payloadBegin>0 && payloadBegin<65000);
    const QString needle="BOUNDARY_NEEDLE";
    payload.replace(100,needle.size(),needle);
    payload.replace(65530-payloadBegin,needle.size(),needle);
    auto revised=std::make_shared<OriginEvidence>(*origin);
    revised->originalFieldsJson=("{\r\n\"payload\":\""+payload+"\"\r\n}").toStdString();
    mark->originEvidence=revised;evidence.setAnnotation(mark,file);open();
    auto* search=evidence.findChild<QLineEdit*>("annotationEvidenceSearch");
    auto* raw=evidence.findChild<QPlainTextEdit*>("annotationEvidence");
    const auto verifyAll=[&](const QString& word) {
      const auto original=fullCopy();
      QList<qsizetype> hits;
      for(qsizetype at=original.indexOf(word);at>=0;at=original.indexOf(word,at+word.size()))hits.append(at);
      QVERIFY(hits.size()>=2);
      search->setText(word);
      for(qsizetype i=0;i<=hits.size();++i) {
        QTest::keyClick(search,Qt::Key_Return);
        QCOMPARE(raw->textCursor().selectedText(),word);
        QVERIFY(evidence.findChild<QLabel*>("annotationEvidencePage")->text()
          .startsWith(QString("第 %1/").arg(hits[i%hits.size()]/65536+1)));
      }
      QCOMPARE(fullCopy(),original);
    };
    const auto boundaryCopy=fullCopy();
    QCOMPARE(boundaryCopy.indexOf(needle,boundaryCopy.indexOf(needle)+needle.size()),qsizetype(65530));
    verifyAll(needle);
    for(const auto& text:QStringList{
          "{\r\n\"a\":\"TARGET\",\r\n\"b\":\"TARGET\"\r\n}",
          "{\r\n\"a\":\"TARGET\",\r\"b\":\"TARGET\",\n\"c\":\"TARGET\"\r\n}"}) {
      auto sample=std::make_shared<OriginEvidence>(*origin);
      sample->originalFieldsJson=text.toStdString();mark->originEvidence=sample;
      evidence.setAnnotation(mark,file);open();
      QVERIFY(fullCopy().contains(text));verifyAll("TARGET");
      QVERIFY(fullCopy().contains(text));
    }
  }

  void uxD01_historicalDerivation() {
    QTemporaryDir directory;
    MainWindow window;
    QString issue;
    QVERIFY2(window.createProject(directory.path(), "history", &issue), qPrintable(issue));
    issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    const auto history = historyFor(window);
    window.session().activeFile()->detectionRuns.push_back(history);
    issue = addSource(window, directory.path(), "IQ_B_FS1Msps_FC200MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    QVERIFY(window.session().activeFile()->metadata.id != history.request.sourceIdentity.fileId);
    expose(window);
    window.findChild<DetectionPanel*>("detectionPanel")->refresh();
    focusHistory(window, "ux-history");
    auto* derive = window.findChild<QPushButton*>("deriveWidebandDetection");
    QVERIFY(derive && derive->isVisible() && derive->isEnabled());
    QTest::mouseClick(derive, Qt::LeftButton);
    const auto result = window.findChild<DetectionCard*>("detectionCard")->request();
    QCOMPARE(result.parentRunId, std::optional<std::string>(history.request.runId));
    QVERIFY(result.runId.empty());
    QCOMPARE(result.sourceIdentity.fileId, history.request.sourceIdentity.fileId);
    QCOMPARE(result.profile.fftSize, history.request.profile.fftSize);
    QCOMPARE(result.profile.hopSeconds, history.request.profile.hopSeconds);
    QCOMPARE(result.profile.modelFrames, history.request.profile.modelFrames);
    QCOMPARE(result.profile.modelHopFrames, history.request.profile.modelHopFrames);
    QCOMPARE(result.spectrumMode, history.request.spectrumMode);
    QCOMPARE(result.parameters.scoreThreshold, history.request.parameters.scoreThreshold);
    QCOMPARE(result.parameters.timeStrategy, history.request.parameters.timeStrategy);
    QCOMPARE(result.requestedRange, history.request.requestedRange);
    QCOMPARE(result.rangeOrigin, std::string("custom"));
    QCOMPARE(window.session().activeFile()->detectionRuns.size(), std::size_t{1});
  }

  void uxD03_crossFileCancellation() {
    QTemporaryDir directory;
    MainWindow window;
    QString issue;
    QVERIFY2(window.createProject(directory.path(), "cancellation", &issue), qPrintable(issue));
    issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    auto running = historyFor(window);
    running.lifecycle = DetectionLifecycle::Running;
    running.request.runId = "ux-running-a";
    running.computationPhase = "inference";
    const auto firstId = running.request.sourceIdentity.fileId;
    window.session().activeFile()->detectionRuns.push_back(running);
    issue = addSource(window, directory.path(), "IQ_B_FS1Msps_FC200MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    expose(window);
    window.findChild<QAction*>("widebandDetectionAction")->trigger();
    auto* card = window.findChild<DetectionCard*>("detectionCard");
    auto* cancel = window.findChild<QPushButton*>("cancelDetectionCardRun");
    QVERIFY(card && cancel);
    QVERIFY2(!cancel->isEnabled(), "A card for B must not cancel A's task");
    QSignalSpy cancelled(card, &DetectionCard::cancelRunRequested);
    QTest::mouseClick(cancel, Qt::LeftButton);
    QCOMPARE(cancelled.count(), 0);
    auto* view = window.findChild<QPushButton*>("viewRunningDetection");
    QVERIFY(view && view->isVisible());
    QTest::mouseClick(view, Qt::LeftButton);
    QCOMPARE(window.session().activeFile()->metadata.id, firstId);
    QVERIFY(cancel->isEnabled());
    QTest::mouseClick(cancel, Qt::LeftButton);
    QCOMPARE(cancelled.count(), 1);
    QCOMPARE(cancelled.takeFirst().at(0).toString(), QString("ux-running-a"));
    window.session().activeFile()->detectionRuns.front().lifecycle = DetectionLifecycle::Cancelled;
    window.refresh();
    QVERIFY(!cancel->isEnabled());
    QTest::mouseClick(cancel, Qt::LeftButton);
    QCOMPARE(cancelled.count(), 0);
  }

  void uxD02_viewSourceKeepsDraft() {
    QTemporaryDir directory;
    MainWindow window;
    QString issue;
    QVERIFY2(window.createProject(directory.path(), "source-view", &issue), qPrintable(issue));
    issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    window.session().activeFile()->detectionRuns.push_back(historyFor(window));
    expose(window);
    window.findChild<DetectionPanel*>("detectionPanel")->refresh();
    focusHistory(window, "ux-history");
    window.findChild<QAction*>("widebandDetectionAction")->trigger();
    auto* card = window.findChild<DetectionCard*>("detectionCard");
    auto* score = card->findChild<AdaptiveValueEdit*>("detectionScoreThreshold");
    window.findChild<QScrollArea*>("propScroll")->ensureWidgetVisible(score);
    const auto committed = card->request().parameters.scoreThreshold;
    score->setFocus();
    score->selectAll();
    QTest::keyClicks(score, "0.83");
    auto* list = window.findChild<QListView*>("detectionRunList");
    const auto index = list->currentIndex();
    QVERIFY(index.isValid());
    QSignalSpy focused(window.findChild<DetectionPanel*>("detectionPanel"),
                       &DetectionPanel::runFocused);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                     list->visualRect(index).topLeft() + QPoint(24, 12));
    QTest::keyClick(list, Qt::Key_Return);
    QTRY_VERIFY(!focused.isEmpty());
    QCOMPARE(score->text(), QString("0.83"));
    QVERIFY(score->property("parameterDraft").toBool());
    QCOMPARE(card->request().parameters.scoreThreshold, committed);
    QVERIFY(!card->request().parentRunId);
  }

  void uxD06_visibleRangeEntry() {
    QTemporaryDir directory;
    MainWindow window;
    QString issue;
    QVERIFY2(window.createProject(directory.path(), "visible-range", &issue), qPrintable(issue));
    issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    expose(window);
    auto* card = window.findChild<DetectionCard*>("detectionCard");
    const ViewRange custom{{500, 1000}, {99.9e6, 100.1e6}};
    card->setRequestedRoi(custom);
    const auto score = card->request().parameters.scoreThreshold;
    auto* button = window.findChild<QPushButton*>("widebandDetect");
    QVERIFY(button && button->isVisible() && button->isEnabled());
    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(card->request().rangeOrigin, std::string("visible"));
    QCOMPARE(card->request().requestedRange, window.session().activeFile()->view);
    QCOMPARE(card->request().parameters.scoreThreshold, score);
    QVERIFY(card->isVisible());
    QVERIFY(window.session().activeFile()->detectionRuns.empty());
  }

  void uxA01_roiConfirmationAndEscape() {
    QTemporaryDir directory;
    MainWindow window;
    QString issue;
    QVERIFY2(window.createProject(directory.path(), "roi", &issue), qPrintable(issue));
    issue = addSource(window, directory.path(), "IQ_A_FS1Msps_FC100MHz.dat");
    QVERIFY2(issue.isEmpty(), qPrintable(issue));
    expose(window);
    auto* plot = window.findChild<PlotWidget*>("mainPlot");
    auto* manual = window.findChild<QPushButton*>("manualTool");
    auto* roi = window.findChild<QPushButton*>("detectionRoiTool");
    QVERIFY(plot && manual && roi);
    QTest::mouseClick(manual, Qt::LeftButton);
    QVERIFY(plot->isCreating());
    QTest::mouseClick(roi, Qt::LeftButton);
    QVERIFY(plot->isSelectingDetectionRoi());
    QTest::keyClick(plot, Qt::Key_Escape);
    QVERIFY(!plot->isSelectingDetectionRoi());
    QVERIFY(plot->isCreating());
    const auto count = window.session().activeFile()->marks.size();
    QTest::mouseClick(roi, Qt::LeftButton);
    const auto start = plot->toPixel(2000, 99.8e6).toPoint();
    const auto end = plot->toPixel(8000, 100.2e6).toPoint();
    QTest::mousePress(plot, Qt::LeftButton, Qt::NoModifier, start);
    QMouseEvent move(QEvent::MouseMove, end, plot->mapToGlobal(end),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(plot, &move);
    QTest::mouseRelease(plot, Qt::LeftButton, Qt::NoModifier, end);
    QVERIFY(plot->hasDetectionRoiPreview());
    const auto exact = *plot->detectionRoiPreview();
    QTest::keyClick(plot, Qt::Key_Return);
    auto* card = window.findChild<DetectionCard*>("detectionCard");
    QCOMPARE(card->request().rangeOrigin, std::string("custom"));
    QCOMPARE(card->request().requestedRange, exact);
    QVERIFY(card->isVisible());
    QVERIFY(plot->isCreating());
    QCOMPARE(window.session().activeFile()->marks.size(), count);
    QVERIFY(window.session().activeFile()->detectionRuns.empty());
  }
};

#include "popover_tests.h"
#include "toolbar_ux_tests.h"
#include "round_two_ux_tests.h"
#include "spectral_ux_tests.h"
#include "parameter_ux_tests.h"
#include "inspector_ux_tests.h"
#include "detection_card_ux_tests.h"
#include "task_ux_tests.h"
#include "ux_r5_ui_tests.h"

QTEST_MAIN(UxTests)
#include "ux_tests.moc"
