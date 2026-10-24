#pragma once
// Included after the existing native monitor/control helpers in ui_capture.cpp.
QJsonObject narrowbandScreenJson(QScreen* screen) {
    auto result=widebandScreenJson(screen);
#ifdef Q_OS_WIN
    UINT32 pathCount=0,modeCount=0;
    if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pathCount,&modeCount)!=ERROR_SUCCESS)return result;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pathCount,paths.data(),&modeCount,modes.data(),nullptr)!=ERROR_SUCCESS)return result;
    for(UINT32 i=0;i<pathCount;++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};source.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(source),paths[i].sourceInfo.adapterId,paths[i].sourceInfo.id};
        if(DisplayConfigGetDeviceInfo(&source.header)!=ERROR_SUCCESS || QString::fromWCharArray(source.viewGdiDeviceName)!=displayDeviceName(screen))continue;
        DISPLAYCONFIG_TARGET_PREFERRED_MODE preferred{};preferred.header={DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE,sizeof(preferred),paths[i].targetInfo.adapterId,paths[i].targetInfo.id};
        if(DisplayConfigGetDeviceInfo(&preferred.header)==ERROR_SUCCESS)result["preferredPixelSize"]=sizeJson(QSize(preferred.width,preferred.height));
        result["outputTechnology"]=static_cast<qint64>(paths[i].targetInfo.outputTechnology);
        break;
    }
#endif
    return result;
}
QJsonArray narrowbandScreensJson() {
    QJsonArray all;for(auto* screen:QGuiApplication::screens())all.append(narrowbandScreenJson(screen));return all;
}
bool isNarrowbandAcceptanceScreen(QScreen* screen) {
    const auto identity=narrowbandScreenJson(screen);const auto preferred=identity["preferredPixelSize"].toObject();
    return identity["friendlyName"].toString().compare("Redmi 27 NU",Qt::CaseInsensitive)==0 &&
        !identity["monitorDevicePath"].toString().isEmpty() && preferred["width"].toInt()==3840 && preferred["height"].toInt()==2160 &&
        displayPixelSize(screen)==QSize(3840,2160) && screen->geometry().size()==QSize(2560,1440) && std::abs(screen->devicePixelRatio()-1.5)<.001;
}
int runNarrowbandAnalysisCapture(const QString& directory,bool software,bool auxiliary) {
    QDir output(directory);require(QFileInfo(directory).isAbsolute() && output.mkpath("."),"An absolute fresh capture directory is required");
    const auto reportPath=output.filePath("narrowband-analysis-report.json");
    require(!QFileInfo::exists(reportPath),"Use a fresh directory to preserve capture evidence");
    QJsonObject report{{"mode","narrowband-analysis-ux-r3"},{"platform",QGuiApplication::platformName()},
        {"screens",narrowbandScreensJson()},{"nativeAcceptance",false},{"auxiliaryDisplay",auxiliary},{"softwareRenderer",software},{"pass",false}};
    const auto write=[&]{QFile file(reportPath);const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size();};
    QScreen* screen=nullptr;int candidates=0;
    for(auto* item:QGuiApplication::screens())if(isNarrowbandAcceptanceScreen(item)){screen=item;++candidates;}
    if(auxiliary)screen=nativeAcceptanceScreen();
    if((!auxiliary && candidates!=1) || !screen) {
        report["blocked"]=true;report["error"]="指定 Redmi 27 NU 物理 3840x2160 / 150% 屏幕未作为唯一合格 Qt 屏幕暴露。";
        write();return 3;
    }
    report["screen"]=narrowbandScreenJson(screen);
    qApp->setProperty("softwareRenderer",software);qApp->setProperty("uiStatePersistenceEnabled",false);
    MainWindow window;QJsonArray scenes,projects;
    try {
        showFullScreenOnScreen(window,screen);
        require(waitUntil([&]{return window.isFullScreen() && window.screen()==screen && window.size()==QSize(2560,1440);},10000),"Fullscreen must use the actual selected screen and system DPI");
        auto* workspace=child<NarrowbandWorkspace>(window,"narrowbandWorkspace");
        const auto scene=[&](const QString& name) {
            require(waitUntil([&]{return workspace->visibleChartsSettled() && (software || (workspace->visibleChartsGpuReady() && (workspace->visibleTextureUploads()>0 || workspace->visibleGpuVertexUploads()>0) && workspace->visibleGpuDataDrawCalls()>0));},15000),"Real chart rendering did not settle: "+name);
            QTest::qWait(100);require(window.size()==QSize(2560,1440),"Controls must fit the actual logical workspace");
            const auto capture=window.grab();const auto path=output.filePath(name+".png");
            require(capture.size()==QSize(3840,2160) && std::abs(capture.devicePixelRatioF()-1.5)<.001 && capture.save(path),"Capture must use native screen dimensions and actual DPI");
            const auto backend=workspace->visibleBackendDescription();
            require(software || (backend.contains("D3D11",Qt::CaseInsensitive) && !backend.contains("CPU",Qt::CaseInsensitive)),"D3D11 hardware backend is required");
            scenes.append(QJsonObject{{"scene",name},{"pass",true},{"png",path},{"pixelSize",sizeJson(capture.size())},
                {"devicePixelRatio",capture.devicePixelRatioF()},{"analysis",narrowbandAnalysisEvidence(workspace->analysisResult().get())},
                {"render",workspace->renderStatistics()},{"backend",backend}});
        };
        const auto enter=[&](QObject& owner,const char* name,const QString& value) {
            auto* edit=child<QLineEdit>(owner,name);widebandScrollTo(edit);widebandNativeReachable(edit);
            auto* root=edit->window();QTest::mouseClick(root->windowHandle(),Qt::LeftButton,Qt::NoModifier,root->mapFromGlobal(edit->mapToGlobal(edit->rect().center())));
            QTest::keyClick(root->windowHandle(),Qt::Key_A,Qt::ControlModifier);for(const auto character:value.toLatin1())QTest::keyClick(root->windowHandle(),character);QTest::keyClick(root->windowHandle(),Qt::Key_Return);
        };
        for(const auto& suffix:{QString("raw"),QString("sigmf-meta")}) {
            const auto prefix=suffix=="raw"?QString("ci16"):QString("cf32");QString error;
            require(window.createProject(directory,prefix,&error),error);
            SignalImportDialog import(prefix,&window);import.setScreen(screen);import.show();import.move(screen->geometry().center()-import.rect().center());
            require(import.addPath(QDir(qEnvironmentVariable("SS_NARROWBAND_FIXTURES")).filePath("qpsk-a35-awgn20."+suffix)),"Actual generated IQ fixture is required");
            if(suffix=="raw") {
                enter(import,"importSampleRate","20 MS/s");enter(import,"importCenterFrequency","100 MHz");enter(import,"importBandwidth","20 MHz");
                auto* confirmed=child<QCheckBox>(import,"importConfirmFormat");widebandScrollTo(confirmed);if(!confirmed->isChecked())widebandNativeClick(confirmed);
            }
            import.commitSource=[&](FileState file,QString& e){return window.addImportedSource(std::move(file),e);};import.startImport();
            auto* finish=child<QPushButton>(import,"importProgressFinish");require(waitUntil([&]{return finish->isEnabled();},15000),"Actual source scanning must finish");widebandNativeClick(finish);
            auto* file=window.session().activeFile();require(file && !file->metadata.demo && file->metadata.sampleRateHz==20e6 && !file->navigationEnvelope.empty(),"Source import must retain real IQ and peak navigation");
            window.session().addMark({{0,file->metadata.sampleCount},{101.5e6,102.5e6}});window.refresh();
            widebandNativeClick(child<QPushButton>(window,"extract"));auto* dialog=child<QDialog>(window,"channelConfigDialog");
            auto* factor=child<QComboBox>(*dialog,"channelBandwidthFactor");auto* preview=child<QLabel>(*dialog,"channelDspPlanPreview");
            require(preview->property("realizedRateHz").toDouble()==2e6,"Default output must be actual 2B");
            for(int i=0;i<4;++i){widebandNativeSelect(factor,i);require(preview->property("realizedRateHz").toDouble()==factor->currentData().toDouble()*1e6,"Each factor must plan the actual Fs");}
            widebandNativeSelect(factor,1);widebandNativeClick(child<QPushButton>(*dialog,"confirmChannelConfig"));
            require(waitUntil([&]{return window.session().activeChannel()!=nullptr;},15000),"Background FIR validation must publish a channel");
            scene(prefix+"-01-ddc");widebandNativeClick(child<QPushButton>(window,"modulationPage"));
            require(waitUntil([&]{return workspace->analysisResult()!=nullptr;},15000),"Raw analysis did not finish");
            auto result=workspace->analysisResult();require(!result->constellation.empty() && result->eyes.empty() && result->constellation.front().value==result->source->samples.front(),"Raw scatter must match continuous IQ and unknown Rs must not create an eye");scene(prefix+"-02-raw");
            widebandNativeClick(child<QToolButton>(window,"narrowbandSyncSettingsToggle"));
            widebandNativeSelect(child<QComboBox>(window,"analysisSymbolRateMode"),1);
            auto* rs=child<QDoubleSpinBox>(window,"symbolRate");auto* rsEdit=rs->findChild<QLineEdit*>();require(rsEdit,"Rs editor is absent");
            widebandNativeReachable(rs);rsEdit->setFocus();QTest::keyClick(window.windowHandle(),Qt::Key_A,Qt::ControlModifier);for(const auto character:QByteArray("625000"))QTest::keyClick(window.windowHandle(),character);QTest::keyClick(window.windowHandle(),Qt::Key_Return);
            require(waitUntil([&]{auto r=workspace->analysisResult();return r && r->eyeStatus==AnalysisStatus::Ready && r->symbolRateHz==625000;},15000),"Manual fractional-SPS eye did not finish");
            require(!workspace->analysisResult()->timingLocked && workspace->analysisResult()->samplesPerSymbol==3.2,"Manual folding must remain unlocked");
            const auto manual=workspace->analysisResult();widebandNativeClick(child<QPushButton>(window,"eyePage"));
            require(workspace->analysisResult()==manual && !child<QWidget>(window,"narrowbandConstellationLargeChart")->isVisible() && child<QWidget>(window,"narrowbandEyeLargeChart")->isVisible(),"Eye must be independent and reuse the real stage snapshot");scene(prefix+"-03-manual-eye");
            if(suffix=="sigmf-meta")widebandNativeSelect(child<QComboBox>(window,"analysisSymbolRateMode"),static_cast<int>(SymbolRateMode::Estimated));
            widebandNativeSelect(child<QComboBox>(window,"modulationFormat"),static_cast<int>(ModulationHypothesis::Qpsk));
            widebandNativeClick(child<QPushButton>(window,"recoverNarrowbandSignal"));
            require(waitUntil([&]{auto r=workspace->analysisResult();return r && r->carrierLocked && !r->eyes.empty();},20000),"True single-carrier recovery did not lock on the prescribed fixture");
            const auto recovered=workspace->analysisResult();
            widebandNativeClick(child<QPushButton>(window,"modulationPage"));scene(prefix+"-04-recovered-constellation");
            widebandNativeClick(child<QPushButton>(window,"eyePage"));scene(prefix+"-05-recovered-eye");
            widebandNativeClick(child<QPushButton>(window,"deepLearningPage"));
            require(!child<QPushButton>(window,"runRealRecognition")->isEnabled() && child<QLabel>(window,"recognitionUnavailable")->isVisible(),"Undeployed recognition must be unavailable");scene(prefix+"-06-recognition-unavailable");
            widebandNativeClick(child<QPushButton>(window,"demodulationPage"));
            require(child<QLabel>(window,"demodulationUnavailable")->isVisible(),"Undeployed bitstream must be disclosed");scene(prefix+"-07-demodulation-unavailable");
            widebandNativeClick(child<QPushButton>(window,"eyePage"));
            require(workspace->analysisResult()==recovered,"Navigation must not repeat synchronization");
            const auto saved=*window.session().activeChannel();const auto projectPath=output.filePath(prefix+".ssproj");
            require(window.saveProject(projectPath) && window.openProject(projectPath),"ssproj save/reopen failed");
            require(window.session().activeChannel()->analysis==saved.analysis && window.session().activeChannel()->outputSampleRateHz==2e6 && window.session().activeChannel()->page==NarrowbandPage::Eye && window.session().activeChannel()->syncPanelExpanded,"ssproj must preserve Eye, shared panel, analysis and actual Fs");
            require(waitUntil([&]{auto r=workspace->analysisResult();return r && r->carrierLocked;},20000),"Reopened real source must recover again");scene(prefix+"-08-reopened-eye");projects.append(projectPath);
            auto* cancel=child<QPushButton>(window,"cancelNarrowbandAnalysis");widebandScrollTo(cancel);widebandNativeClick(cancel);require(!workspace->analysisResult(),"Cancel must clear the current result");scene(prefix+"-09-cancelled");
        }
        report["pass"]=scenes.size()==18;report["nativeAcceptance"]=!auxiliary && report["pass"].toBool();
    } catch(const std::exception& e) {report["error"]=QString::fromUtf8(e.what());}
    report["scenes"]=scenes;report["sceneCount"]=scenes.size();report["projects"]=projects;report["fullScreen"]=window.isFullScreen();report["logicalWindowSize"]=sizeJson(window.size());
    require(write(),"Could not write narrowband capture report");return report["pass"].toBool()?0:1;
}

QJsonArray narrowbandFeedbackHeaders(MainWindow& window) {
    QJsonArray checks;
    for(const auto* name:{"narrowbandConstellationControls","narrowbandEyeControls","narrowbandModulationStftControls"}) {
        auto* strip=dynamic_cast<ChartControlStrip*>(child<QWidget>(window,name));require(strip,"Shared chart strip required");
        auto* head=strip->parentWidget();require(head->height()==30,"All chart titles must use the same height");
        if(!head->isVisible())continue;
        std::vector<QRect> occupied;int right=-1;
        for(auto* field:strip->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly)) {
            if(!field->isVisible())continue;
            if(qobject_cast<QAbstractButton*>(field) || qobject_cast<QComboBox*>(field))widebandNativeReachable(field);
            const QRect rectangle(field->mapTo(head,QPoint()),field->size());
            require(head->rect().contains(rectangle),"Header field clipped: "+field->objectName());
            for(const auto& previous:occupied)require(!rectangle.intersects(previous),"Header fields overlap: "+field->objectName());
            occupied.push_back(rectangle);right=std::max(right,field->mapTo(strip,QPoint(field->width()-1,0)).x());
        }
        require(right>=0 && std::abs(right-(strip->width()-1))<=1,"Chart controls must be right aligned");
        checks.append(QJsonObject{{"strip",name},{"controls",static_cast<int>(occupied.size())},{"noOverlap",true},{"rightAligned",true},{"height",head->height()}});
    }
    return checks;
}
int runNarrowbandFeedbackCapture(const QString& directory,const QString& sourcePath,bool software) {
    QDir output(directory);require(QFileInfo(directory).isAbsolute() && output.mkpath("."),"Absolute capture directory required");
    const auto reportPath=output.filePath("feedback-report.json");require(!QFileInfo::exists(reportPath),"Fresh capture directory required");
    QJsonObject report{{"mode","narrowband-feedback"},{"pass",false},{"nativeAcceptance",false},{"softwareRenderer",software},{"source",sourcePath}};QJsonArray scenes;
#ifdef QT_DEBUG
    report["configuration"]="Debug";
#else
    report["configuration"]="Release";
#endif
    QScreen* screen=nullptr;int count=0;for(auto* s:QGuiApplication::screens())if(isNarrowbandAcceptanceScreen(s)){screen=s;++count;}
    if(count!=1){report["error"]="指定 Redmi 4K/150% 实屏不可用";}
    else {
        report["screen"]=narrowbandScreenJson(screen);qApp->setProperty("softwareRenderer",software);qApp->setProperty("uiStatePersistenceEnabled",false);
        MainWindow window;
        try {
            QString error;require(window.createProject(directory,"user-source-copy",&error),error);showFullScreenOnScreen(window,screen);
            WavDescriptor wav;require(readWavDescriptor(sourcePath,wav,error),error);
            FileState file;file.metadata.path=sourcePath.toStdString();file.metadata.name=QFileInfo(sourcePath).fileName().toStdString();file.metadata.demo=false;
            file.metadata.sampleFormat=wav.format;file.metadata.sampleCount=wav.sampleCount;file.metadata.sampleRateHz=wav.sampleRateHz;
            file.metadata.centerFrequencyHz=30e6;file.metadata.effectiveBandwidthHz=60e6;require(window.addImportedSource(std::move(file),error),error);
            auto* f=window.session().activeFile();const auto mark=window.session().addMark({{823,597805},{6577967,13347526}});
            require(!mark.empty(),"Source mark must exist");
            const auto channel=window.session().createChannel("32QAM feedback",f->activeMarkId,9962746.14,6769558.28,13540000,{823,597805},ChannelFilter::Standard,false,true);
            require(!channel.empty(),"Real feedback channel must exist");window.refresh();
            widebandNativeClick(child<QPushButton>(window,"modulationPage"));auto* workspace=child<NarrowbandWorkspace>(window,"narrowbandWorkspace");
            const auto settle=[&]{return waitUntil([&]{return workspace->visibleChartsSettled() && workspace->analysisResult() && (software || (workspace->visibleChartsGpuReady() && (workspace->visibleTextureUploads()>0 || workspace->visibleGpuVertexUploads()>0) && workspace->visibleGpuDataDrawCalls()>0));},30000);};
            const auto scene=[&](const QString& name){
                require(settle(),"Feedback rendering did not settle: "+name);QTest::qWait(100);
                const auto picture=window.grab();const auto png=output.filePath(name+".png");
                require(window.isFullScreen() && window.screen()==screen && window.size()==QSize(2560,1440) && picture.size()==QSize(3840,2160) && picture.save(png),"Actual 4K fullscreen capture required");
                const auto backend=workspace->visibleBackendDescription();require(software || backend.contains("D3D11"),"Actual D3D11 required");
                scenes.append(QJsonObject{{"name",name},{"png",png},{"pass",true},{"headerChecks",narrowbandFeedbackHeaders(window)},{"analysis",narrowbandAnalysisEvidence(workspace->analysisResult().get())},{"render",workspace->renderStatistics()},{"backend",backend}});
            };
            require(settle(),"Raw source analysis unavailable");
            widebandNativeSelect(child<QComboBox>(window,"constellationAnalysisStage"),4);require(settle(),"Blocked stage unavailable");
            require(!workspace->analysisResult()->carrierLocked && child<QWidget>(window,"narrowbandConstellationLargeChart")->property("analysisStageFallback").toBool(),"Unknown request must expose fallback");scene("01-unknown-fallback");
            widebandNativeClick(child<QToolButton>(window,"narrowbandSyncSettingsToggle"));
            widebandNativeSelect(child<QComboBox>(window,"modulationFormat"),2);widebandNativeClick(child<QPushButton>(window,"recoverNarrowbandSignal"));require(settle(),"QPSK diagnostic unavailable");
            require(!workspace->analysisResult()->carrierLocked && workspace->analysisResult()->constellationStage!=AnalysisStage::RawIq,"Wrong template must fail and show an actual intermediate stage");scene("02-qpsk-mismatch");
            widebandNativeSelect(child<QComboBox>(window,"modulationFormat"),8);require(child<QComboBox>(window,"analysisModulationOrder")->currentData().toInt()==32,"32QAM selection required");
            widebandNativeClick(child<QPushButton>(window,"recoverNarrowbandSignal"));require(settle() && workspace->analysisResult()->carrierLocked,"Actual 32QAM source must lock");scene("03-32qam-recovered");
            const auto step=[&](QDoubleSpinBox* spin,QStyle::SubControl control){
                widebandNativeReachable(spin);QStyleOptionSpinBox option;option.initFrom(spin);option.rect=spin->rect();option.frame=true;option.buttonSymbols=QAbstractSpinBox::UpDownArrows;
                option.stepEnabled=QAbstractSpinBox::StepUpEnabled|QAbstractSpinBox::StepDownEnabled;
                const auto rect=spin->style()->subControlRect(QStyle::CC_SpinBox,&option,control,spin);require(!rect.isEmpty() && !qobject_cast<QLineEdit*>(spin->childAt(rect.center())),"Step button must not be covered by editor");
                QTest::mouseClick(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,spin->mapTo(&window,rect.center()));
            };
            auto* eyeStrip=dynamic_cast<ChartControlStrip*>(child<QWidget>(window,"narrowbandEyeControls"));
            auto* constellationStrip=dynamic_cast<ChartControlStrip*>(child<QWidget>(window,"narrowbandConstellationControls"));
            require(eyeStrip && constellationStrip,"Both plots require common chart settings");
            widebandNativeClick(child<QPushButton>(window,"eyePage"));
            widebandNativeClick(eyeStrip->overflowButton());
            const auto readable=[&](QDoubleSpinBox* spin){
                widebandNativeReachable(spin);auto* edit=spin->findChild<QLineEdit*>();
                require(edit && edit->contentsRect().width()>=edit->fontMetrics().horizontalAdvance(edit->displayText()),"Full numeric value and unit must fit: "+spin->objectName());
                require(edit->contentsRect().height()>=edit->fontMetrics().height(),"Numeric text must fit vertically");
            };
            auto* rolloff=child<QDoubleSpinBox>(window,"analysisRrcRolloff");readable(rolloff);readable(child<QDoubleSpinBox>(window,"eyeTimingOffsetUi"));
            step(rolloff,QStyle::SC_SpinBoxUp);require(std::abs(window.session().activeChannel()->analysis.rolloff-.4)<1e-9,"Native rolloff Up must commit");step(rolloff,QStyle::SC_SpinBoxDown);
            scene("04-eye-settings");widebandNativeClick(eyeStrip->overflowButton());
            widebandNativeClick(child<QPushButton>(window,"modulationPage"));
            widebandNativeClick(child<QCheckBox>(window,"constellationAutoScale"));
            widebandNativeClick(constellationStrip->overflowButton());
            readable(child<QDoubleSpinBox>(window,"constellationAxisMinimum"));readable(child<QDoubleSpinBox>(window,"constellationAxisMaximum"));
            require(!eyeStrip->popoverFrame()->isVisible(),"Opening another plot must close the old settings layer");scene("05-constellation-settings");
            widebandNativeClick(constellationStrip->overflowButton());widebandNativeClick(child<QCheckBox>(window,"constellationAutoScale"));
            auto* syncToggle=child<QToolButton>(window,"narrowbandSyncSettingsToggle");
            if(!syncToggle->isChecked())widebandNativeClick(syncToggle);
            auto* bandwidth=child<QDoubleSpinBox>(window,"analysisTimingLoopBandwidth");widebandScrollTo(bandwidth);
            step(bandwidth,QStyle::SC_SpinBoxUp);require(std::abs(window.session().activeChannel()->analysis.timingLoopBandwidth-.0021)<1e-9,"Native timing bandwidth Up must use meaningful step");step(bandwidth,QStyle::SC_SpinBoxDown);
            require(settle() && workspace->analysisResult()->carrierLocked,"Stepping controls must preserve the analysis chain");scene("06-native-step-controls");
            const auto project=output.filePath("user-source-copy.ssproj");require(window.saveProject(project) && window.openProject(project),"ssproj actual source reopen required");workspace->activatePageForAcceptance(1);
            require(settle() && workspace->analysisResult()->carrierLocked && window.session().activeChannel()->analysis.modulationOrder==32,"Reopened 32QAM must recover");scene("07-ssproj-reopened");report["project"]=project;
            report["pass"]=scenes.size()==7;report["nativeAcceptance"]=report["pass"];report["layoutUnified"]=report["pass"];
        } catch(const std::exception& e){report["error"]=QString::fromUtf8(e.what());}
    }
    report["scenes"]=scenes;report["sceneCount"]=scenes.size();QFile file(reportPath);require(file.open(QIODevice::WriteOnly),"Feedback report write failed");file.write(QJsonDocument(report).toJson());return report["pass"].toBool()?0:1;
}
