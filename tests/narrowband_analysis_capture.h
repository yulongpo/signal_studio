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
    QJsonObject report{{"mode","narrowband-analysis-v2"},{"platform",QGuiApplication::platformName()},
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
            require(waitUntil([&]{return workspace->visibleChartsSettled() && (software || (workspace->visibleChartsGpuReady() && workspace->visibleTextureUploads()>0 && workspace->visibleGpuDataDrawCalls()>0));},15000),"Real chart rendering did not settle: "+name);
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
            widebandNativeSelect(child<QComboBox>(window,"analysisSymbolRateMode"),1);
            auto* rs=child<QDoubleSpinBox>(window,"symbolRate");auto* rsEdit=rs->findChild<QLineEdit*>();require(rsEdit,"Rs editor is absent");
            widebandNativeReachable(rs);rsEdit->setFocus();QTest::keyClick(window.windowHandle(),Qt::Key_A,Qt::ControlModifier);for(const auto character:QByteArray("625000"))QTest::keyClick(window.windowHandle(),character);QTest::keyClick(window.windowHandle(),Qt::Key_Return);
            require(waitUntil([&]{auto r=workspace->analysisResult();return r && r->eyeStatus==AnalysisStatus::Ready && r->symbolRateHz==625000;},15000),"Manual fractional-SPS eye did not finish");
            require(!workspace->analysisResult()->timingLocked && workspace->analysisResult()->samplesPerSymbol==3.2,"Manual folding must remain unlocked");scene(prefix+"-03-manual-eye");
            if(suffix=="sigmf-meta")widebandNativeSelect(child<QComboBox>(window,"analysisSymbolRateMode"),static_cast<int>(SymbolRateMode::Estimated));
            widebandNativeSelect(child<QComboBox>(window,"modulationFormat"),static_cast<int>(ModulationHypothesis::Qpsk));
            widebandNativeClick(child<QPushButton>(window,"recoverNarrowbandSignal"));
            require(waitUntil([&]{auto r=workspace->analysisResult();return r && r->carrierLocked && !r->eyes.empty();},20000),"True single-carrier recovery did not lock on the prescribed fixture");
            scene(prefix+"-04-recovered");
            const auto saved=*window.session().activeChannel();const auto projectPath=output.filePath(prefix+"/project.json");
            require(window.saveProject(projectPath) && window.openProject(projectPath),"v6 save/reopen failed");workspace->activatePageForAcceptance(1);
            require(window.session().activeChannel()->analysis==saved.analysis && window.session().activeChannel()->outputSampleRateHz==2e6,"v6 must preserve committed analysis and actual Fs");
            require(waitUntil([&]{auto r=workspace->analysisResult();return r && r->carrierLocked;},20000),"Reopened real source must recover again");scene(prefix+"-05-reopened");projects.append(projectPath);
            widebandNativeClick(child<QPushButton>(window,"cancelNarrowbandAnalysis"));require(!workspace->analysisResult(),"Cancel must clear the current result");scene(prefix+"-06-cancelled");
        }
        report["pass"]=scenes.size()==12;report["nativeAcceptance"]=!auxiliary && report["pass"].toBool();
    } catch(const std::exception& e) {report["error"]=QString::fromUtf8(e.what());}
    report["scenes"]=scenes;report["sceneCount"]=scenes.size();report["projects"]=projects;report["fullScreen"]=window.isFullScreen();report["logicalWindowSize"]=sizeJson(window.size());
    require(write(),"Could not write narrowband capture report");return report["pass"].toBool()?0:1;
}
