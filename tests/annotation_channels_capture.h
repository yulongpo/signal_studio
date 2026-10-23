#pragma once

// Real IQ/DDC with explicit synthetic annotations. No detector runs here.
QDialog* channelsDialog(MainWindow& window, QScreen* screen) {
  QDialog* dialog = nullptr;
  require(waitUntil([&] {
            for (auto* candidate : window.findChildren<QDialog*>())
              if (candidate->objectName() == "channelConfigDialog" &&
                  candidate->isVisible())
                dialog = candidate;
            return dialog && dialog->windowHandle();
          }),
          "The unified channel dialog did not open");
  dialog->windowHandle()->setScreen(screen);
  dialog->move(screen->geometry().center() - dialog->rect().center());
  require(waitUntil([&] {
            return dialog->screen() == screen &&
                   std::abs(dialog->devicePixelRatioF() - 1.5) < .001 &&
                   screen->geometry().contains(dialog->frameGeometry());
          }),
          "The channel form left the selected native 4K / 150% monitor");
  require(!dialog->findChild<QCheckBox*>("channelConfirmedSegment"),
          "The obsolete activity-confirmation gate is still present");
  return dialog;
}

void channelsContext(MainWindow& window, QWidget& widget, QPoint point,
                     const char* actionName) {
  require(widget.isVisible() && widebandVisibleRect(&widget, window)
                                    .contains(widget.mapTo(&window, point)),
          "The annotation context target is outside its actual visible area");
  bool handled = false;
  QString failure;
  QTimer::singleShot(100, &window, [&] {
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    try {
      require(menu, "The actual annotation context menu did not open");
      QAction* action = nullptr;
      for (auto* candidate : menu->actions())
        if (candidate->objectName() == actionName) action = candidate;
      require(action && action->isEnabled(),
              "The context menu lacks the unified enabled channel action");
      QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                        menu->actionGeometry(action).center());
    } catch (const std::exception& error) {
      failure = QString::fromUtf8(error.what());
      if (menu) menu->close();
    }
    handled = true;
  });
  // QtTest's synthesized mouse buttons do not produce a context-menu event.
  // Send the same public Qt mouse-context event to the visible widget, then
  // select the real popup action rather than calling a business slot.
  QContextMenuEvent event(QContextMenuEvent::Mouse, point,
                          widget.mapToGlobal(point));
  QCoreApplication::sendEvent(&widget, &event);
  require(waitUntil([&] { return handled; }) && failure.isEmpty(), failure);
}

void channelsSave(MainWindow& window, const QString& path, const QDir& output) {
  // Diagnose a malformed candidate before MainWindow's user-facing error
  // message box can block an unattended native capture. The real target is
  // still saved through MainWindow, including its first-upgrade transaction.
  QTemporaryDir preflight(output.filePath("save-preflight-XXXXXX"));
  require(preflight.isValid(), "Cannot create the save preflight directory");
  QString error;
  require(ProjectStore::save(preflight.filePath("candidate.json"),
                             window.session().project(), error),
          error);
  require(window.saveProject(path), "Saving the native channel project failed");
}

QJsonObject channelsDdc(MainWindow& window, const Channel& channel,
                        bool tail = false) {
  const auto metadata = window.session().activeFile()->metadata;
  ChannelDspPlan plan;
  ChannelSampleData data;
  QString error;
  require(makeChannelDspPlan(metadata, channel, plan, error), error);
  const auto count = static_cast<SampleIndex>(
      (channel.sourceTime.end - channel.sourceTime.begin) *
      static_cast<long double>(channel.outputSampleRateHz) /
      metadata.sampleRateHz);
  const auto length = std::min<SampleIndex>(8192, count);
  const TimeRange output =
      tail ? TimeRange{count - length, count} : TimeRange{0, length};
  require(
      length && processChannelSamples(metadata, channel, plan, output, data),
      "CPU DDC did not read the actual representative IQ");
  require(data.outputSamples == output && data.samples.size() == length &&
              data.sourceSamples.begin >= channel.sourceTime.begin &&
              data.sourceSamples.end <= channel.sourceTime.end &&
              data.sourceSamples.end <= availableSamples(metadata),
          "DDC output/support is outside the extraction range or prefix");
  double energy = 0;
  for (const auto value : data.samples) {
    require(std::isfinite(value.real()) && std::isfinite(value.imag()),
            "Actual DDC output contains a non-finite sample");
    energy += std::norm(value);
  }
  require(energy > 0, "Actual DDC output has no signal energy");
  const QByteArray bytes(reinterpret_cast<const char*>(data.samples.data()),
                         static_cast<qsizetype>(data.samples.size() *
                                                sizeof(std::complex<float>)));
  const auto& snapshot = *channel.sourceSnapshot;
  return {{"channelId", QString::fromStdString(channel.id)},
          {"annotationId", QString::fromStdString(snapshot.annotationId)},
          {"segmentId", QString::fromStdString(snapshot.segmentId)},
          {"bindingKind", snapshot.bindingKind == ChannelBindingKind::UserRange
                              ? "UserRange"
                              : "LocatedSegment"},
          {"sourceBegin", QString::number(channel.sourceTime.begin)},
          {"sourceEnd", QString::number(channel.sourceTime.end)},
          {"availableSamples", QString::number(availableSamples(metadata))},
          {"outputBegin", QString::number(output.begin)},
          {"outputEnd", QString::number(output.end)},
          {"outputSamples", static_cast<qint64>(data.samples.size())},
          {"firstSourceSample",
           static_cast<double>(
               channel.sourceTime.begin +
               output.begin * static_cast<long double>(metadata.sampleRateHz) /
                   channel.outputSampleRateHz)},
          {"leftPadded", data.leftPadded},
          {"rightPadded", data.rightPadded},
          {"energy", energy},
          {"outputSha256",
           QString::fromLatin1(
               QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                   .toHex())},
          {"realIq", true},
          {"cpuDdcVerified", true},
          {"modelInferenceVerified", false}};
}

Channel channelsConfirm(MainWindow& window, QDialog& dialog, bool autoOpen) {
  const auto before = window.session().activeFile()->channels.size();
  auto* automatic = child<QCheckBox>(dialog, "channelAutoOpen");
  if (automatic->isChecked() != autoOpen) widebandNativeClick(automatic);
  require(automatic->isChecked() == autoOpen,
          "Native channel auto-open indicator did not apply its requested state");
  auto* confirm = child<QPushButton>(dialog, "confirmChannelConfig");
  require(waitUntil([&] { return confirm->isEnabled(); }),
          child<QLabel>(dialog, "channelValidation")->text());
  widebandNativeClick(confirm);
  require(waitUntil([&] {
            return window.session().activeFile()->channels.size() == before + 1;
          }),
          "The unified channel request was not committed");
  return window.session().activeFile()->channels.back();
}

void channelsCopyLegacyShards(const QString& project, const QDir& target) {
  QFile input(project);
  require(input.open(QIODevice::ReadOnly), "Cannot read the v5 fixture index");
  const auto files =
      QJsonDocument::fromJson(input.readAll()).object()["files"].toArray();
  const QDir source = QFileInfo(project).absoluteDir();
  for (const auto& file : files) {
    const auto store = file.toObject()["annotationStore"].toObject();
    for (const auto* key : {"sourceSets", "shards"}) {
      for (const auto& reference : store[key].toArray()) {
        const auto ref = reference.toObject();
        const auto relative = ref["path"].toString();
        require(relative.startsWith("annotations/") && !relative.contains(".."),
                "The frozen v5 fixture contains an unsafe sidecar path");
        QFile sidecar(source.filePath(relative));
        require(sidecar.open(QIODevice::ReadOnly),
                "A frozen v5 source/annotation sidecar is missing");
        const auto bytes = sidecar.readAll();
        require(
            QString::number(bytes.size()) == ref["bytes"].toString() &&
                QString::fromLatin1(
                    QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                        .toHex()) == ref["sha256"].toString(),
            "A frozen v5 sidecar differs from its original index");
        const auto path = target.filePath(relative);
        QFile existing(path);
        if (existing.exists()) {
          require(
              existing.open(QIODevice::ReadOnly) && existing.readAll() == bytes,
              "An existing v5 sidecar has different bytes");
        } else {
          require(
              target.mkpath(QFileInfo(path).absolutePath()) &&
                  QFile::copy(sidecar.fileName(), path),
              "Cannot copy an original v5 sidecar into the upgrade project");
        }
      }
    }
  }
}

void interactionChannelScenes(Capture& capture, QScreen* screen) {
  auto& window = capture.window;
  auto& session = window.session();
  const auto primary = capture.output.filePath("u3-channel-project.json");
  channelsSave(window, primary, capture.output);

  // Upgrade an untouched U2 v5 snapshot; the raw backup must be byte exact.
  const auto legacyFixture = QDir(QCoreApplication::applicationDirPath())
                                 .filePath(
                                     "../../docs/implementation/"
                                     "annotation-interactions/U3/fixtures/"
                                     "v5-project/project.json");
  QFile original(legacyFixture);
  require(original.open(QIODevice::ReadOnly),
          "Cannot read the frozen v5 fixture");
  const auto legacyBytes = original.readAll();
  require(QJsonDocument::fromJson(legacyBytes).object()["version"].toInt() == 5,
          "The upgrade fixture is not the committed native v5 project");
  const auto legacyPath = capture.output.filePath("legacy-v5-project.json");
  require(QFile::copy(legacyFixture, legacyPath),
          "The native upgrade fixture already exists or cannot be copied");
  channelsCopyLegacyShards(legacyFixture, capture.output);
  // The frozen project refers to the formerly available W: source. Migrate
  // its exact bytes through the real store without rewriting that identity
  // or claiming its old channel has executed DDC on the relocated source.
  Project legacy;
  QString error;
  require(ProjectStore::load(legacyPath, legacy, error), error);
  const auto legacySnapshot =
      *legacy.files.front().channels.front().sourceSnapshot;
  const auto legacyTime = legacy.files.front().channels.front().sourceTime;
  require(ProjectStore::save(legacyPath, legacy, error), error);
  const auto backupNames = capture.output.entryList(
      {QFileInfo(legacyPath).fileName() + ".pre-v6-v5-*.bak*"}, QDir::Files);
  require(backupNames.size() == 1, "The first upgrade lacks one unique backup");
  QFile backup(capture.output.filePath(backupNames.front()));
  require(backup.open(QIODevice::ReadOnly) && backup.readAll() == legacyBytes,
          "The upgrade backup does not preserve the original v5 bytes");
  require(ProjectStore::save(legacyPath, legacy, error), error);
  Project reopenedLegacy;
  require(ProjectStore::load(legacyPath, reopenedLegacy, error), error);
  require(capture.output.entryList(
              {QFileInfo(legacyPath).fileName() + ".pre-v6-v5-*.bak*"},
              QDir::Files) == backupNames &&
              *reopenedLegacy.files.front().channels.front().sourceSnapshot ==
                  legacySnapshot &&
              reopenedLegacy.files.front().channels.front().sourceTime ==
                  legacyTime,
          "Repeat save or migration changed the old channel snapshot");
  editingScene(capture, "01-v5-upgrade-preserves-backup-and-channel", screen);
  auto migrationScene = capture.scenes.last().toObject();
  migrationScene["migrationEntry"] =
      "ProjectStore load/save of frozen v5 bytes";
  migrationScene["legacyChannelDdcVerified"] = false;
  migrationScene["legacySourceIdentityRewritten"] = false;
  capture.scenes[capture.scenes.size() - 1] = migrationScene;
  require(window.openProject(primary) && widebandDisplayReady(window),
          "The primary U3 actual IQ project did not reopen");

  std::map<std::string, OriginEvidence> origins;
  for (const auto& mark : session.activeFile()->marks)
    origins.emplace(mark.id, *mark.originEvidence);
  editingSelect(window, "algo-located", "algo-located-s2");
  const auto locatedBefore = editingMark(window, "algo-located");
  require(locatedBefore.reviewStatus == ReviewStatus::Pending &&
              !locatedBefore.segments.back().confirmed &&
              child<QPushButton>(window, "extract")->isEnabled(),
          "Pending located activity still requires acceptance/confirmation");
  widebandChooseQuality(window, "clearAnnotationReview", &capture, screen,
                        "02-optional-quality-menu");

  auto* table = child<QTableView>(window, "annotationTable");
  const auto cell = interactionTableIndex(*table, "algo-located");
  table->scrollTo(cell);
  channelsContext(window, *table->viewport(), table->visualRect(cell).center(),
                  "annotationTableCreateChannelAction");
  auto* dialog = channelsDialog(window, screen);
  editingType(*child<QLineEdit>(*dialog, "channelRangeStartMilliseconds"),
              "11");
  QTest::keyClick(child<QLineEdit>(*dialog, "channelRangeStartMilliseconds"),
                  Qt::Key_Tab);
  editingType(*child<QLineEdit>(*dialog, "channelRangeEndMilliseconds"), "13");
  QTest::keyClick(child<QLineEdit>(*dialog, "channelRangeEndMilliseconds"),
                  Qt::Key_Tab);
  editingScene(capture, "03-pending-located-channel-trim-form", screen, dialog);
  const auto located = channelsConfirm(window, *dialog, false);
  require(located.sourceSnapshot->bindingKind ==
                  ChannelBindingKind::LocatedSegment &&
              located.sourceSnapshot->boundRange ==
                  locatedBefore.segments.back().range &&
              located.sourceTime == TimeRange{220000, 260000} &&
              located.sourceRelation == ChannelSourceRelation::Current &&
              editingMark(window, "algo-located").reviewStatus ==
                  ReviewStatus::Pending,
          "Trimmed extraction lost its complete binding or changed quality");
  QJsonArray ddcRuns;
  ddcRuns.append(channelsDdc(window, located));
  editingScene(capture, "04-pending-located-real-cpu-ddc", screen);

  editingSelect(window, "algo-observation");
  widebandChooseQuality(window, "rejectAnnotations");
  const auto unknownBefore = editingMark(window, "algo-observation");
  auto view = fullRange(session.activeFile()->metadata);
  view.time = {80000, 160000};
  session.setView(view);
  window.refresh();
  auto* tree = child<QTreeWidget>(window, "projectTree");
  require(waitUntil([&] {
            return interactionTreeItem(*tree, "algo-observation") != nullptr;
          }),
          "The current unknown-time source is missing from the left tree");
  auto* item = interactionTreeItem(*tree, "algo-observation");
  tree->scrollToItem(item);
  auto* resources = child<QScrollArea>(window, "resourceScroll");
  const auto itemCenter = tree->viewport()->mapTo(
      resources->widget(), tree->visualItemRect(item).center());
  resources->ensureVisible(itemCenter.x(), itemCenter.y(), 12, 12);
  require(widebandVisibleRect(tree->viewport(), window)
              .contains(tree->viewport()->mapTo(
                  &window, tree->visualItemRect(item).center())),
          "The actual source item is clipped in the left tree");
  channelsContext(window, *tree->viewport(),
                  tree->visualItemRect(item).center(),
                  "annotationTreeCreateChannelAction");
  dialog = channelsDialog(window, screen);
  require(child<QLineEdit>(*dialog, "channelRangeStartMilliseconds")
                      ->text()
                      .toDouble() == 4 &&
              child<QLineEdit>(*dialog, "channelRangeEndMilliseconds")
                      ->text()
                      .toDouble() == 8,
          "UserRange did not default to visible time intersected with prefix");
  editingScene(capture, "05-ignored-unknown-user-range-form", screen, dialog);
  const auto unknown = channelsConfirm(window, *dialog, true);
  require(
      unknown.sourceSnapshot->bindingKind == ChannelBindingKind::UserRange &&
          unknown.sourceSnapshot->segmentId.empty() &&
          unknown.sourceTime == view.time &&
          editingMark(window, "algo-observation").segments.empty() &&
          editingMark(window, "algo-observation").observation ==
              unknownBefore.observation &&
          editingMark(window, "algo-observation").reviewStatus ==
              ReviewStatus::Rejected,
      "Manual extraction invented activity or altered ignored quality");
  ddcRuns.append(channelsDdc(window, unknown));
  auto* narrow = window.findChild<NarrowbandWorkspace*>();
  require(narrow && waitUntil(
                        [&] {
                          return narrow->isVisible() &&
                                 narrow->visibleChartsSettled() &&
                                 narrow->visibleChartsGpuReady() &&
                                 narrow->visibleTextureUploads() > 0 &&
                                 narrow->dataStatusText().contains("真实 DDC");
                        },
                        60000),
          "The UserRange channel did not display actual CPU DDC data");
  editingScene(capture, "06-unknown-user-range-real-ddc-narrowband", screen);
  auto narrowScene = capture.scenes.last().toObject();
  narrowScene["narrowRendering"] = narrow->renderStatistics();
  capture.scenes[capture.scenes.size() - 1] = narrowScene;
  click(window, "narrowbandClose");

  const auto completeMetadata = session.activeFile()->metadata;
  session.activeFile()->metadata.availability.status = LoadStatus::Partial;
  session.activeFile()->metadata.availability.availableSamples = 200000;
  // Controlled availability fixture: the physical file remains complete. An
  // unread view can arise while restoring/replacing the available prefix.
  session.activeFile()->view = fullRange(completeMetadata);
  session.activeFile()->view.time = {240000, 320000};
  window.refresh();
  widebandShowInspector(window, "contextAnnotation");
  widebandScrollTo(child<QPushButton>(window, "extract"));
  widebandNativeClick(child<QPushButton>(window, "extract"));
  dialog = channelsDialog(window, screen);
  auto* confirm = child<QPushButton>(*dialog, "confirmChannelConfig");
  require(!confirm->isEnabled() &&
              !child<QLabel>(*dialog, "channelValidation")->text().isEmpty(),
          "An empty visible/prefix intersection silently gained samples");
  editingScene(capture, "07-empty-visible-range-requires-explicit-prefix",
               screen, dialog, false);
  click(*dialog, "channelUseAvailablePrefix");
  require(
      waitUntil([&] { return confirm->isEnabled(); }) &&
          child<QLineEdit>(*dialog, "channelRangeStartMilliseconds")
                  ->text()
                  .toDouble() == 0 &&
          child<QLineEdit>(*dialog, "channelRangeEndMilliseconds")
                  ->text()
                  .toDouble() == 10,
      "The explicit prefix action did not select the actual 0-10 ms prefix");
  const auto partial = channelsConfirm(window, *dialog, false);
  require(
      partial.sourceTime == TimeRange{0, 200000} &&
          partial.sourceSnapshot->bindingKind == ChannelBindingKind::UserRange,
      "Partial UserRange escaped its available prefix");
  ddcRuns.append(channelsDdc(window, partial, true));
  view = fullRange(completeMetadata);
  view.time = {0, 200000};
  session.setView(view);
  window.refresh();
  editingScene(capture, "08-partial-prefix-tail-real-cpu-ddc", screen);

  session.activeFile()->metadata = completeMetadata;
  session.setView(fullRange(completeMetadata));
  window.refresh();
  session.activeFile()->metadata.centerFrequencyHz += 1000;
  window.refresh();
  auto* plot = capture.plots[2];
  const auto observation = *editingMark(window, "algo-observation").observation;
  const auto point = plot->toPixel(
      (observation.time.begin + observation.time.end) / 2,
      (observation.frequency.lowerHz + observation.frequency.upperHz) / 2);
  channelsContext(window, *plot, point.toPoint(), "contextCreateChannelAction");
  dialog = channelsDialog(window, screen);
  const auto reason = child<QLabel>(*dialog, "channelValidation")->text();
  require(!child<QPushButton>(*dialog, "confirmChannelConfig")->isEnabled() &&
              (reason.contains("源") ||
               reason.contains("source", Qt::CaseInsensitive)),
          "A changed physical source lacks a specific source diagnostic");
  editingScene(capture, "09-physical-source-diagnostic", screen, dialog);
  dialog->reject();
  session.activeFile()->metadata = completeMetadata;
  window.refresh();

  require(session.annotations()
              .select(*session.activeFile(), {"manual-1", "algo-observation"},
                      "algo-observation")
              .ok,
          "Cannot establish the multiple-source selection fixture");
  window.refresh();
  widebandShowInspector(window, "contextAnnotation");
  widebandScrollTo(child<QPushButton>(window, "extract"));
  widebandNativeClick(child<QPushButton>(window, "extract"));
  dialog = channelsDialog(window, screen);
  auto* source = child<QComboBox>(*dialog, "channelSourceMark");
  require(source->currentData().toString().isEmpty() &&
              !child<QPushButton>(*dialog, "confirmChannelConfig")->isEnabled(),
          "A multi-selection silently picked an active or last source");
  const auto manualIndex = source->findData("manual-1");
  require(manualIndex >= 0,
          "The explicitly selectable manual source is absent");
  widebandNativeSelect(source, manualIndex);
  require(
      waitUntil([&] {
        return child<QPushButton>(*dialog, "confirmChannelConfig")->isEnabled();
      }),
      "Choosing one source did not resolve the multi-selection");
  editingScene(capture, "10-multiple-selection-explicit-one-source", screen,
               dialog);
  dialog->reject();

  const auto locatedChannel = [&]() -> const Channel& {
    const auto& channels = session.activeFile()->channels;
    const auto found =
        std::find_if(channels.begin(), channels.end(),
                     [&](const auto& value) { return value.id == located.id; });
    require(found != channels.end(), "The located channel was lost");
    return *found;
  };
  editingSelect(window, "algo-located", "algo-located-s2");
  require(session.annotations()
              .rename(*session.activeFile(), "algo-located",
                      "Channel source revised name", "Review note")
              .ok,
          "Source metadata edit failed");
  window.refresh();
  widebandChooseQuality(window, "acceptAnnotations");
  auto sibling = editingMark(window, "algo-located").segments.front().range;
  sibling.time.end -= 100;
  require(session.annotations()
                  .commitGeometry(*session.activeFile(), "algo-located",
                                  "algo-located-s1", sibling)
                  .ok &&
              locatedChannel().sourceRelation == ChannelSourceRelation::Current,
          "Metadata, quality or an unrelated segment invalidated the channel");
  auto bound = editingMark(window, "algo-located").segments.back().range;
  bound.time.end -= 100;
  require(
      session.annotations()
              .commitGeometry(*session.activeFile(), "algo-located",
                              "algo-located-s2", bound)
              .ok &&
          locatedChannel().sourceRelation == ChannelSourceRelation::Changed &&
          *locatedChannel().sourceSnapshot == *located.sourceSnapshot,
      "Binding geometry did not report change with snapshot retained");
  require(session.annotations().undo(*session.activeFile()).ok &&
              locatedChannel().sourceRelation == ChannelSourceRelation::Current,
          "Undo did not restore the bound range relation");
  require(
      session.annotations()
              .deleteBatch(*session.activeFile(), {"algo-located"})
              .ok &&
          locatedChannel().sourceRelation == ChannelSourceRelation::Missing &&
          session.annotations().undo(*session.activeFile()).ok &&
          locatedChannel().sourceRelation == ChannelSourceRelation::Current,
      "Source deletion/undo did not preserve and restore the channel link");
  window.refresh();
  editingScene(capture, "11-bound-geometry-relations-and-snapshot", screen);

  channelsSave(window, primary, capture.output);
  require(window.openProject(primary) && widebandDisplayReady(window),
          "The actual IQ v6 channel project did not reopen");
  for (const auto& [id, evidence] : origins)
    require(*editingMark(window, id).originEvidence == evidence,
            "Channel creation or quality changed original annotation evidence");
  require(session.activeFile()->channels.size() == 3 &&
              editingMark(window, "algo-observation").segments.empty() &&
              editingMark(window, "algo-observation").reviewStatus ==
                  ReviewStatus::Rejected &&
              *locatedChannel().sourceSnapshot == *located.sourceSnapshot,
          "v6 reopening lost channels, unknown time, quality or snapshot");
  const auto reopenedDdc = channelsDdc(window, locatedChannel());
  require(
      reopenedDdc["outputSha256"] == ddcRuns.first().toObject()["outputSha256"],
      "Saving/reopening silently changed the actual DDC output");
  editingSelect(window, "algo-observation");
  editingScene(capture, "12-v6-reopen-preserves-user-range-and-real-ddc",
               screen);
  auto final = capture.scenes.last().toObject();
  final["cpuDdcVerified"] = true;
  final["ddcRuns"] = ddcRuns;
  final["modelInferenceVerified"] = false;
  final["legacyV5BackupVerified"] = true;
  final["legacyBackupPath"] = capture.output.filePath(backupNames.front());
  final["originalEvidenceUnchanged"] = true;
  final["unknownStillUnlocated"] = true;
  final["projectPath"] = primary;
  QFile saved(primary);
  require(saved.open(QIODevice::ReadOnly) &&
              QJsonDocument::fromJson(saved.readAll())
                      .object()["version"]
                      .toInt() == 6,
          "The saved native project is not v6");
  final["v6Verified"] = true;
  capture.scenes[capture.scenes.size() - 1] = final;
}

int runAnnotationChannelsCapture(const QString& outputDirectory) {
  return runAnnotationInteractionsCapture("U3", outputDirectory,
                                          interactionChannelScenes, 12);
}
