#pragma once

// Uses the existing native Capture runner and its actual-screen requirements.
QJsonObject widebandAdaptationClosure(Capture& capture, QScreen* screen,
                                      const QString& project) {
  auto& window = capture.window;
  auto* card = child<DetectionCard>(window, "detectionCard");
  auto* model = child<QComboBox>(*card, "detectionModel");
  auto* spectrum = child<QComboBox>(*card, "detectionSpectrumMode");
  const auto previousModel = model->currentData().toString();
  const auto previousSpectrum = spectrum->currentData().toInt();
  widebandSelectModelSpectrum(window, "ffscn-gui-iq-v1:2");
  widebandNativeSelect(child<QComboBox>(*card, "detectionRangeOrigin"), 1);
  const auto commit = [&](const char* name, const char* value) {
    auto* edit = qobject_cast<QLineEdit*>(widebandDetectionControl(window, name));
    require(edit, "Native analysis parameter is not an editor");
    edit->setFocus();
    edit->selectAll();
    QTest::keyClicks(edit, value);
    QTest::keyClick(edit, Qt::Key_Return);
  };
  commit("detectionFftSize", "3001");
  commit("detectionModelFrames", "3");
  commit("detectionModelHopFrames", "7");
  commit("detectionHopMilliseconds", "2");
  const auto request = card->request();
  require(request.spectrumMode == DetectionSpectrumMode::Mean &&
              request.profile.fftSize == 3001 &&
              request.profile.modelFrames == 3 &&
              request.profile.modelHopFrames == 7 &&
              request.profile.hopSeconds == .002 &&
              card->validationError().isEmpty(),
          "Native committed non-reference analysis settings were blocked: " +
              card->validationError());
  widebandResidentCardAtTop(window);
  widebandScene(capture, "42-adapted-mean-spectrum-configuration", screen);
  widebandSelectModelSpectrum(window, previousModel + ":" +
                                        QString::number(previousSpectrum));

  const auto originalFileId = window.session().activeFile()->metadata.id;
  QFile audit(QStringLiteral(
      SS_SOURCE_DIR "/docs/implementation/wideband-detection/M0/assets.json"));
  require(audit.open(QIODevice::ReadOnly),
          "Cannot read frozen generator audit");
  const auto pair = QJsonDocument::fromJson(audit.readAll())
                        .object()["datasets"]
                        .toObject()["matlabGenerator"]
                        .toObject()["goldenPair"]
                        .toObject();
  const auto wav = pair["wav"].toObject()["path"].toString();
  const auto label = pair["txtLabel"].toObject()["path"].toString();
  require(widebandFileHash(wav) == pair["wav"].toObject()["sha256"] &&
              widebandFileHash(label) == pair["txtLabel"].toObject()["sha256"],
          "Generator WAV/TXT differ from frozen assets");
  SignalImportDialog dialog("生成器标签原生验收", &window);
  dialog.commitSource = [&](FileState file, QString& error) {
    return window.addImportedSource(std::move(file), error);
  };
  dialog.setScreen(screen);
  dialog.show();
  dialog.move(screen->geometry().center() - dialog.rect().center());
  require(dialog.addPath(wav), "Generator WAV did not enter import workbench");
  auto* center = child<QLineEdit>(dialog, "importCenterFrequency");
  center->setFocus();
  center->selectAll();
  QTest::keyClicks(center, "2.5 GHz");
  QTest::keyClick(center, Qt::Key_Return);
  auto* confirmed = child<QCheckBox>(dialog, "importConfirmFormat");
  if (!confirmed->isChecked())
    QTest::mouseClick(confirmed, Qt::LeftButton, Qt::NoModifier,
                      QPoint(8, confirmed->height() / 2));
  require(dialog.controller().rows().front().confirmed,
          "Generator format confirmation did not commit");
  dialog.startImport();
  auto* finish = child<QPushButton>(dialog, "importProgressFinish");
  const bool finished = waitUntil(
      [&] {
        return finish->isEnabled() ||
               dialog.controller().rows().front().status ==
                   ImportStatus::Failed;
      },
      60'000);
  if (!finished || !finish->isEnabled()) {
    dialog.grab().save(capture.output.filePath("generator-import-failed.png"));
    const auto& row = dialog.controller().rows().front();
    require(false, "Generator WAV import failed: " + row.error + " " +
                       row.metadataError + " " +
                       dialog.controller().importLog().join("; "));
  }
  require(dialog.controller().rows().front().status == ImportStatus::Ready,
          "Generator WAV import did not reach Ready");
  QTest::mouseClick(finish, Qt::LeftButton);
  QString error;
  require(window.importLabels(label, &error, false), error);
  auto* file = window.session().activeFile();
  require(file && file->metadata.path == wav.toStdString() &&
              file->metadata.centerFrequencyHz == 2.5e9 &&
              file->marks.size() == 10 &&
              file->sourceSets.back().sourceHash ==
                  widebandFileHash(label).toStdString(),
          "Generator label physical binding or original evidence differs");
  const auto generatorId = file->metadata.id;
  const auto savedAnnotations = widebandAnnotations(*file);
  require(window.importLabels(label, &error, false) && file->marks.size() == 10,
          "Repeated generator label import must be idempotent");
  window.refresh();
  require(widebandDisplayReady(window), "Generator IQ display did not settle");
  widebandSelectAnnotation(window, file->marks.front().id, "generator-txt-v1");
  widebandScene(capture, "43-generator-labels-real-wav", screen);
  require(window.saveProject(project) && window.openProject(project),
          "Generator labels did not save/reopen");
  require(window.session().activeFile()->metadata.id == generatorId &&
              widebandAnnotations(*window.session().activeFile()) ==
                  savedAnnotations,
          "Generator multi-segment/original evidence changed after reopen");
  require(window.session().activateFile(originalFileId),
          "Cannot restore original SigMF source");
  window.refresh();
  require(widebandDisplayReady(window) && window.saveProject(project),
          "Original SigMF source did not restore after generator verification");
  return {{"committedAnalysis", encodeDetectionRequest(request)},
          {"generatorWavSha256", widebandFileHash(wav)},
          {"generatorLabelSha256", widebandFileHash(label)},
          {"generatorObjects", 10},
          {"generatorIdempotentAndReopened", true}};
}

QJsonObject widebandCancellationFeedback(Capture& capture, QScreen* screen,
                                         const QString& project) {
  auto& window = capture.window;
  const auto originalId = window.session().activeFile()->metadata.id;
  std::string generatorId;
  for (const auto& file : window.session().project().files)
    if (file.metadata.path.ends_with(".wav")) generatorId = file.metadata.id;
  require(!generatorId.empty() && window.session().activateFile(generatorId),
          "Real cancellation needs the already imported generator WAV");
  window.refresh();
  require(widebandDisplayReady(window),
          "Cancellation source display not ready");
  auto* card = child<DetectionCard>(window, "detectionCard");
  widebandSelectModelSpectrum(window, "ffscn-gui-iq-v1");
  widebandNativeSelect(child<QComboBox>(*card, "detectionRangeOrigin"), 1);
  const auto commit = [&](const char* name, const char* value) {
    auto* edit = qobject_cast<QLineEdit*>(widebandDetectionControl(window, name));
    require(edit, "Native cancellation parameter is not an editor");
    edit->setFocus();
    edit->selectAll();
    QTest::keyClicks(edit, value);
    QTest::keyClick(edit, Qt::Key_Return);
  };
  commit("detectionFftSize", "8192");
  commit("detectionModelFrames", "3");
  commit("detectionModelHopFrames", "2");
  commit("detectionHopMilliseconds", "2");
  widebandScrollTo(child<QPushButton>(*card, "detectionStrategyCoarse"));
  widebandNativeClick(child<QPushButton>(*card, "detectionStrategyCoarse"));
  require(card->validationError().isEmpty(), card->validationError());
  auto* runButton = child<QPushButton>(window, "runWidebandDetection");
  auto* cancel = child<QPushButton>(window, "cancelDetectionCardRun");
  const auto previous = window.session().activeFile()->detectionRuns.size();
  WidebandFeedbackPaint startPaint(*cancel, [&] {
    const auto* file = window.session().activeFile();
    return file->detectionRuns.size() == previous + 1 && cancel->isEnabled() &&
           !runButton->isEnabled() &&
           cancel->toolTip().contains(QString::fromStdString(
               file->detectionRuns.back().request.runId));
  });
  QElapsedTimer timer;
  timer.start();
  widebandNativeClick(runButton);
  require(waitUntil([&] {
            return window.session().activeFile()->detectionRuns.size() ==
                   previous + 1;
          }),
          "Real task launch did not reach visible state");
  require(waitUntil([&] { return startPaint.count() > 0; }),
          "Visible fixed controls did not paint the new task state");
  const auto startMs = timer.nsecsElapsed() / 1000000.0;
  require(waitUntil(
              [&] {
                const auto& run =
                    window.session().activeFile()->detectionRuns.back();
                return run.lifecycle == DetectionLifecycle::Running &&
                       run.completedWindows > 0 && cancel->isEnabled();
              },
              60'000),
          "Real task did not expose a cancellable persisted prefix");
  WidebandFeedbackPaint cancelPaint(*cancel, [&] {
    const auto& run = window.session().activeFile()->detectionRuns.back();
    return !cancel->isEnabled() &&
           (run.lifecycle == DetectionLifecycle::Cancelling ||
            run.lifecycle == DetectionLifecycle::Cancelled);
  });
  timer.restart();
  widebandNativeClick(cancel);
  require(waitUntil([&] {
            const auto state =
                window.session().activeFile()->detectionRuns.back().lifecycle;
            return state == DetectionLifecycle::Cancelling ||
                   state == DetectionLifecycle::Cancelled;
          }),
          "Real cancel did not reach visible Cancelling/Cancelled state");
  require(waitUntil([&] { return cancelPaint.count() > 0; }),
          "Visible cancel control did not paint the cancellation state");
  const auto cancelFeedbackMs = timer.nsecsElapsed() / 1000000.0;
  require(
      waitUntil(
          [&] {
            return detectionIsTerminal(
                window.session().activeFile()->detectionRuns.back().lifecycle);
          },
          60'000),
      "Real cancellation did not reach a terminal state");
  const auto cancelCompletionMs = timer.nsecsElapsed() / 1000000.0;
  const auto run = window.session().activeFile()->detectionRuns.back();
  require(run.lifecycle == DetectionLifecycle::Cancelled &&
              !run.chunks.empty() && run.completedWindows > 0 &&
              run.completedWindows < run.eligibleWindows &&
              run.device.find("RTX 5060 Laptop") != std::string::npos,
          "Cancellation lost the real model prefix or claimed full coverage");
  QString error;
  DetectionRun durable;
  require(loadDetectionRun(QFileInfo(project).absolutePath(),
                           QString::fromStdString(run.request.runId), durable,
                           error) &&
              encodeDetectionRun(durable) == encodeDetectionRun(run),
          error);
  widebandScene(capture, "44-real-model-cancelled-prefix", screen);
  require(window.saveProject(project), "Cancelled run project did not save");
  require(window.session().activateFile(originalId),
          "Cannot restore original source after cancellation");
  window.refresh();
  require(widebandDisplayReady(window) && window.saveProject(project),
          "Original source did not restore after cancellation");
  return {{"startFeedbackMilliseconds", startMs},
          {"cancelFeedbackMilliseconds", cancelFeedbackMs},
          {"cancelCompletionMilliseconds", cancelCompletionMs},
          {"feedbackIncludesFirstPaint", true},
          {"feedbackTarget", "visible fixed Run/Cancel enabled state"},
          {"startFeedbackPaints", startPaint.count()},
          {"cancelFeedbackPaints", cancelPaint.count()},
          {"targetMilliseconds", 200},
          {"startTargetMet", startMs <= 200},
          {"cancelTargetMet", cancelFeedbackMs <= 200},
          {"run", encodeDetectionRun(run)},
          {"durablePrefixExact", true}};
}
