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
  const auto step=[&](const QString& value) {
    QFile log(out.filePath("steps.txt"));
    if(log.open(QIODevice::Append))log.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)+" "+value+"\n").toUtf8());
  };
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
    require(waitUntil([&]{return software ? plot->isDisplaySettled() && plot->renderingBackend().contains("QPainter") :
        plot->gpuReady() && plot->textureUploadCount()>0;},30000),"Charts did not render");
    const auto capture = [&](const QString& name) {
      report["lastScene"]=name;
      step(name+" begin");
      QTest::qWait(150);
      step(name+" events settled");
      require(window.screen()==screen && window.isFullScreen() &&
              window.size()==QSize(2560,1440) &&
              std::abs(window.devicePixelRatioF()-1.5)<.001,"Native window/DPI changed");
      require(waitUntil([&]{return plot->isDisplaySettled();},10000),"Chart not settled for native readback");
      const auto path=out.filePath(name+".png");
      // Use Qt's native widget/QRhi readback, as the established Capture
      // harness does. Windows grabWindow can return an old DWM backing image.
      const auto pix=window.grab();
      step(name+" widget readback complete");
      require(pix.size()==QSize(3840,2160) && pix.save(path),"Native capture failed");
      QJsonArray controls;
      auto* inspector=child<QWidget>(window,"annotationInspector");
      for(auto* w:inspector->findChildren<QWidget*>()) {
        if(!w->isVisible() || w->window()!=&window ||
           (!qobject_cast<QLabel*>(w) && !qobject_cast<QAbstractButton*>(w) &&
            !qobject_cast<QLineEdit*>(w) && !qobject_cast<QComboBox*>(w))) continue;
        controls.append(QJsonObject{{"objectName",w->objectName()},
          {"rect",rectJson(QRect(w->mapTo(&window,QPoint()),w->size()))},
          {"enabled",w->isEnabled()}});
      }
      if(!baseline && inspector->isVisible()) {
        for(const auto* id:{"annotationInspectorLocate","annotationInspectorCreateChannel",
                           "annotationStartMilliseconds","annotationEndMilliseconds",
                           "annotationLowerMHz","annotationUpperMHz"}) {
          auto* control=child<QWidget>(*inspector,id);
          widebandNativeReachable(control,false);
          require(!qobject_cast<QAbstractButton*>(control) || control->height()>=28,
                  "Main action hit height too small");
        }
        const auto fields=inspector->findChildren<QWidget*>();
        for(auto* a:fields) {
          if(a->window()!=&window || !a->isVisible() ||
             (!qobject_cast<QLabel*>(a)&&!qobject_cast<QAbstractButton*>(a)&&
              !qobject_cast<QLineEdit*>(a)&&!qobject_cast<QComboBox*>(a)))continue;
          const QRect ar(a->mapTo(inspector,QPoint()),a->size());
          require(ar.left()>=0 && ar.right()<inspector->width(),
                  "Inspector horizontal overflow: "+a->objectName());
          if(auto* label=qobject_cast<QLabel*>(a); label && !label->text().isEmpty())
            require(label->height()>=label->heightForWidth(label->width()),
                    "Wrapped text height clipped: "+a->objectName());
          for(auto* b:fields) {
            if(a==b || b->window()!=&window || !b->isVisible() ||
               a->isAncestorOf(b)||b->isAncestorOf(a)||
               (!qobject_cast<QLabel*>(b)&&!qobject_cast<QAbstractButton*>(b)&&
                !qobject_cast<QLineEdit*>(b)&&!qobject_cast<QComboBox*>(b)))continue;
            require(!ar.intersects(QRect(b->mapTo(inspector,QPoint()),b->size())),
                    "Inspector controls overlap: "+a->objectName()+" / "+b->objectName());
          }
        }
      }
      scenes.append(QJsonObject{{"scene",name},{"png",path},
        {"captureMethod","native QWidget grab with QRhi readback"},
        {"identityText",child<QLabel>(window,"annotationIdentity")->text()},
        {"propertyWidth",child<QWidget>(window,"properties")->width()},
        {"pixelSize",sizeJson(pix.size())},{"inspectorControls",controls},
        {"plotRect",rectJson(plot->plotRect().toRect())},
        {"backend",plot->renderingBackend()}, {"render",plot->renderStatistics()}});
      step(name+" assertions complete");
    };
    capture("01-default");
    auto* mark = findAnnotation(*file,"algo-located");
    require(mark,"Missing fixture");
    mark->name="超长标记名称 · Long signal name / QPSK / 中文 English / 0123456789 / 现场样本";
    step("long name refresh begin");
    window.refresh();
    step("long name refresh complete");
    capture("02-long-name");
    if (baseline) {
      report["pass"]=true;
    } else {
      auto* inspector=child<QWidget>(window,"annotationInspector");
      auto* scroll=child<QScrollArea>(window,"propScroll");
      const auto tap=[&](const char* id) {
        auto* control=child<QWidget>(window,id);
        widebandScrollTo(control);
        widebandNativeClick(control);
      };
      const auto select=[&](const std::string& id) {
        const auto before=file->view;
        interactionClickRow(window,id);
        require(file->view.time.begin==before.time.begin && file->view.time.end==before.time.end &&
                file->view.frequency.lowerHz==before.frequency.lowerHz &&
                file->view.frequency.upperHz==before.frequency.upperHz,"Selection unexpectedly zoomed");
        scroll->verticalScrollBar()->setValue(0);
      };
      // Resize through the application's native handle, not a different screen
      // or a synthetic huge window. Keep the same full-screen 4K/DPR.
      const auto width=[&](int target) {
        auto* properties=child<QWidget>(window,"properties");
        auto* handle=child<QWidget>(window,"rightPanelResize");
        const auto start=handle->mapTo(&window,handle->rect().center());
        const int delta=properties->width()-target;
        QTest::mousePress(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,start);
        QTest::mouseMove(window.windowHandle(),start+QPoint(delta,0),30);
        QTest::mouseRelease(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,start+QPoint(delta,0));
        require(waitUntil([&]{return properties->width()==target;}),"Sidebar resize failed");
        scroll->verticalScrollBar()->setValue(0);
      };
      width(320); capture("03-narrow-320-long-name");
      width(650); capture("04-expanded-650");
      width(390);
      tap("propClose"); capture("05-hidden");
      tap("propRailOpen");
      const auto view=file->view;
      tap("annotationNextSegment");
      require(file->activeSegmentId==findAnnotation(*file,"algo-located")->segments[1].id,
              "Next segment not selected");
      require(file->view.time.begin==view.time.begin && file->view.time.end==view.time.end,
              "Segment selection zoomed");
      tap("annotationPreviousSegment");
      tap("annotationInspectorLocate");
      require(file->view.time.begin!=view.time.begin || file->view.time.end!=view.time.end,
              "Locate did not change view");
      capture("06-located");
      window.session().setView(fullRange(file->metadata)); window.refresh();
      auto* set=findSourceSet(*file,findAnnotation(*file,"algo-located")->sourceSetId);
      require(set,"Missing source set");
      set->originalPath="D:/very-long-source-path/"+std::string(240,'p')+"/标签.txt";
      auto origin=std::make_shared<OriginEvidence>(*findAnnotation(*file,"algo-located")->originEvidence);
      origin->originalFieldsJson="{\"long\":\""+std::string(16000,'x')+"END_OF_RAW\"}";
      findAnnotation(*file,"algo-located")->originEvidence=origin;
      window.refresh();
      const auto originalRecord=encodeAnnotation(*findAnnotation(*file,"algo-located"));
      auto* evidence=inspector->findChild<QWidget*>("annotationEvidenceWidget");
      require(evidence,"Missing shared evidence widget");
      auto* open=evidence->findChild<QToolButton*>("annotationEvidenceDetailsToggle");
      widebandScrollTo(open); widebandNativeClick(open);
      auto* dialog=evidence->findChild<QDialog*>("annotationEvidenceDetailsDialog");
      require(waitUntil([&]{return dialog->isVisible();}),"Evidence details did not open");
      auto* copy=dialog->findChild<QPushButton*>("annotationCopyEvidence");
      widebandNativeClick(copy);
      const auto copied=QApplication::clipboard()->text();
      require(copied.contains("END_OF_RAW") && copied.contains(QString::fromStdString(set->sourceHash)) &&
              copied.contains(QString::fromStdString(set->originalPath)),"Evidence copy was truncated");
      auto* raw=dialog->findChild<QPlainTextEdit*>("annotationEvidence");
      auto* rawToggle=dialog->findChild<QToolButton*>("annotationOriginalFieldsToggle");
      widebandScrollTo(rawToggle); widebandNativeClick(rawToggle); widebandScrollTo(raw);
      const auto detailPath=out.filePath("07-full-evidence-dialog.png");
      require(dialog->grab().save(detailPath),"Cannot capture evidence dialog");
      report["detailsScreenshot"]=detailPath;
      report["fullCopyCharacters"]=copied.size();
      require(encodeAnnotation(*findAnnotation(*file,"algo-located"))==originalRecord,
              "Disclosure mutated evidence");
      dialog->close(); scroll->verticalScrollBar()->setValue(0);
      auto* edit=child<QLineEdit>(window,"annotationStartMilliseconds");
      widebandNativeClick(edit); edit->selectAll(); QTest::keyClicks(edit,"3.1");
      require(findAnnotation(*file,"algo-located")->segments[0].range.time.begin==58000,
              "Draft committed while typing");
      QTest::keyClick(edit,Qt::Key_Return);
      require(findAnnotation(*file,"algo-located")->segments[0].range.time.begin==62000,
              "Enter did not commit actual geometry");
      tap("undoAnnotations");
      require(findAnnotation(*file,"algo-located")->segments[0].range.time.begin==58000,
              "Geometry undo failed");
      tap("splitAnnotationSegment");
      require(findAnnotation(*file,"algo-located")->segments.size()==3,"Split failed");
      tap("undoAnnotations");
      // Native deletion confirmation and undo, preserving immutable evidence.
      QString modalError;
      QTimer::singleShot(120,[&] {
        auto* confirm=window.findChild<QPushButton*>("deleteAnnotationsConfirm");
        if(confirm && confirm->isVisible())widebandNativeClick(confirm);
        else {modalError="Delete confirmation not visible"; if(auto* modal=QApplication::activeModalWidget())modal->close();}
      });
      tap("deleteAnnotationSegment");
      require(modalError.isEmpty() && findAnnotation(*file,"algo-located")->segments.size()==1,
              "Segment deletion failed "+modalError);
      tap("undoAnnotations");
      require(findAnnotation(*file,"algo-located")->segments.size()==2,"Delete undo failed");
      capture("08-edited-and-restored");
      select("algo-observation"); capture("09-time-unknown");
      select("algo-located");
      window.session().selectMarks({"algo-located","algo-observation"},"algo-located");window.refresh();
      require(!child<QPushButton>(window,"annotationInspectorCreateChannel")->isEnabled(),"Multi-selection created ambiguous channel");
      capture("10-multi-selection");
      window.session().selectMarks({});window.refresh();capture("11-no-selection");
      select("algo-located");
      auto* current=findAnnotation(*file,"algo-located");
      const auto sourceId=current->sourceSetId;
      current->sourceSetId="missing-source-fixture";window.refresh();capture("12-missing-source");
      current->sourceSetId=sourceId;window.refresh();
      for(const auto& annotation:file->marks) {
        if(annotation.sourceKind==AnnotationSource::Manual && !annotation.deleted) {select(annotation.id);break;}
      }
      capture("13-manual-source");
      for(const auto& annotation:file->marks) {
        if(annotation.sourceKind==AnnotationSource::ImportedLabel && !annotation.deleted) {select(annotation.id);break;}
      }
      capture("14-imported-source");
      select("algo-located");
      // The new primary button must route to the existing real channel dialog.
      Channel channel;
      QTimer::singleShot(120,[&] {
        try {
          auto* dialog=window.findChild<QDialog*>("channelConfigDialog");
          require(dialog,"Channel dialog missing");
          channel=channelsConfirm(window,*dialog,false);
        } catch(const std::exception& e) {
          modalError=QString::fromUtf8(e.what());
          if(auto* modal=QApplication::activeModalWidget())modal->close();
        }
      });
      tap("annotationInspectorCreateChannel");
      require(waitUntil([&]{return !channel.id.empty() || !modalError.isEmpty();},10000),
              "Channel confirmation did not complete");
      require(modalError.isEmpty() && !channel.id.empty() && bool(channel.sourceSnapshot),
              "New channel action did not preserve source snapshot "+modalError);
      report["channelId"]=QString::fromStdString(channel.id);
      ChannelDspPlan plan; ChannelSampleData samples;
      require(makeChannelDspPlan(file->metadata,channel,plan,error) &&
              processChannelSamples(file->metadata,channel,plan,{0,512},samples) &&
              samples.samples.size()==512,"Real channel DDC failed: "+error);
      report["cpuDdcOutputSamples"]=int(samples.samples.size());
      tap("detectionEvidenceMode"); capture("15-auxiliary-evidence");
      tap("psdMode");capture("16-psd");
      tap("waterfallMode");capture("17-waterfall");
      tap("timeFrequencyMode");
      // Independent selection entrances: tree -> plot -> table, with no zoom.
      select("algo-located");
      auto* tree=child<QTreeWidget>(window,"projectTree");
      auto* node=interactionTreeItem(*tree,"algo-observation");
      require(node,"Tree projection lost an annotation");
      for(auto* p=node->parent();p;p=p->parent())p->setExpanded(true);
      tree->scrollToItem(node); QCoreApplication::processEvents();
      const auto treeView=file->view;
      const auto nodeRect=tree->visualItemRect(node);
      const auto treePoint=tree->viewport()->mapTo(&window,nodeRect.center());
      QTest::mouseClick(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,treePoint);
      require(waitUntil([&]{return file->activeMarkId=="algo-observation";}),"Native tree selection not linked");
      require(file->view.time.begin==treeView.time.begin && file->view.time.end==treeView.time.end,
              "Tree selection zoomed");
      // The real imported label overlaps this synthetic algorithm rectangle.
      // Hide that layer using its real checkbox to test an unambiguous graph
      // selection; overlapping-hit cycling is covered by the existing UI test.
      tap("annotationLayer0");
      const auto target=plot->toPixel(100000,2502.17e6).toPoint();
      QTest::mouseClick(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,plot->mapTo(&window,target));
      require(waitUntil([&]{return file->activeMarkId=="algo-located";}),"Native graph selection not linked");
      tap("annotationLayer0");
      auto* table=child<QTableView>(window,"annotationTable");
      require(interactionTableIndex(*table,"algo-located").isValid(),"Plot selection missing from records");
      capture("18-tree-plot-table-linked");
      const auto powers=plot->powerGenerationCount();
      moveThroughWindow(plot,target);
      require(plot->powerGenerationCount()==powers,"Hover reran spectral analysis");
      // Dense annotations exercise labels/handles; fixtures do not run a model.
      for(int i=0;i<24;++i)
        window.session().addMark({{SampleIndex(40000+i*8000),SampleIndex(60000+i*8000)},
            {2491e6+i*0.6e6,2491.3e6+i*0.6e6}});
      window.refresh();
      select("algo-located");
      file->metadata.name="长文件名_中文与English_"+std::string(100,'L')+".sigmf-data";
      window.refresh();
      tap("detectionRoiTool");
      uxR3Drag(window,*plot,plot->toPixel(30000,2493e6).toPoint(),
                          plot->toPixel(180000,2504e6).toPoint());
      require(plot->hasDetectionRoiPreview(),"Native ROI preview failed");
      capture("19-dense-roi-long-file");
      QTest::keyClick(plot,Qt::Key_Escape);
      require(!plot->hasDetectionRoiPreview(),"ROI cancel failed");
      auto* palette=child<QComboBox>(window,"colormap");
      widebandNativeSelect(palette,(palette->currentIndex()+1)%palette->count());
      capture("20-palette-and-handles");
      const auto projectPath=out.filePath("r4-native.ssproj");
      require(window.saveProject(projectPath),"Save .ssproj failed");
      require(window.openProject(projectPath),"Reopen .ssproj failed");
      file=window.session().activeFile();
      require(file && !file->channels.empty() && file->channels.back().sourceSnapshot,
              "Reopen lost channel provenance");
      report["projectPath"]=projectPath;
      capture("21-reopened");
      // Explicit UI-only metadata stress fixture: this last scene is not a
      // claim of RF analysis at this carrier frequency.
      window.session().addDemoFile("频率字段排版夹具 · synthetic metadata",1e6,9.99999e11,.02);
      file=window.session().activeFile();
      const auto stress=window.session().addMark({{1000,18000},{9.99999e11-200000,9.99999e11+200000}});
      require(!stress.empty(),"Valid high-frequency fixture was rejected");
      window.session().selectMarks({stress},stress); window.refresh();
      widebandShowInspector(window,"contextAnnotation");width(320);
      require(child<QLineEdit>(window,"annotationUpperMHz")->text().toDouble()>9e5,
              "Large frequency field missing");
      capture("22-large-frequency-synthetic-metadata");
      report["largeFrequencySceneIsSyntheticMetadata"]=true;
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
