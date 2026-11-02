#pragma once

QJsonArray refinementHeaders(MainWindow& window) {
  QJsonArray result;
  auto* maximized=window.findChild<QWidget*>("maximizeHost");
  for(auto* widget:window.findChildren<QWidget*>()) {
    auto* strip=dynamic_cast<ChartControlStrip*>(widget);
    if(!strip||!strip->isVisible())continue;
    if(maximized&&maximized->isVisible()&&!maximized->isAncestorOf(strip))continue;
    std::vector<QRect> rectangles;int rightMost=-1;
    for(auto* field:strip->findChildren<QWidget*>()){
      if(!field->isVisible()||(!qobject_cast<QAbstractButton*>(field)&&!qobject_cast<QComboBox*>(field)))continue;
      bool nested=false;for(auto* owner=field->parentWidget();owner&&owner!=strip;owner=owner->parentWidget())nested|=qobject_cast<QAbstractButton*>(owner)||qobject_cast<QComboBox*>(owner);
      if(nested)continue;
      widebandUxReachable(field,window,false);
      const QRect rect(field->mapTo(&window,QPoint()),field->size());
      for(const auto& previous:rectangles)require(!rect.intersects(previous),"Round two header fields overlap: "+field->objectName());
      rectangles.push_back(rect);rightMost=std::max(rightMost,field->mapTo(strip,QPoint(field->width()-1,0)).x());
    }
    require(rightMost<0||std::abs(rightMost-(strip->width()-1))<=1,"Chart settings are not right aligned: "+strip->objectName());
    result.append(QJsonObject{{"strip",strip->objectName()},{"controls",static_cast<int>(rectangles.size())},{"rightAligned",true}});
  }
  return result;
}

void refinementReveal(MainWindow& window,QWidget* field) {
  require(field,"The real field is missing");
  if(!field->isVisible()) {
    ChartControlStrip* owner=nullptr;
    for(auto* current=field;current&&!owner;current=current->parentWidget()){
      owner=dynamic_cast<ChartControlStrip*>(current->property("chartControlOwner").value<QObject*>());
      if(!owner)owner=dynamic_cast<ChartControlStrip*>(current->property("chartPanelOwner").value<QObject*>());
    }
    require(owner&&owner->overflowButton()->isVisible(),"Field has no visible More entrance: "+field->objectName());
    if(!owner->popoverFrame()->isVisible())widebandNativeClick(owner->overflowButton());
  }
  widebandScrollTo(field);widebandUxReachable(field,window,false);
}

int runUiRefinementCapture(const QString& directory) {
  QDir output(directory);
  if(!QFileInfo(directory).isAbsolute()||!QDir().mkpath(directory)||QFileInfo::exists(output.filePath("ui-refinement-report.json")))return 2;
  qApp->setProperty("uiStatePersistenceEnabled",false);
  MainWindow window;window.openNarrowbandDemoProject();
  const auto projectDirectory=output.filePath("project");QDir().mkpath(projectDirectory);
  const auto projectPath=QDir(projectDirectory).filePath("project.ssproj");
  if(!window.saveProject(projectPath))return 2;
  auto* screen=nativeAcceptanceScreen();showFullScreenOnScreen(window,screen);
  Capture capture{window,output,{child<PlotWidget>(window,"navigationPlot"),child<PlotWidget>(window,"auxPlot"),child<PlotWidget>(window,"mainPlot")}};
  QJsonObject report{{"mode","ui-refinement-round2"},{"pass",false},{"screen",widebandScreenJson(screen)},
    {"platform",QGuiApplication::platformName()},{"qtVersion",qVersion()},{"modelInferenceVerified",false},
    {"inputKind","embedded real int16 IQ with CPU DDC; synthetic modulation/recognition/demodulation remain labelled"},
    {"startedUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
  const auto clickName=[&](const char* name){auto* field=child<QWidget>(window,name);refinementReveal(window,field);widebandNativeClick(field);};
  const auto select=[&](const char* name,int index){auto* field=child<QComboBox>(window,name);refinementReveal(window,field);widebandNativeSelect(field,index);};
  const auto escape=[&]{QTest::keyClick(window.windowHandle(),Qt::Key_Escape);QTest::qWait(50);};
  const auto scene=[&](const QString& name,bool rendered=true){
    require(window.isFullScreen()&&window.screen()==screen&&window.size()==QSize(2560,1440),"Round two must stay native 4K/150 fullscreen");
    const auto controls=refinementHeaders(window);
    capture.scene(name,nullptr,{},rendered);
    auto record=capture.scenes.last().toObject();record["headerChecks"]=controls;record["screen"]=widebandScreenJson(screen);capture.scenes[capture.scenes.size()-1]=record;
    require(record["pass"].toBool(),"Round two scene failed: "+name+" "+record["error"].toString());
  };
  try {
    auto* narrow=child<NarrowbandWorkspace>(window,"narrowbandWorkspace");
    require(waitUntil([&]{return narrow->visibleChartsSettled();},20000),"Initial narrowband display did not finish");
    capture.waitForSettledDisplay();scene("01-narrowband-observe");
    require(!window.findChild<QWidget*>("narrowbandChartToolbar")&&!window.findChild<QWidget*>("narrowbandConstellationPreview"),"Old global toolbar/preview remained");
    auto* source=child<QComboBox>(window,"psdSource");
    select("psdSource",0);
    auto* points=child<QComboBox>(window,"narrowbandPsdFft");
    select("narrowbandPsdFft",points->findData(8192));
    require(window.session().activeChannel()->psdFftSize==8192,"Visible PSD N did not commit");
    scene("02-narrowband-psd-8192");
    auto* psdMore=chartSettingsEntry(window,"psdSettings");widebandNativeClick(psdMore);
    require(chartSettingsFrame(window,"psdSettings")->isVisible(),"Narrow PSD More did not open");
    require(chartSettingsFrame(window,"psdSettings")->findChildren<QMenu*>().empty(),"More settings contains a nested menu");
    scene("03-narrowband-psd-flat-more");escape();
    auto* heat=child<QWidget>(window,"narrowbandStftPanelChart");
    const QPoint heatPoint(66+(heat->width()-82)/2,14+(heat->height()-55)/2);
    QTest::mouseClick(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,heat->mapTo(&window,heatPoint));
    require(waitUntil([&]{return source->currentIndex()==1&&!points->isEnabled();}),"Frame PSD mode is not explicit");
    require(points->currentText().contains("STFT"),"Frame PSD N does not explain its STFT binding");
    scene("04-narrowband-frame-psd");
    select("psdSource",0);require(points->isEnabled()&&points->currentData().toInt()==8192,"Independent PSD N did not restore");
    scene("05-narrowband-statistical-psd-restored");
    clickName("modulationPage");scene("06-modulation-and-eye-settings");
    clickName("deepLearningPage");scene("07-recognition-empty");
    auto* status=child<QLabel>(window,"recognitionStatus");
    require(!status->text().contains("synthetic="),"Implementation flag leaked into the normal status");
    for(auto* card:window.findChildren<QLabel*>("recognitionStatCard"))require(!card->text().contains("0.82"),"Initial recognition result is fabricated");
    clickName("runRecognition");
    require(child<QPushButton>(window,"stopRecognition")->isEnabled(),"Recognition demonstration did not start");
    scene("08-recognition-running");clickName("stopRecognition");scene("09-recognition-stopped");
    clickName("runRecognition");
    require(waitUntil([&]{return child<QProgressBar>(window,"recognitionProgress")->value()==100;},20000),"Recognition demonstration did not finish");
    scene("10-recognition-completed");
    auto* model=child<QComboBox>(window,"recognitionModel");
    refinementReveal(window,model);widebandNativeSelect(model,(model->currentIndex()+1)%model->count());
    require(status->text().contains("过期"),"Changed recognition configuration did not mark its result stale");
    scene("11-recognition-stale");
    clickName("demodulationPage");
    require(waitUntil([&]{return narrow->visibleChartsGpuReady()&&narrow->visibleCompletedFrames()>0;}),"Demodulation charts did not render");
    scene("12-demodulation-labelled-example",false);
    clickName("observePage");scene("13-observation-restored");
    const auto channel=*window.session().activeChannel();
    const auto ddc=channelsDdc(window,channel);require(ddc["outputSamples"].toInt()>0,"No real CPU DDC samples");
    widebandMenuAction(window,"saveProjectAction");
    report["projectPicker"]=widebandUxOpenSavedProject(window,projectPath,output);
    require(waitUntil([&]{return window.session().activeChannel()!=nullptr&&narrow->visibleChartsSettled();},20000),"Saved narrowband project did not restore");
    require(window.session().activeChannel()->psdFftSize==8192,"Saved independent PSD N changed");
    const auto restored=channelsDdc(window,*window.session().activeChannel());
    require(restored["outputSha256"]==ddc["outputSha256"],"Save/reopen changed real DDC output");
    report["cpuDdc"]=ddc;report["cpuDdcVerified"]=true;report["v6ReopenVerified"]=true;
    scene("14-project-reopened");
    clickName("narrowbandClose");scene("15-wideband-waveform");
    clickName("psdMode");scene("16-wideband-psd-right-settings");
    auto* windowType=child<QComboBox>(window,"psdWindow");select("psdWindow",windowType->findText("Kaiser"));
    if(!chartSettingsFrame(window,"psdSettings")->isVisible())widebandNativeClick(chartSettingsEntry(window,"psdSettings"));
    refinementReveal(window,child<QWidget>(window,"psdKaiserBeta"));scene("17-wideband-psd-kaiser-more");escape();
    widebandNativeClick(chartSettingsEntry(window,"stftSettings"));scene("18-wideband-stft-flat-more");escape();
    clickName("waterfallMode");scene("19-wideband-waterfall");
    clickName("specMaximize");scene("20-wideband-main-maximized");clickName("specMaximize");
    clickName("resourceToggle");scene("21-sidebar-collapsed-responsive");clickName("resourceToggle");
    auto& file=*window.session().activeFile();
    file.metadata.name="很长的当前源信号名称_用于确认工程树名称省略与当前对象标识_完整结尾.iq";
    file.channels.front().name="很长的窄带通道名称_完整通道结尾";
    const auto view=file.view;
    const ViewRange first{{view.time.begin,view.time.begin+(view.time.end-view.time.begin)/3},view.frequency};
    const ViewRange second{{view.time.begin+(view.time.end-view.time.begin)/3,view.time.end},view.frequency};
    const auto firstId=window.session().addMark(first),secondId=window.session().addMark(second);
    window.refresh();QTest::qWait(100);scene("22-project-tree-names-and-current");
    auto* tree=child<QTreeWidget>(window,"projectTree");
    const auto treeClick=[&](const std::string& id,Qt::KeyboardModifiers modifiers){
      QTreeWidgetItem* item=nullptr;for(QTreeWidgetItemIterator it(tree);*it;++it)if((*it)->data(0,Qt::UserRole).toString()=="mark"&&(*it)->data(0,Qt::UserRole+1).toString()==QString::fromStdString(id)){item=*it;break;}
      require(item,"Prepared mark not present in the tree");
      auto* scroll=child<QScrollArea>(window,"resourceScroll");
      scroll->ensureVisible(tree->mapTo(scroll->widget(),tree->visualItemRect(item).center()).x(),tree->mapTo(scroll->widget(),tree->visualItemRect(item).center()).y(),20,30);QTest::qWait(60);
      QTest::mouseClick(window.windowHandle(),Qt::LeftButton,modifiers,tree->viewport()->mapTo(&window,tree->visualItemRect(item).center()));
    };
    treeClick(firstId,Qt::NoModifier);treeClick(secondId,Qt::ControlModifier);
    require(window.session().activeFile()->selectedMarkIds.size()==2,"Tree visible Ctrl multi-selection failed");
    scene("23-project-tree-multiselection");
    const auto selected=window.session().activeFile()->selectedMarkIds;
    window.refresh();require(window.session().activeFile()->selectedMarkIds==selected,"Background refresh changed tree selection");
    scene("24-project-tree-refresh-preserved");
    report["pass"]=capture.passed;
  }catch(const std::exception& error){report["error"]=QString::fromUtf8(error.what());window.grab().save(output.filePath("failed-refinement.png"));}
  report["scenes"]=capture.scenes;report["sceneCount"]=capture.scenes.size();report["fullScreen"]=window.isFullScreen();report["logicalWindowSize"]=sizeJson(window.size());report["renderEvidence"]=capture.evidence();
  report["finishedUtc"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  QFile reportFile(output.filePath("ui-refinement-report.json"));
  if(!reportFile.open(QIODevice::WriteOnly|QIODevice::NewOnly))return 2;
  reportFile.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
  return report["pass"].toBool()?0:1;
}
