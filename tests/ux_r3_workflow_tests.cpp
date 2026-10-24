// Qt software/offscreen workflow tests. The small CI16 file below is generated
// test data, not RF evidence, model inference or native monitor acceptance.
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <functional>
#include <utility>
#include <vector>

#include "app/main_window.h"
#include "application/annotation_service.h"
#include "infrastructure/project_store.h"
#include "ui/charts/plot_widget.h"
#include "ui/label_mapping_dialog.h"

using namespace signalstudio;

namespace {
bool writeBytes(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readBytes(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
bool reveal(QWidget* widget) {
  if (!widget || !widget->isVisible()) return false;
  for (auto* parent = widget->parentWidget(); parent; parent = parent->parentWidget())
    if (auto* area = qobject_cast<QScrollArea*>(parent)) area->ensureWidgetVisible(widget);
  QCoreApplication::processEvents();
  return widget->isVisible();
}
void show(MainWindow& window) {
  window.resize(2560, 1440);
  window.show();
  window.refresh();
  QCoreApplication::processEvents();
}

// Drives only explicitly expected dialogs. A bounded watchdog rejects an
// unexpected modal instead of leaving CTest blocked; the test then fails.
class ModalScript {
 public:
  struct Step {
    std::function<bool(QDialog*)> matches;
    std::function<void(QDialog*)> act;
  };
  explicit ModalScript(std::vector<Step> steps) : steps_(std::move(steps)) {
    elapsed_.start();
    timer_.setInterval(10);
    QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] {
      auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
      if (!dialog) return;
      if (elapsed_.elapsed() > 10000) {
        timedOut_ = true;
        dialog->reject();
        return;
      }
      if (qobject_cast<QProgressDialog*>(dialog)) return;
      if (next_ >= steps_.size() || !steps_[next_].matches(dialog)) return;
      const auto act = steps_[next_++].act;
      act(dialog);
    });
    timer_.start();
  }
  bool complete() const { return !timedOut_ && next_ == steps_.size(); }
  bool timedOut() const { return timedOut_; }
  static Step acknowledge(const QString& title) {
    return {[title](QDialog* dialog) {
              return qobject_cast<QMessageBox*>(dialog) && dialog->windowTitle() == title;
            },
            [](QDialog* dialog) {
              auto* message = qobject_cast<QMessageBox*>(dialog);
              if (auto* ok = message->button(QMessageBox::Ok)) ok->click();
            }};
  }
 private:
  QTimer timer_;
  QElapsedTimer elapsed_;
  std::vector<Step> steps_;
  std::size_t next_ = 0;
  bool timedOut_ = false;
};

// Creates a real readable file with an explicitly supplied format/Fs/Fc. No
// binary format is inferred from the .data suffix, and no external fixtures,
// model package or environment variables are needed by this test executable.
bool prepare(MainWindow& window, const QString& root, QString& error) {
  if (!QDir().mkpath(QDir(root).filePath("data")) ||
      !QDir().mkpath(QDir(root).filePath("label"))) return false;
  QByteArray iq(16384 * 4, '\0');
  for (qsizetype sample = 0; sample < 16384; ++sample) {
    iq[sample * 4] = static_cast<char>(sample % 101);
    iq[sample * 4 + 2] = static_cast<char>(sample % 83);
  }
  const auto path = QDir(root).filePath("data/capture.data");
  if (!writeBytes(path, iq) || !window.createProject(root, "live", &error)) return false;
  FileState source;
  source.metadata.name = "capture.data";
  source.metadata.path = path.toStdString();
  source.metadata.demo = false;
  source.metadata.sampleCount = 16384;
  source.metadata.sampleRateHz = 1e6;
  source.metadata.centerFrequencyHz = 100e6;
  source.metadata.effectiveBandwidthHz = 1e6;
  source.metadata.availability.status = LoadStatus::Ready;
  if (!window.addImportedSource(std::move(source), error)) return false;
  auto* file = window.session().activeFile();
  if (!file) return false;
  file->display.psdSize = 256;
  file->display.stftSize = 256;
  // Deliver the automatic no-label discovery before callers create a TXT.
  QCoreApplication::processEvents();
  window.refresh();
  ModalScript unexpectedDiagnostic({});
  return window.saveProject(QDir(root).filePath("live.ssproj")) && !unexpectedDiagnostic.timedOut();
}

struct LiveSnapshot {
  QByteArray state;
  std::uint64_t generation;
  std::string projectId, assetDirectory, fileId, channelId;
  QString title;
  explicit LiveSnapshot(MainWindow& window)
      : state(ProjectStore::stateToken(window.session().project())),
        generation(window.session().projectGeneration()),
        projectId(window.session().project().projectId),
        assetDirectory(window.session().project().assetDirectory),
        fileId(window.session().project().activeFileId),
        channelId(window.session().project().activeChannelId),
        title(window.windowTitle()) {}
  bool sameSession(MainWindow& window) const {
    const auto& project = window.session().project();
    return generation == window.session().projectGeneration() &&
           projectId == project.projectId && assetDirectory == project.assetDirectory &&
           fileId == project.activeFileId && channelId == project.activeChannelId &&
           state == ProjectStore::stateToken(project);
  }
};

bool configureUnknownMapping(QDialog* dialog, bool frequencyUnknown) {
  const auto select = [dialog](const char* name, const QString& text) {
    auto* combo = dialog->findChild<QComboBox*>(name);
    if (!combo) return false;
    const int index = combo->findText(text, Qt::MatchExactly);
    if (index < 0) return false;
    combo->setCurrentIndex(index);
    return true;
  };
  auto* klass = dialog->findChild<QSpinBox*>("labelClassColumn");
  auto* low = dialog->findChild<QSpinBox*>("labelLowerColumn");
  auto* high = dialog->findChild<QSpinBox*>("labelUpperColumn");
  if (!klass || !low || !high) return false;
  klass->setValue(1);
  if (!select("labelDelimiter", "空白") || !select("labelTimeUnit", "未知") ||
      !select("labelFrequencyLayout", frequencyUnknown ? "未知" : "频率下界 / 上界")) return false;
  if (!frequencyUnknown) {
    low->setValue(2); high->setValue(3);
    if (!select("labelFrequencyUnit", "Hz") || !select("labelFrequencyReference", "基带偏移（加一次 Fc）")) return false;
  }
  return true;
}
}  // namespace

class UxR3WorkflowTests : public QObject {
  Q_OBJECT
 private slots:
  void initTestCase() {
    QVERIFY(settings_.isValid());
    QCoreApplication::setOrganizationName("SignalStudioWorkflowTests");
    QCoreApplication::setApplicationName("UX-R3");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings_.path());
    qApp->setProperty("softwareRenderer", true);
    qApp->setProperty("uiStatePersistenceEnabled", false);
  }

  void arbitraryDirectoryCreateAndDuplicateFailure() {
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto parent = directory.filePath("ordinary documents/2026 unrelated folder");
    QVERIFY(QDir().mkpath(parent));
    MainWindow window; show(window); QString error;
    QVERIFY2(window.createProject(parent, "research signal", &error), qPrintable(error));
    const auto path = QDir(parent).filePath("research signal.ssproj");
    QVERIFY(QFileInfo(path).isFile());
    QVERIFY(!QFileInfo::exists(QDir(parent).filePath("research signal")));
    QVERIFY(!QFileInfo::exists(QDir(parent).filePath("project.json")));
    QVERIFY(!window.session().project().projectId.empty());
    QVERIFY(!window.isProjectDirty());
    QVERIFY(window.windowTitle().contains("research signal"));
    QVERIFY(!window.windowTitle().contains('*'));
    const LiveSnapshot before(window); const auto bytes = readBytes(path);
    QVERIFY(!bytes.isEmpty());
    QVERIFY(!window.createProject(parent, "research signal", &error));
    QVERIFY(!error.isEmpty()); QVERIFY(before.sameSession(window));
    QCOMPARE(readBytes(path), bytes);
    QVERIFY(!window.createProject(directory.filePath("missing parent"), "other", &error));
    QVERIFY(before.sameSession(window));
    QVERIFY(window.openProject(path));
    QCOMPARE(window.session().project().projectId, before.projectId);
    QVERIFY(!window.isProjectDirty());
  }

  void titleDirtyTracksCommitButNotDraftOrRefresh() {
    QTemporaryDir directory; MainWindow window; QString error;
    QVERIFY2(prepare(window, directory.path(), error), qPrintable(error)); show(window);
    auto* display = window.findChild<QPushButton*>("contextDisplay");
    QVERIFY(reveal(display)); QTest::mouseClick(display, Qt::LeftButton);
    auto* properties = window.findChild<QPushButton*>("fileProperties");
    QVERIFY(reveal(properties)); QTest::mouseClick(properties, Qt::LeftButton);
    auto* bandwidth = window.findChild<QDoubleSpinBox*>("effectiveBandwidthMHz");
    QVERIFY(reveal(bandwidth));
    auto* editor = bandwidth->findChild<QLineEdit*>(); QVERIFY(editor);
    const auto oldBandwidth = window.session().activeFile()->metadata.effectiveBandwidthHz;
    editor->setFocus(); editor->selectAll(); QTest::keyClicks(editor, "0.75");
    QVERIFY(bandwidth->property("parameterDraft").toBool());
    window.refresh();
    QVERIFY(editor->text().startsWith("0.75"));
    QCOMPARE(window.session().activeFile()->metadata.effectiveBandwidthHz, oldBandwidth);
    QVERIFY(!window.isProjectDirty()); QVERIFY(!window.windowTitle().contains('*'));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(window.session().activeFile()->metadata.effectiveBandwidthHz, 750000.);
    QTRY_VERIFY(window.isProjectDirty()); QTRY_VERIFY(window.windowTitle().contains('*'));
    QVERIFY(window.saveProject(directory.filePath("live.ssproj")));
    QVERIFY(!window.isProjectDirty()); QVERIFY(!window.windowTitle().contains('*'));
    // Escape discards a second draft, retaining the successfully saved value.
    if (!bandwidth->isVisible()) { QVERIFY(reveal(properties)); QTest::mouseClick(properties, Qt::LeftButton); }
    QVERIFY(reveal(bandwidth)); editor->setFocus(); editor->selectAll();
    QTest::keyClicks(editor, "0.50"); QTest::keyClick(editor, Qt::Key_Escape);
    window.refresh();
    QCOMPARE(window.session().activeFile()->metadata.effectiveBandwidthHz, 750000.);
    QVERIFY(!window.isProjectDirty()); QVERIFY(!window.windowTitle().contains('*'));
  }

  void failedIoRetainsCommittedSession_data() {
    QTest::addColumn<QString>("operation");
    QTest::newRow("save-current-obstructed") << QString("save");
    QTest::newRow("save-as-foreign-target") << QString("save-as");
    QTest::newRow("open-corrupt-container") << QString("open");
    QTest::newRow("legacy-conversion-rejected") << QString("legacy");
  }
  void failedIoRetainsCommittedSession() {
    QFETCH(QString, operation);
    QTemporaryDir directory; MainWindow window; QString error;
    QVERIFY2(prepare(window, directory.path(), error), qPrintable(error)); show(window);
    const auto mark = window.session().addMark({{1000, 5000}, {99.9e6, 100.1e6}});
    QVERIFY(!mark.empty()); window.refresh(); QVERIFY(window.isProjectDirty());
    const auto originalPath = directory.filePath("live.ssproj");
    const auto originalBytes = readBytes(originalPath); QVERIFY(!originalBytes.isEmpty());
    const LiveSnapshot before(window);
    const auto target = directory.filePath("foreign.ssproj");
    const QByteArray foreign("user owned bytes; preserve them");
    QVERIFY(writeBytes(target, foreign));
    if (operation == "legacy") {
      const auto legacy = directory.filePath("broken-v6.json");
      QVERIFY(writeBytes(legacy, "{broken"));
      QVERIFY(!window.importLegacyProject(legacy, directory.filePath("converted.ssproj"), &error));
      QVERIFY(!error.isEmpty()); QVERIFY(!QFileInfo::exists(directory.filePath("converted.ssproj")));
    } else {
      ModalScript messages({ModalScript::acknowledge(operation == "open" ? "打开失败" : "保存失败")});
      if (operation == "save") {
        // Portable write failure: the current file's leaf is now a directory.
        // Keep the old container as a test-owned backup; no ACL mutation.
        QVERIFY(QFile::rename(originalPath, originalPath + ".backup"));
        QVERIFY(QDir().mkdir(originalPath));
        auto* save = window.findChild<QAction*>("saveProjectAction"); QVERIFY(save); save->trigger();
        QVERIFY(QFileInfo(originalPath).isDir());
        QCOMPARE(readBytes(originalPath + ".backup"), originalBytes);
      } else if (operation == "save-as") {
        QVERIFY(!window.saveProject(target));
      } else {
        QVERIFY(!window.openProject(target));
      }
      QVERIFY2(messages.complete(), "Expected failure diagnostic was not shown, or a modal timed out");
    }
    QVERIFY(before.sameSession(window));
    QVERIFY(window.isProjectDirty()); QCOMPARE(window.windowTitle(), before.title);
    QCOMPARE(window.session().activeFile()->marks.back().id, mark);
    QCOMPARE(readBytes(target), foreign);
    if (operation != "save") QCOMPARE(readBytes(originalPath), originalBytes);
  }

  void saveAsReopenKeepsOriginalAndMovesSaveDestination() {
    QTemporaryDir directory; MainWindow window; QString error;
    QVERIFY2(prepare(window, directory.path(), error), qPrintable(error)); show(window);
    const auto original = directory.filePath("live.ssproj");
    const auto originalBytes = readBytes(original);
    const auto identity = window.session().project().projectId;
    const auto mark = window.session().addMark({{2000, 6000}, {99.95e6, 100.05e6}});
    QVERIFY(!mark.empty()); window.refresh(); QVERIFY(window.isProjectDirty());
    const auto destinationDir = directory.filePath("another unrelated directory");
    QVERIFY(QDir().mkpath(destinationDir));
    const auto copy = QDir(destinationDir).filePath("saved-copy.ssproj");
    QVERIFY(window.saveProject(copy)); QVERIFY(!window.isProjectDirty());
    QCOMPARE(window.session().project().projectId, identity);
    QCOMPARE(readBytes(original), originalBytes);
    auto* liveMark = findAnnotation(*window.session().activeFile(), mark); QVERIFY(liveMark);
    const auto edit = window.session().annotations().rename(*window.session().activeFile(), mark, "Saved through current destination", {});
    QVERIFY2(edit.ok, edit.error.c_str()); window.refresh(); QVERIFY(window.isProjectDirty());
    auto* save = window.findChild<QAction*>("saveProjectAction"); QVERIFY(save); save->trigger();
    QVERIFY(!window.isProjectDirty()); QCOMPARE(readBytes(original), originalBytes);
    QVERIFY(window.openProject(original));
    QTRY_VERIFY_WITH_TIMEOUT(window.session().activeFile() &&
        window.session().activeFile()->metadata.availability.status == LoadStatus::Ready, 10000);
    // Background hydration alone must not turn a successfully opened project dirty.
    QVERIFY2(!window.isProjectDirty(), "Source reload changed the dirty baseline without a committed user edit");
    QVERIFY(!window.windowTitle().contains('*'));
    QVERIFY(window.session().activeFile()->marks.empty());
    QVERIFY(window.openProject(copy));
    QTRY_VERIFY_WITH_TIMEOUT(window.session().activeFile() &&
        window.session().activeFile()->metadata.availability.status == LoadStatus::Ready, 10000);
    liveMark = findAnnotation(*window.session().activeFile(), mark); QVERIFY(liveMark);
    QCOMPARE(liveMark->name, std::string("Saved through current destination"));
    QCOMPARE(window.session().project().projectId, identity);
    QVERIFY(!window.isProjectDirty());
  }

  void relocateMissingSourceRejectsSameLengthWrongContent() {
    QTemporaryDir directory; MainWindow window; QString error;
    QVERIFY2(prepare(window, directory.path(), error), qPrintable(error)); show(window);
    const auto mark = window.session().addMark({{1200, 7000}, {99.9e6, 100.1e6}});
    QVERIFY(!mark.empty()); window.refresh();
    const auto projectPath = directory.filePath("live.ssproj");
    QVERIFY(window.saveProject(projectPath));
    const auto fileId = QString::fromStdString(window.session().activeFile()->metadata.id);
    const auto source = QString::fromStdString(window.session().activeFile()->metadata.path);
    const auto bytes = readBytes(source); QVERIFY(!bytes.isEmpty());
    const auto moved = directory.filePath("relocated.data");
    QVERIFY(QFile::rename(source, moved));
    QVERIFY(window.openProject(projectPath));
    QCOMPARE(window.session().activeFile()->metadata.availability.status, LoadStatus::Failed);
    const auto wrong = directory.filePath("wrong-same-length.data");
    auto altered = bytes; altered[altered.size() / 2] = static_cast<char>(altered[altered.size() / 2] ^ 0x5a);
    QVERIFY(writeBytes(wrong, altered)); QCOMPARE(QFileInfo(wrong).size(), QFileInfo(moved).size());
    const LiveSnapshot before(window);
    const auto geometry = window.session().activeFile()->marks.front().range;
    QVERIFY(!window.relocateSource(fileId, wrong, &error)); QVERIFY(!error.isEmpty());
    QVERIFY(before.sameSession(window));
    QCOMPARE(QString::fromStdString(window.session().activeFile()->metadata.path), source);
    QVERIFY2(window.relocateSource(fileId, moved, &error), qPrintable(error));
    QCOMPARE(window.session().projectGeneration(), before.generation);
    QTRY_COMPARE_WITH_TIMEOUT(window.session().activeFile()->metadata.availability.status, LoadStatus::Ready, 10000);
    QCOMPARE(availableSamples(window.session().activeFile()->metadata), SampleIndex{16384});
    QCOMPARE(window.session().activeFile()->marks.front().id, mark);
    QCOMPARE(window.session().activeFile()->marks.front().range, geometry);
    QCOMPARE(QString::fromStdString(window.session().activeFile()->metadata.path), moved);
    QVERIFY(window.isProjectDirty()); QVERIFY(window.saveProject(projectPath));
    QVERIFY(window.openProject(projectPath));
    QTRY_VERIFY_WITH_TIMEOUT(window.session().activeFile() &&
        window.session().activeFile()->metadata.availability.status == LoadStatus::Ready, 10000);
    QCOMPARE(QString::fromStdString(window.session().activeFile()->metadata.path), moved);
    QVERIFY(!window.isProjectDirty());
    QCOMPARE(readBytes(moved), bytes);
  }

  void mappingUnknownAndCancel_data() {
    QTest::addColumn<QString>("decision");
    QTest::newRow("cancel-mapping") << QString("cancel-mapping");
    QTest::newRow("cancel-preflight-preview") << QString("cancel-preview");
    QTest::newRow("unknown-time-is-observation") << QString("unknown-time");
    QTest::newRow("unknown-frequency-is-original-evidence") << QString("unknown-frequency");
  }
  void mappingUnknownAndCancel() {
    QFETCH(QString, decision);
    QTemporaryDir directory; MainWindow window; QString error;
    QVERIFY2(prepare(window, directory.path(), error), qPrintable(error)); show(window);
    const auto manual = window.session().addMark({{1000, 4000}, {99.92e6, 100.08e6}});
    QVERIFY(!manual.empty()); window.refresh(); QVERIFY(window.saveProject(directory.filePath("live.ssproj")));
    const LiveSnapshot before(window);
    const auto sourceCount = window.session().activeFile()->sourceSets.size();
    const QByteArray text("QPSK -100000 100000\n");
    const auto labels = directory.filePath("label/capture.txt"); QVERIFY(writeBytes(labels, text));
    bool unspecifiedRejected = false, configured = false, previewSeen = false;
    std::vector<ModalScript::Step> steps;
    steps.push_back({[](QDialog* dialog) { return dialog->objectName() == "labelMappingDialog"; },
      [&](QDialog* dialog) {
        auto* mapping = dynamic_cast<LabelMappingDialog*>(dialog);
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (!mapping || !buttons) { dialog->reject(); return; }
        if (decision == "cancel-mapping") {
          // A cancelled, partially configured dialog must publish nothing.
          configureUnknownMapping(dialog, false);
          QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel), Qt::LeftButton);
          return;
        }
        QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
        auto* diagnostic = dialog->findChild<QLabel*>("labelMappingError");
        unspecifiedRejected = dialog->isVisible() && !mapping->mapping() && diagnostic && !diagnostic->text().isEmpty();
        configured = configureUnknownMapping(dialog, decision == "unknown-frequency");
        if (!configured) { dialog->reject(); return; }
        QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
      }});
    if (decision != "cancel-mapping") steps.push_back({
      [](QDialog* dialog) { return qobject_cast<QMessageBox*>(dialog) && dialog->windowTitle() == "标签导入预览"; },
      [&](QDialog* dialog) {
        previewSeen = true;
        auto* message = qobject_cast<QMessageBox*>(dialog);
        auto* button = message->button(decision == "cancel-preview" ? QMessageBox::Cancel : QMessageBox::Yes);
        if (button) QTest::mouseClick(button, Qt::LeftButton);
      }});
    ModalScript script(std::move(steps));
    window.discoverLabels();
    QVERIFY2(script.complete(), "Expected mapping/preview dialog missing, or an unexpected modal blocked the workflow");
    QCOMPARE(window.session().projectGeneration(), before.generation);
    QCOMPARE(readBytes(labels), text);
    const auto* file = window.session().activeFile();
    QVERIFY(findAnnotation(*file, manual));
    if (decision.startsWith("cancel")) {
      QVERIFY(before.sameSession(window)); QVERIFY(!window.isProjectDirty());
      QCOMPARE(file->marks.size(), std::size_t{1}); QCOMPARE(file->sourceSets.size(), sourceCount);
    } else {
      QVERIFY(unspecifiedRejected && configured && previewSeen);
      QVERIFY(window.isProjectDirty()); QCOMPARE(file->sourceSets.size(), sourceCount + 1);
      const auto& source = file->sourceSets.back(); QCOMPARE(source.originalText, text.toStdString());
      if (decision == "unknown-frequency") {
        QCOMPARE(file->marks.size(), std::size_t{1}); // Class/raw evidence only; no invented rectangle.
      } else {
        QCOMPARE(file->marks.size(), std::size_t{2});
        const auto& imported = file->marks.back();
        QCOMPARE(imported.sourceKind, AnnotationSource::ImportedLabel);
        QVERIFY(imported.segments.empty()); QVERIFY(imported.activeSegmentId.empty());
        QVERIFY(imported.observation.has_value());
        QCOMPARE(imported.observationTimeQuality, TimeQuality::Unknown);
        QVERIFY(imported.originEvidence && imported.originEvidence->originalSegments.empty());
        QVERIFY(imported.originEvidence->originalObservation.has_value());
        QVERIFY(!segmentCanCreateChannel(imported, {}));
        QCOMPARE(imported.observation->frequency.lowerHz, 99.9e6);
        QCOMPARE(imported.observation->frequency.upperHz, 100.1e6);
      }
    }
    if (decision == "cancel-preview") QVERIFY(unspecifiedRejected && configured && previewSeen);
  }

  void noLabelsStillAllowsManualPlotCreation() {
    QTemporaryDir directory; MainWindow window; QString error;
    QVERIFY2(prepare(window, directory.path(), error), qPrintable(error)); show(window);
    QVERIFY(!QFileInfo::exists(directory.filePath("label/capture.txt")));
    const LiveSnapshot before(window);
    ModalScript noPrompt({}); window.discoverLabels();
    QVERIFY(!noPrompt.timedOut()); QVERIFY(before.sameSession(window));
    auto* tool = window.findChild<QPushButton*>("manualTool");
    auto* plot = window.findChild<PlotWidget*>("mainPlot");
    QVERIFY(reveal(tool)); QVERIFY(plot && plot->isVisible());
    QTest::mouseClick(tool, Qt::LeftButton); QVERIFY(plot->isCreating());
    const auto area = plot->plotRect();
    const QPoint start = (area.topLeft() + QPointF(area.width() * .2, area.height() * .2)).toPoint();
    const QPoint end = (area.topLeft() + QPointF(area.width() * .7, area.height() * .7)).toPoint();
    QTest::mousePress(plot, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(plot, end);
    QTest::mouseRelease(plot, Qt::LeftButton, Qt::NoModifier, end);
    QTRY_COMPARE(window.session().activeFile()->marks.size(), std::size_t{1});
    const auto& mark = window.session().activeFile()->marks.front();
    QCOMPARE(mark.sourceKind, AnnotationSource::Manual); QCOMPARE(mark.segments.size(), std::size_t{1});
    QVERIFY(rangeInside(mark.segments.front().range, fullRange(window.session().activeFile()->metadata)));
    QVERIFY(mark.segments.front().range.time.end > mark.segments.front().range.time.begin);
    QCOMPARE(mark.segments.front().timeQuality, TimeQuality::Manual);
    QVERIFY(window.isProjectDirty()); QTRY_VERIFY(window.windowTitle().contains('*'));
  }
 private:
  QTemporaryDir settings_;
};

QTEST_MAIN(UxR3WorkflowTests)
#include "ux_r3_workflow_tests.moc"
