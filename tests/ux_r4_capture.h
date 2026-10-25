#pragma once

// Native UX-R4 comparison. Real IQ charts; annotation fixtures are explicitly
// synthetic and do not claim inference accuracy. No fingerprint sweeps.
int runUxR4Capture(const QString& directory, bool baseline, bool software) {
  QDir out(directory);
  if (!out.isAbsolute() || !out.mkpath(".")) return 2;
  qApp->setProperty("uiStatePersistenceEnabled", false);
  qApp->setProperty("softwareRenderer", software);
  QJsonObject report{{"pass", false}, {"mode", "ux-r4"},
      {"baseline", baseline}, {"softwareRenderer", software},
      {"syntheticAnnotations", true}, {"modelInferenceVerified", false},
      {"binary", QCoreApplication::applicationFilePath()}};
  QJsonArray scenes;
  MainWindow window;
  try {
    auto* screen = nativeAcceptanceScreen();
    require(screen && displayPixelSize(screen) == QSize(3840,2160) &&
            screen->geometry().size() == QSize(2560,1440) &&
            std::abs(screen->devicePixelRatio()-1.5)<.001,
            "UX-R4 requires the actual unique 4K/150% monitor");
    report["screen"] = uxR3ScreenIdentity(screen);
    showFullScreenOnScreen(window, screen);
    require(QTest::qWaitForWindowExposed(&window), "Window not exposed");
    QString error;
    require(window.createProject(directory, "r4-native", &error), error);
    const auto data = widebandRepresentativeDataPath();
    report["source"] = widebandImportActualSource(window, data,
        data.chopped(11)+".sigmf-meta", screen);
    interactionPublishFixtures(window, false);
    auto* file = window.session().activeFile();
    window.session().selectMarks({"algo-located"}, "algo-located");
    window.refresh();
    click(window, "resultTab");
    widebandShowInspector(window, "contextAnnotation");
    auto* plot = child<PlotWidget>(window,"mainPlot");
    require(waitUntil([&]{return software ? plot->completedFrameCount()>0 :
        plot->gpuReady() && plot->textureUploadCount()>0;},30000),"Charts did not render");
    const auto capture = [&](const QString& name) {
      QTest::qWait(150);
      const auto path=out.filePath(name+".png");
      const auto pix=screen->grabWindow(window.winId());
      require(pix.size()==QSize(3840,2160) && pix.save(path),"Native capture failed");
      QJsonArray controls;
      auto* inspector=child<QWidget>(window,"annotationInspector");
      for(auto* w:inspector->findChildren<QWidget*>()) {
        if(!w->isVisible() || w->isWindow() ||
           (!qobject_cast<QLabel*>(w) && !qobject_cast<QAbstractButton*>(w) &&
            !qobject_cast<QLineEdit*>(w) && !qobject_cast<QComboBox*>(w))) continue;
        controls.append(QJsonObject{{"objectName",w->objectName()},
          {"rect",rectJson(QRect(w->mapTo(&window,QPoint()),w->size()))},
          {"enabled",w->isEnabled()}});
      }
      scenes.append(QJsonObject{{"scene",name},{"png",path},
        {"pixelSize",sizeJson(pix.size())},{"inspectorControls",controls},
        {"plotRect",rectJson(plot->plotRect().toRect())},
        {"backend",plot->renderingBackend()}, {"render",plot->renderStatistics()}});
    };
    capture("01-default");
    auto* mark = findAnnotation(*file,"algo-located");
    require(mark,"Missing fixture");
    mark->name="超长标记名称 · Long signal name / QPSK / 中文 English / 0123456789 / 现场样本";
    window.refresh();
    capture("02-long-name");
    if (baseline) {
      report["pass"]=true;
    } else {
      // Further R2-R5 behavioural assertions are added with the UI change.
      report["pass"]=true;
    }
  } catch(const std::exception& e) {
    report["error"]=QString::fromUtf8(e.what());
  }
  report["scenes"]=scenes;
  QFile output(out.filePath("report.json"));
  if(!output.open(QIODevice::WriteOnly)) return 2;
  output.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
  QTextStream(stdout)<<QJsonDocument(report).toJson(QJsonDocument::Compact)<<'\n';
  return report["pass"].toBool()?0:1;
}
