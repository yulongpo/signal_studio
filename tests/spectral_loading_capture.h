// Included inside ui_capture.cpp's native acceptance namespace.
int runSpectralLoadingCapture(const QString& directory,const QString& largeIq) {
    QDir output(directory);QDir().mkpath(output.absolutePath());
    QJsonObject report{{"pass",false},{"screens",screensJson()}};QJsonArray scenes;
    MainWindow window;
    const auto save=[&](QString name){require(window.isFullScreen()&&isDisplayNumber(window.screen(),2),"Must capture display 2 fullscreen");const auto image=window.grab();require(image.size()==QSize(3840,2160),"Native 4K screenshot required");require(image.save(output.filePath(name+".png")),"Screenshot write failed");scenes.append(name+".png");};
    const auto wideReady=[&]{return waitUntil([&]{for(const auto* p:window.findChildren<PlotWidget*>())if(p->isVisible()&&(!p->isDisplaySettled()||!p->gpuReady()))return false;return true;},30000);};
    const auto narrowReady=[&]{return waitUntil([&]{auto* w=window.findChild<NarrowbandWorkspace*>();return w&&w->visibleChartsSettled()&&w->visibleChartsGpuReady()&&window.findChild<QWidget*>("narrowbandPsdPanelChart")->property("psdComplete").toBool();},30000);};
    try {
        auto* screen=QGuiApplication::screens().value(1);require(screen&&displayPixelSize(screen)==QSize(3840,2160)&&std::abs(screen->devicePixelRatio()-1.5)<.001,"Display 2 must be native 4K / 150%");
        showFullScreenOnScreen(window,screen);require(QTest::qWaitForWindowExposed(&window,5000),"Native window not exposed");require(waitUntil([&]{return window.isFullScreen()&&window.screen()==screen&&window.size()==QSize(2560,1440);},5000),"Fullscreen placement failed");report["screen"]=screenJson(screen);
        QString path=largeIq;
        if(path.isEmpty()){path=output.filePath("loading_FS1Msps_BW800kHz_FC10MHz.dat");QFile f(path);require(f.open(QIODevice::WriteOnly)&&f.resize(256LL*1024*1024),"Loading fixture creation failed");}
        QString error;require(window.addIqFile(path,&error),error);auto* file=window.session().activeFile();
        require(waitUntil([&]{return file->metadata.availability.availableSamples>0;},10000),"No actual loading progress");
        require(file->metadata.availability.status==LoadStatus::Loading,"Loading completed before stop acceptance");
        save("01-read-progress");child<QPushButton>(window,"stopSourceLoad")->click();
        require(waitUntil([&]{return file->metadata.availability.status!=LoadStatus::Loading;},10000),"Stop did not finish bounded block");
        const auto prefix=availableSamples(file->metadata);require(prefix>0&&prefix<file->metadata.sampleCount,"Stop must retain a real partial prefix");
        require(file->navigationEnvelope.size()<=2048,"Navigator exceeded 2048 envelope points");
        report["loading"]=QJsonObject{{"source",path},{"physicalBytes",QFileInfo(path).size()},{"prefixSamples",QString::number(prefix)},{"envelopePoints",static_cast<int>(file->navigationEnvelope.size())},{"status","Partial"}};
        file->view.time={0,std::min<SampleIndex>(prefix,static_cast<SampleIndex>(file->metadata.sampleRateHz*.01))};window.refresh();require(wideReady(),"Prefix wideband did not settle");save("02-prefix-envelope");
        require(window.saveProject(output.filePath("prefix-project.json")),"Prefix project save failed");require(window.openProject(output.filePath("prefix-project.json")),"Prefix project reopen failed");
        require(waitUntil([&]{return window.session().activeFile()->metadata.availability.status!=LoadStatus::Loading;},10000),"Prefix reopen did not complete");file=window.session().activeFile();require(availableSamples(file->metadata)==prefix,"Reopen changed prefix");
        child<QComboBox>(window,"psdFft")->setCurrentText("32");child<QComboBox>(window,"stftFft")->setCurrentText("32");
        window.session().setAuxiliaryMode(AuxiliaryMode::Psd);window.session().setView({{0,3},file->view.frequency},false);window.refresh();require(wideReady(),"Padded wide PSD did not settle");
        require(file->view.time==TimeRange{0,3},"Padding changed physical view");report["widePadded"]=child<PlotWidget>(window,"auxPlot")->renderStatistics();save("03-wide-padded-32");
        QTimer confirm;QObject::connect(&confirm,&QTimer::timeout,[&]{if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))if(box->windowTitle()=="打开窄带演示工程")if(auto* yes=box->button(QMessageBox::Yes))yes->click();});confirm.start(10);
        window.openNarrowbandDemoProject();confirm.stop();require(narrowReady(),"NB initial whole-channel PSD not ready");auto* workspace=window.findChild<NarrowbandWorkspace*>();auto* ch=window.session().activeChannel();
        require(ch->psd.scope==PsdScope::Whole&&ch->psd.parameters.method==SpectralMethod::Welch,"New channel must default to whole-channel Welch");
        const auto context=ch->id;const auto initial=window.session().spectrogram(context);const QJsonValue generation=workspace->renderStatistics().value("requestGeneration");
        child<QComboBox>(window,"psdFft")->setCurrentText("32");child<QComboBox>(window,"psdWindow")->setCurrentIndex(2);require(narrowReady(),"Independent PSD not complete");
        require(window.session().spectrogram(context)==initial&&workspace->renderStatistics()["requestGeneration"]==generation,"PSD parameters recomputed STFT");
        child<QComboBox>(window,"psdStatistic")->setCurrentIndex(1);require(narrowReady(),"Maximum PSD not ready");save("04-narrow-whole-maximum");
        child<QComboBox>(window,"stftFft")->setCurrentText("32");const auto begin=ch->sourceTime.begin;const auto shortSpan=static_cast<SampleIndex>(std::ceil(5*window.session().activeFile()->metadata.sampleRateHz/ch->outputSampleRateHz));window.session().setChannelView({begin,begin+shortSpan},ch->visibleBasebandFrequency,false);window.refresh();require(narrowReady(),"Short NB STFT not ready");
        const auto data=window.session().spectrogram(context);require(data&&data->frames.size()>=10&&data->sourceView==TimeRange{begin,begin+shortSpan}&&data->paddedFrames>=10,"Short STFT violated frame count, time axis or padding");
        report["narrowShort"]=workspace->renderStatistics();auto* heat=child<QWidget>(window,"narrowbandStftPanelChart");auto* psd=child<QWidget>(window,"narrowbandPsdPanelChart");
        const QPoint point(static_cast<int>(66+(heat->width()-82)*.85),static_cast<int>(14+(heat->height()-55)*.5));QTest::mouseClick(heat,Qt::LeftButton,Qt::NoModifier,point);QTest::qWait(100);
        const auto* frame=window.session().selectedSpectralFrame(context);require(frame&&psd->property("effectiveFftPoints").toInt()==32,"Padded display frame not reused by PSD");save("05-narrow-padded-frame");
        report["selectedFrame"]=QString::number(frame->id);
        child<QPushButton>(window,"propertyEditChannel")->click();
        require(waitUntil([&]{return window.findChild<QDialog*>("channelConfigDialog")!=nullptr;},2000),"Channel config dialog did not open");
        auto* dialog=window.findChild<QDialog*>("channelConfigDialog");
        require(waitUntil([&]{return dialog->isVisible()&&dialog->windowHandle()!=nullptr;},2000),"Channel dialog not exposed");
        dialog->windowHandle()->setScreen(window.screen());dialog->move(window.screen()->geometry().center()-dialog->rect().center());
        child<QCheckBox>(*dialog,"channelPreserveSourceTime")->setChecked(false);
        QTest::qWait(100);const auto dialogImage=dialog->grab();
        require(dialog->screen()==window.screen()&&std::abs(dialogImage.devicePixelRatioF()-1.5)<.001,"Channel dialog must use display 2 native DPI");
        require(dialogImage.save(output.filePath("06-channel-config-checkboxes.png")),"Channel dialog screenshot write failed");
        report["channelDialog"]=QJsonObject{{"screen",screenJson(dialog->screen())},{"pixelSize",sizeJson(dialogImage.size())},{"devicePixelRatio",dialogImage.devicePixelRatioF()},{"parentFullScreen",window.isFullScreen()}};
        scenes.append("06-channel-config-checkboxes.png");dialog->reject();QTest::qWait(50);
        require(workspace->visibleGpuDataDrawCalls()>0,"No GPU geometry/heatmap data draws");report["pass"]=true;
    }catch(const std::exception& e){report["error"]=QString::fromUtf8(e.what());}
    report["scenes"]=scenes;QFile result(output.filePath("spectral-loading-report.json"));if(result.open(QIODevice::WriteOnly))result.write(QJsonDocument(report).toJson());
    return report["pass"].toBool()?0:1;
}
