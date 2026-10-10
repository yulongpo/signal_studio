// Included in the native acceptance namespace; only operates on this test window.
QJsonObject verifyWindowChrome(MainWindow& window, QScreen* screen) {
    auto* maximize=window.findChild<QToolButton*>("windowMaximize");
    auto* minimize=window.findChild<QToolButton*>("windowMinimize");
    require(maximize && minimize && window.findChild<QToolButton*>("windowClose"),"Window buttons missing");
    require(window.windowFlags().testFlag(Qt::FramelessWindowHint) && window.menuBar()->height()==34,"Native title bar still present or header height changed");
    const auto context=window.session().activeFile()->metadata.id;
    const auto pin=window.session().linkedCursor(context);
    const auto view=window.session().activeFile()->view;
    maximize->click();
    require(waitUntil([&] { return !window.isFullScreen() && !window.isMaximized() && window.screen()==screen; },5000),"Fullscreen restore failed");
    const auto normal=window.geometry(); maximize->click();
    require(waitUntil([&] { return window.isMaximized() && window.geometry()==screen->availableGeometry(); },5000),"Maximize did not use current screen available area");
    maximize->click(); require(waitUntil([&] { return !window.isMaximized() && window.geometry()==normal; },5000),"Normal geometry was not restored");
    QPoint blank(window.menuBar()->width()-210,16);
    require(!window.menuBar()->actionAt(blank) && !window.menuBar()->childAt(blank),"No blank header test area");
    QTest::mouseDClick(window.menuBar(),Qt::LeftButton,Qt::NoModifier,blank);
    require(waitUntil([&] { return window.isMaximized(); }),"Header double click failed");
    QTest::mouseDClick(window.menuBar(),Qt::LeftButton,Qt::NoModifier,blank);
    require(waitUntil([&] { return !window.isMaximized(); }),"Header double click restore failed");
    // Menu actions must never enter the title-bar drag path.
    auto* action=window.menuBar()->actions().front();
    QTest::mouseClick(window.menuBar(),Qt::LeftButton,Qt::NoModifier,window.menuBar()->actionGeometry(action).center());
    require(waitUntil([&] { return action->menu()->isVisible(); }),"Menu stopped working with custom chrome");
    action->menu()->hide(); require(window.geometry()==normal,"Menu click changed window geometry");
    minimize->click(); require(waitUntil([&] { return window.isMinimized(); }),"Minimize failed");
    window.showNormal(); window.raise(); window.activateWindow();
    require(waitUntil([&] { return !window.isMinimized() && window.screen()==screen; }),"Minimize restore failed");
    QJsonObject result{{"normal",rectJson(normal)},{"maximizedAvailable",rectJson(screen->availableGeometry())},
        {"buttons",true},{"doubleClick",true},{"menuIsolation",true},{"minimizeRestore",true}};
#ifdef Q_OS_WIN
    const auto nativeDrag=[&](QWidget* target,QPoint point,LONG dx,LONG dy) {
        QCursor::setPos(target->mapToGlobal(point)); QTest::qWait(100);
        INPUT down{}; down.type=INPUT_MOUSE; down.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
        require(SendInput(1,&down,sizeof(INPUT))==1,"Native mouse down failed");
        std::thread input([dx,dy] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            INPUT move{}; move.type=INPUT_MOUSE; move.mi.dwFlags=MOUSEEVENTF_MOVE; move.mi.dx=12;
            SendInput(1,&move,sizeof(INPUT));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            move.mi.dx=dx; move.mi.dy=dy; SendInput(1,&move,sizeof(INPUT));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            INPUT up{}; up.type=INPUT_MOUSE; up.mi.dwFlags=MOUSEEVENTF_LEFTUP; SendInput(1,&up,sizeof(INPUT));
        });
        QTest::qWait(1000); input.join(); QCoreApplication::processEvents();
    };
    const auto beforeMove=window.geometry(); nativeDrag(window.menuBar(),blank,54,36);
    require(window.geometry().topLeft()!=beforeMove.topLeft() && window.screen()==screen,"System title drag did not move the window on screen 2");
    result["moved"]=rectJson(window.geometry()); result["systemMove"]=true;
    const auto beforeResize=window.geometry(); nativeDrag(&window,QPoint(window.width()-2,window.height()/2),60,0);
    require(window.width()>beforeResize.width() && window.screen()==screen,QString("System edge resize failed: before=%1 after=%2 edges=%3 started=%4").arg(beforeResize.width()).arg(window.width()).arg(window.property("windowResizeEdges").toInt()).arg(window.property("windowSystemResizeStarted").toBool()));
    result["resized"]=rectJson(window.geometry()); result["systemResize"]=true;
#endif
    require(window.session().activeFile()->view==view && window.session().linkedCursor(context).sourceSample==pin.sourceSample,"Window actions changed project view or pin");
    window.findChild<QToolButton*>("windowClose")->click(); require(!window.isVisible(),"Custom close button did not use close path"); result["close"]=true;
    showFullScreenOnScreen(window,screen);
    require(waitUntil([&] { return window.isFullScreen() && window.screen()==screen && window.size()==QSize(2560,1440); },5000),"Return to native fullscreen failed");
    result["restoredFullscreen"]=true; return result;
}

QJsonObject verifyPowerAndLabels(MainWindow& window, bool narrow) {
    auto* dynamic=window.findChild<QComboBox*>("dynamic"); auto* reference=window.findChild<QComboBox*>("reference");
    auto* fit=window.findChild<QPushButton*>("autoPowerFit"); auto* workspace=window.findChild<NarrowbandWorkspace*>();
    auto* main=window.findChild<PlotWidget*>("mainPlot"); auto* aux=window.findChild<PlotWidget*>("auxPlot");
    QWidget* heat=narrow ? window.findChild<QWidget*>("narrowbandStftPanelChart") : main;
    const QRectF plot=narrow ? QRectF(66,14,heat->width()-82,heat->height()-55) : main->plotRect();
    const auto context=narrow ? window.session().activeChannel()->id : window.session().activeFile()->metadata.id;
    const auto spectrum=window.session().spectrogram(context); require(spectrum && fit,"Power data unavailable");
    require(waitUntil([&] { return fit->isEnabled(); }),"Auto fit not enabled after data arrived");
    const auto before=narrow ? workspace->renderStatistics() : main->renderStatistics();
    const double previousDynamic=window.session().activeFile()->display.dynamicRangeDb;
    const auto previousCustom=dynamic->property("lastCustomValue");
    dynamic->lineEdit()->setFocus(); dynamic->lineEdit()->selectAll(); QTest::keyClicks(dynamic->lineEdit(),"87.5");
    require(window.session().activeFile()->display.dynamicRangeDb==previousDynamic && dynamic->lineEdit()->hasFocus() && dynamic->lineEdit()->text()=="87.5","Dynamic range applied before explicit commit or disturbed editor");
    require(dynamic->property("lastCustomValue")==previousCustom,"Draft replaced the most recent custom value");
    QTest::keyClick(dynamic->lineEdit(),Qt::Key_Return);
    require(window.session().activeFile()->display.dynamicRangeDb==87.5,"Return did not commit dynamic range");
    reference->lineEdit()->setFocus(); reference->lineEdit()->selectAll(); QTest::keyClicks(reference->lineEdit(),"-");
    const auto last=window.session().activeFile()->display.referenceLevelDb;
    require(reference->lineEdit()->text()=="-" && window.session().activeFile()->display.referenceLevelDb==last,"Intermediate reference text changed the parameter");
    QTest::keyClicks(reference->lineEdit(),"35.25");
    require(window.session().activeFile()->display.referenceLevelDb==last && reference->lineEdit()->hasFocus(),"Reference level applied during typing");
    QTest::keyClick(reference->lineEdit(),Qt::Key_Tab);
    require(window.session().activeFile()->display.referenceLevelDb==-35.25,"Tab did not commit reference level");
    reference->lineEdit()->setFocus();reference->lineEdit()->selectAll();QTest::keyClicks(reference->lineEdit(),"-35.5");
    require(window.session().activeFile()->display.referenceLevelDb==-35.25,"Second draft applied before outside click");
    QTest::mouseClick(window.findChild<QLabel*>("statusTime"),Qt::LeftButton);
    require(window.session().activeFile()->display.referenceLevelDb==-35.5,"Outside click did not commit reference level");
    reference->lineEdit()->setFocus();reference->lineEdit()->selectAll();QTest::keyClicks(reference->lineEdit(),"-35.25");
    QTest::keyClick(reference->lineEdit(),Qt::Key_Backtab);
    require(window.session().activeFile()->display.referenceLevelDb==-35.25,"Shift+Tab did not commit reference level");
    const auto& d=window.session().activeFile()->display;
    require(d.psdMin==-122.75 && d.psdMax==-35.25,"Wide PSD limits did not follow power controls");
    if(narrow) require(window.session().activeChannel()->psdAxisMinimum==-122.75 && window.session().activeChannel()->psdAxisMaximum==-35.25,"Narrow PSD limits did not follow power controls");
    PowerAnalysisSnapshot snapshot=narrow ? workspace->powerSnapshot() : PowerAnalysisSnapshot{main->currentSpectrogram(),aux->currentPowerFrame(),true};
    require(bool(snapshot.heatmap),"Current numerical heatmap missing");
    const auto expected=fitPowerDisplayRange(*snapshot.heatmap,snapshot.psd.get()); fit->click();
    require(expected.valid && d.referenceLevelDb==expected.range.referenceLevelDb && d.dynamicRangeDb==expected.range.dynamicRangeDb,"Auto fit differs from full numerical data");
    const auto after=narrow ? workspace->renderStatistics() : main->renderStatistics();
    require(window.session().spectrogram(context)==spectrum,"Power controls replaced FFT results");
    for(const auto* key : narrow ? std::vector<const char*>{"requestGeneration","iqCacheMisses"} : std::vector<const char*>{"powerGenerations"}) require(before[key]==after[key],"Power control triggered FFT/DDC");
    window.session().clearCursor(context); window.refresh();
    QCursor::setPos(window.menuBar()->mapToGlobal(QPoint(window.menuBar()->width()-210,16)));
    QEvent initialLeave(QEvent::Leave); QCoreApplication::sendEvent(heat,&initialLeave);
    require(waitUntil([&] { return narrow ? workspace->visibleChartsSettled() : main->isDisplaySettled(); }),"Power overlay did not settle before inverse-color capture");
    const auto background=window.grab().toImage();
    QTest::mouseClick(heat,Qt::LeftButton,Qt::NoModifier,plot.center().toPoint()); QTest::qWait(100);
    QCursor::setPos(window.menuBar()->mapToGlobal(QPoint(window.menuBar()->width()-210,16))); QEvent leave(QEvent::Leave); QCoreApplication::sendEvent(heat,&leave); QTest::qWait(100);
    require(!heat->property("hoverCursorVisible").toBool() && heat->property("pinnedReadout").toString().contains("P ="),"Pinned labels disappeared after mouse leave");
    const auto x=heat->property("pinnedXLabelRect").toRectF(),y=heat->property("pinnedYLabelRect").toRectF(),value=heat->property("pinnedValueRect").toRectF();
    require(!x.isEmpty() && !y.isEmpty() && !value.isEmpty() && plot.contains(x) && plot.contains(y) && plot.contains(value),"Pinned axis labels lie outside plot");
    require(std::abs(x.bottom()-plot.bottom())<=5 && std::abs(y.left()-plot.left())<=5,"Cursor coordinates are not at axis ends");
    require(value.left()>plot.center().x() && value.bottom()<plot.center().y(),"Power readout not in first quadrant");
    const QRectF cursorX(plot.center().x()-1,plot.top(),2,plot.height()),cursorY(plot.left(),plot.center().y()-1,plot.width(),2);
    for(const auto& label : {x,y,value}) require(!label.intersects(cursorX) && !label.intersects(cursorY),"Cursor text overlaps cursor line");
    const auto inverted=window.grab().toImage(); const auto origin=heat->mapTo(&window,QPoint{}); const auto dpr=window.devicePixelRatioF();
    const auto inverseError=[&](int px,int py) {
        const auto a=background.pixelColor(px,py),b=inverted.pixelColor(px,py);
        return std::max({std::abs(a.red()+b.red()-255),std::abs(a.green()+b.green()-255),std::abs(a.blue()+b.blue()-255)});
    };
    int inverseLineSamples=0;
    for(const double fraction : {.2,.3,.7,.8}) {
        const int px=qRound((origin.x()+plot.center().x())*dpr),py=qRound((origin.y()+plot.top()+plot.height()*fraction)*dpr);
        int error=255; for(int dx=-1;dx<=1;++dx) error=std::min(error,inverseError(px+dx,py));
        if(error<=8) ++inverseLineSamples;
    }
    require(inverseLineSamples>=3,"Native GPU cursor does not invert its actual background pixels");
    int inverseGlyphPixels=0;
    for(int py=qCeil((origin.y()+value.top())*dpr);py<qFloor((origin.y()+value.bottom())*dpr);++py)
        for(int px=qCeil((origin.x()+value.left())*dpr);px<qFloor((origin.x()+value.right())*dpr);++px)
            if(inverseError(px,py)<=8) ++inverseGlyphPixels;
    require(inverseGlyphPixels>10,"Native GPU readout glyphs do not invert their background pixels");
    const auto resources=narrow ? workspace->renderStatistics() : main->renderStatistics();
    moveThroughWindow(heat,(plot.center()+QPointF(35,25)).toPoint()); QTest::qWait(100);
    const auto hover=narrow ? workspace->renderStatistics() : main->renderStatistics();
    for(const auto* key : {"textureUploads","gpuVertexUploads"}) require(resources[key]==hover[key],"Cursor labels upload data resources");
    const QPoint axis(25,qRound(plot.center().y())); moveThroughWindow(heat,axis); QTest::qWait(50);
    require(heat->cursor().shape()==Qt::OpenHandCursor,"Axis hover cursor differs from shared interaction feedback");
    QCoreApplication::sendEvent(heat,&leave);
    int wheelInputs=0;
    for(auto* input:window.findChildren<QWidget*>()) {
        if(!input->isVisible() || !input->isEnabled())continue;
        auto* combo=qobject_cast<QComboBox*>(input);auto* spin=qobject_cast<QAbstractSpinBox*>(input);
        if(!combo&&!spin)continue;
        const auto text=combo?combo->currentText():spin->findChild<QLineEdit*>()->text();
        const auto index=combo?combo->currentIndex():0;
        for(int delta:{120,-120}) {
            const QPointF point=input->rect().center();
            QWheelEvent wheel(point,input->mapToGlobal(point.toPoint()),QPoint(),QPoint(0,delta),Qt::NoButton,Qt::NoModifier,Qt::ScrollUpdate,false);
            QCoreApplication::sendEvent(input,&wheel);
        }
        require((combo?combo->currentText():spin->findChild<QLineEdit*>()->text())==text && (!combo||combo->currentIndex()==index),"Wheel changed a parameter input");
        ++wheelInputs;
    }
    require(wheelInputs>=5,"Too few visible inputs checked for wheel protection");
    return {{"committedInput",true},{"noApplyDuringTyping",true},{"tabCommit",true},{"backtabCommit",true},{"outsideClickCommit",true},{"wheelProtectedInputs",wheelInputs},{"focusPreserved",true},{"intermediateText",true},{"previousReference",last},
        {"psdCoupling",true},{"referenceDb",expected.range.referenceLevelDb},{"dynamicRangeDb",expected.range.dynamicRangeDb},
        {"noAnalysisRecompute",true},{"pinnedLabels",heat->property("pinnedReadout").toString()},
        {"inverseLineSamples",inverseLineSamples},{"inverseGlyphPixels",inverseGlyphPixels},
        {"xLabel",rectJson(x.toAlignedRect())},{"yLabel",rectJson(y.toAlignedRect())},{"powerLabel",rectJson(value.toAlignedRect())},
        {"before",before},{"after",after},{"cursorResourcesBefore",resources},{"cursorResourcesAfter",hover}};
}
