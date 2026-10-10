// Included in ui_capture.cpp's native acceptance namespace.
int runImportBrandCapture(const QString& directory){
    QDir output(directory);QDir().mkpath(output.filePath("screenshots"));QJsonObject report{{"pass",false},{"platform",QGuiApplication::platformName()},{"screens",screensJson()}};QJsonArray scenes;
    MainWindow window;QScreen* screen=QGuiApplication::screens().value(1);
    const auto save=[&](const QString& name,const QString& input=QString{}){
        require(window.isFullScreen()&&window.screen()==screen&&window.size()==QSize(2560,1440),"Must use connected screen 2 fullscreen");
        QTest::qWait(200);QWidget* target=QApplication::activeModalWidget();if(!target)target=&window;
        require(target->screen()==screen&&std::abs(target->devicePixelRatioF()-1.5)<.001,"Captured Qt window must use actual screen 2 DPI");
        const auto shot=target->grab();if(target==&window)require(shot.size()==QSize(3840,2160),"Fullscreen capture must have native monitor dimensions");
        const auto image=shot.toImage();int nonblack=0;for(int y=0;y<image.height();y+=20)for(int x=0;x<image.width();x+=20){const auto c=image.pixelColor(x,y);if(c.red()+c.green()+c.blue()>30)++nonblack;}require(nonblack>100,"Screenshot is blank/black");require(shot.save(output.filePath("screenshots/"+name+".png")),"Screenshot failed");
        scenes.append(QJsonObject{{"file","screenshots/"+name+".png"},{"input",input},{"screen",screenJson(screen)},{"parentFullscreen",true},{"captureMethod","Native QWidget::grab; modal crop over fullscreen parent"},{"capturedObject",target->objectName()},{"pixelSize",sizeJson(shot.size())},{"dpr",shot.devicePixelRatioF()},{"nonBlackSamples",nonblack}});
    };
    try{
        require(screen&&displayPixelSize(screen)==QSize(3840,2160)&&std::abs(screen->devicePixelRatio()-1.5)<.001,"Screen 2 must be actual 4K / 150%");showFullScreenOnScreen(window,screen);require(QTest::qWaitForWindowExposed(&window,5000),"Fullscreen window not exposed");require(waitUntil([&]{return window.isFullScreen()&&window.screen()==screen&&window.size()==QSize(2560,1440);},5000),"Native fullscreen placement failed");report["screen"]=screenJson(screen);save("01-welcome-brand");
        const auto complex=QStringLiteral(SS_SOURCE_DIR "/tests/fixtures/IQ0_FS1Msps_BW800kHz_FC100MHz.dat");const auto real=QStringLiteral(SS_SOURCE_DIR "/tests/fixtures/real_ADC_FS1Msps_FC0Hz_RI16.raw");QString error;require(window.addIqFile(complex,&error),error);
        const auto settled=[&]{return waitUntil([&]{for(auto* plot:window.findChildren<PlotWidget*>())if(plot->isVisible()&&(!plot->isDisplaySettled()||!plot->gpuReady()))return false;return true;},30000);};require(settled(),"Actual CI16 plots did not settle on hardware");save("02-broadband-brand",complex);QJsonArray renderers;for(auto* plot:window.findChildren<PlotWidget*>())if(plot->isVisible())renderers.append(plot->renderStatistics());report["broadbandRenderers"]=renderers;
        {
            SignalImportDialog dialog("验收工程",&window);dialog.show();dialog.windowHandle()->setScreen(screen);dialog.move(screen->geometry().center()-dialog.rect().center());require(dialog.addPath(complex),"CI16 queue failed");dialog.findChild<QCheckBox*>("importConfirmFormat")->setChecked(true);
            auto* preview=dialog.findChild<SourcePreviewWidget*>("sourcePreview");require(waitUntil([&]{return preview->ready();},15000),"Real CI16 preview failed");save("03-import-complex",complex);save("05-import-preview-real-file",complex);
            require(dialog.addPath(real),"RI16 queue failed");require(waitUntil([&]{return preview->ready();},15000),"Real RI16 preview failed");save("04-import-real",real);dialog.setPage(1);save("06-import-batch",complex+"; "+real);dialog.setPage(2);save("10-import-templates");dialog.hide();
        }
        {
            QTemporaryDir temporary;const auto large=temporary.filePath("CI8_FS1Msps_FC0Hz.raw");{QFile file(large);require(file.open(QIODevice::WriteOnly)&&file.resize(128*1024*1024),"Prefix capture fixture failed");}
            SignalImportDialog dialog("验收工程",&window);dialog.show();dialog.windowHandle()->setScreen(screen);dialog.move(screen->geometry().center()-dialog.rect().center());require(dialog.addPath(large),"Prefix source failed");dialog.startImport();require(waitUntil([&]{return dialog.controller().rows()[0].load.loaded>0;},15000),"No actual decoded progress");dialog.findChild<QPushButton*>("importProgressStop")->click();require(waitUntil([&]{return !dialog.controller().running()&&dialog.controller().canCommit();},15000),"Prefix stop failed");require(waitUntil([&]{return dialog.findChild<QPushButton*>("importProgressFinish")->isEnabled();},15000),"Prefix first preview failed");const auto& row=dialog.controller().rows()[0];require(row.status==ImportStatus::Partial,"Capture must retain true partial data");save("07-import-progress-partial",large);report["partial"]=QJsonObject{{"physicalSamples",QString::number(row.metadata.sampleCount)},{"availableSamples",QString::number(row.load.loaded)},{"envelopePoints",static_cast<int>(row.load.envelope.size())},{"format",QString::fromStdString(formatId(row.metadata.sampleFormat))},{"fixtureDeletedAfterCapture",true}};dialog.hide();
        }
        QTimer::singleShot(0,[]{for(auto* widget:QApplication::topLevelWidgets())if(auto* box=qobject_cast<QMessageBox*>(widget))if(auto* yes=box->button(QMessageBox::Yes))yes->click();});
        window.openNarrowbandDemoProject();
        require(waitUntil([&]{auto* n=window.findChild<NarrowbandWorkspace*>();return n&&n->isVisible()&&n->visibleGpuDataDrawCalls()>0;},30000),"Narrowband hardware did not settle");
        save("08-narrowband-brand",":/signalstudio/demo/narrowband_demo.iq");
        if(auto* n=window.findChild<NarrowbandWorkspace*>())report["narrowbandRenderer"]=n->renderStatistics();
        window.findChild<QAction*>("aboutSignalStudioAction")->trigger();QTest::qWait(200);
        auto* about=window.findChild<QDialog*>("aboutSignalStudioDialog");require(about!=nullptr,"About dialog missing");
        about->windowHandle()->setScreen(screen);about->move(screen->geometry().center()-about->rect().center());save("09-about-brand");about->hide();
        report["pass"]=true;
    }catch(const std::exception& e){report["error"]=QString::fromUtf8(e.what());}
    report["scenes"]=scenes;QFile file(output.filePath("native-capture-report.json"));if(file.open(QIODevice::WriteOnly))file.write(QJsonDocument(report).toJson());return report["pass"].toBool()?0:1;
}
