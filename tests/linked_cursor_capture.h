// Included inside ui_capture.cpp's acceptance namespace to reuse native helpers.
int runLinkedCursorCapture(const QString& directory, const QString& largeIq) {
    QDir output(directory); QDir().mkpath(output.absolutePath());
    QJsonObject report{{"pass", false}, {"screens", screensJson()}};
    QJsonArray scenes;
    MainWindow window;
    auto save = [&](const QString& name) {
        require(window.isFullScreen() && isDisplayNumber(window.screen(), 2), "Capture must use display 2 fullscreen");
        const auto image = window.grab();
        require(image.size() == QSize(3840, 2160), "Capture must have native 4K pixels");
        require(image.save(output.filePath(name + ".png")), "Could not save cursor screenshot");
        scenes.append(name + ".png");
    };
    auto readyWide = [&] {
        return waitUntil([&] {
            for (const auto* plot : window.findChildren<PlotWidget*>())
                if (plot->isVisible() && (!plot->isDisplaySettled() || !plot->gpuReady())) return false;
            return window.session().activeFile() && window.session().spectrogram(window.session().activeFile()->metadata.id);
        }, 60'000);
    };
    auto readyNarrow = [&] {
        return waitUntil([&] {
            const auto* workspace = window.findChild<NarrowbandWorkspace*>();
            return workspace && workspace->visibleChartsSettled() && workspace->visibleChartsGpuReady() &&
                window.session().activeChannel() && window.session().spectrogram(window.session().activeChannel()->id);
        }, 90'000);
    };
    try {
        auto* screen = QGuiApplication::screens().value(1);
        require(screen && displayPixelSize(screen) == QSize(3840, 2160) && std::abs(screen->devicePixelRatio() - 1.5) < .001,
            "Linked cursor acceptance requires connected display 2 at 4K / 150%");
        showFullScreenOnScreen(window, screen);
        require(QTest::qWaitForWindowExposed(&window, 5000), "Window did not expose on display 2");
        require(waitUntil([&] { return window.isFullScreen() && window.screen() == screen && window.size() == QSize(2560, 1440); }, 5000), "Fullscreen placement failed");
        report["screen"] = screenJson(screen); report["fullScreen"] = window.isFullScreen();
        report["logicalWindowSize"] = sizeJson(window.size());
        const auto fixture = output.filePath("IQ0_FS1Msps_BW1MHz_FC100MHz.dat");
        QFile file(fixture); require(file.open(QIODevice::WriteOnly), "Could not create deterministic IQ fixture");
        QByteArray samples(524288 * 4, Qt::Uninitialized);
        for (int n = 0; n < 524288; ++n) {
            const auto value = .3 * std::polar(1.0, 2 * std::numbers::pi * 8040 * n / 1e6) +
                .2 * std::polar(1.0, 2 * std::numbers::pi * 8080 * n / 1e6) +
                .1 * std::polar(1.0, 2 * std::numbers::pi * 26000 * n / 1e6);
            const auto real = static_cast<qint16>(std::lround(value.real() * 32768));
            const auto imag = static_cast<qint16>(std::lround(value.imag() * 32768));
            samples[n * 4] = static_cast<char>(real & 255); samples[n * 4 + 1] = static_cast<char>((real >> 8) & 255);
            samples[n * 4 + 2] = static_cast<char>(imag & 255); samples[n * 4 + 3] = static_cast<char>((imag >> 8) & 255);
        }
        file.write(samples); file.close();
        QString error; require(window.addIqFile(fixture, &error), error);
        require(waitUntil([&]{return window.session().activeFile()->metadata.availability.status!=LoadStatus::Loading;},30000),"Fixture loading did not complete");
        auto* source = window.session().activeFile();
        source->display.referenceLevelDb = -30; source->display.dynamicRangeDb = 100;
        source->display.grid = true; source->display.stftSize = 2048; source->display.psdSize = 4096;
        window.session().setView({{0, 524288}, {100e6 - 32000, 100e6 + 32000}}); window.refresh();
        require(readyWide(), "Wideband display did not settle");
        auto* main = window.findChild<PlotWidget*>("mainPlot"); auto* aux = window.findChild<PlotWidget*>("auxPlot");
        const auto context = source->metadata.id;
        const auto before = main->renderStatistics();
        moveThroughWindow(main, main->plotRect().center().toPoint()); QTest::qWait(100);
        require(main->property("cursorReadout").toString().contains("dBFS/Hz"), "Wide heatmap hover has no numerical power");
        const auto after = main->renderStatistics();
        for (const auto* key : {"powerGenerations", "textureUploads", "gpuVertexUploads"})
            require(before[key] == after[key], "Wide hover changed FFT or data resources");
        QTest::mouseClick(main, Qt::LeftButton, Qt::NoModifier, main->plotRect().center().toPoint());
        require(window.session().linkedCursor(context).pinned, "Wide click did not pin");
        save("wide-waveform-linked");
        window.session().setAuxiliaryMode(AuxiliaryMode::Psd); window.refresh(); require(readyWide(), "Frame PSD did not settle");
        const auto* frame = window.session().selectedSpectralFrame(context); require(frame, "No exact selected frame");
        require(aux->sourcePointCount() == qsizetype(frame->linearPower.size()), "Frame PSD changed the STFT N");
        moveThroughWindow(aux, aux->plotRect().center().toPoint()); QTest::qWait(50);
        const auto bin = frame->binAt((source->view.frequency.lowerHz + source->view.frequency.upperHz) / 2 - source->metadata.centerFrequencyHz);
        require(std::abs(aux->property("cursorPowerDb").toDouble() - frame->dbAt(bin)) < .01, "Frame PSD power differs from clicked STFT frame");
        save("wide-frame-psd"); report["wideFramePsd"] = aux->renderStatistics();
        report["windowChrome"] = verifyWindowChrome(window,screen); require(readyWide(),"GPU resources did not rebuild after window actions");
        report["widePowerControls"] = verifyPowerAndLabels(window,false); require(readyWide(),"Wide display did not settle after color updates"); save("wide-power-axis-labels");
        window.session().setView({source->view.time, {100e6 - 16000, 100e6 + 16000}}); window.refresh();
        require(readyWide(), "Frequency zoom did not settle");
        const auto zoomed = main->renderStatistics();
        require(zoomed["analysisPoints"] == before["analysisPoints"] && zoomed["binHz"].toDouble() < before["binHz"].toDouble() &&
            zoomed["requiredSeconds"].toDouble() > before["requiredSeconds"].toDouble(), "Frequency zoom did not increase real acquisition duration at fixed N");
        report["wideFrequencyZoom"] = zoomed; report["wideHoverBefore"] = before; report["wideHoverAfter"] = after;
        window.session().clearCursor(context); source->display.mainMode = MainMode::Waterfall; window.refresh();
        require(readyWide(), "Waterfall did not settle");
        QTest::mouseClick(main, Qt::LeftButton, Qt::NoModifier, main->plotRect().center().toPoint());
        require(readyWide(), "Waterfall frame PSD did not settle"); save("wide-waterfall-frame");

        window.session().newProject(); window.refresh(); window.openNarrowbandDemoProject(); require(readyNarrow(), "Narrowband display did not settle");
        auto* workspace = window.findChild<NarrowbandWorkspace*>();
        auto* heat = window.findChild<QWidget*>("narrowbandStftPanelChart");
        auto* psd = window.findChild<QWidget*>("narrowbandPsdPanelChart");
        auto* rightStft = window.findChild<QComboBox*>("stftFft");
        auto* headerStft = window.findChild<QComboBox*>("narrowbandStftFft");
        require(rightStft && headerStft, "Narrowband FFT controls are missing");
        const int widePoints = window.session().activeFile()->display.stftSize;
        rightStft->setCurrentText("4096"); require(readyNarrow(), "Sidebar FFT request did not settle");
        require(window.session().spectrogram(window.session().activeChannel()->id)->plan.points == 4096 &&
            headerStft->currentData().toInt() == 4096, "Sidebar FFT did not change the actual narrowband N");
        headerStft->setCurrentIndex(headerStft->findData(2048)); require(readyNarrow(), "Header FFT request did not settle");
        require(rightStft->currentText() == "2048" && window.session().activeFile()->display.stftSize == widePoints,
            "Narrowband FFT controls are not synchronized or changed wideband settings");
        report["narrowbandSidebarFftVerified"] = true;
        const auto nbContext = window.session().activeChannel()->id;
        const auto nbBefore = workspace->renderStatistics();
        const QRectF plot(66, 14, heat->width() - 82, heat->height() - 55);
        moveThroughWindow(heat, plot.center().toPoint()); QTest::qWait(80);
        require(heat->property("cursorReadout").toString().contains("dBFS/Hz"), "Narrowband hover has no numerical power");
        const auto nbAfter = workspace->renderStatistics();
        for (const auto* key : {"requestGeneration", "iqCacheMisses", "textureUploads", "gpuVertexUploads"})
            require(nbBefore[key] == nbAfter[key], "Narrowband hover changed DDC or data resources");
        QTest::mouseClick(heat, Qt::LeftButton, Qt::NoModifier, plot.center().toPoint()); QTest::qWait(80);
        const auto* nbFrame = window.session().selectedSpectralFrame(nbContext); require(nbFrame, "Narrowband frame not selected");
        require(psd->property("effectiveFftPoints").toInt() == int(nbFrame->linearPower.size()), "Narrowband frame N is inconsistent");
        save("narrowband-linked-frame");
        report["narrowbandPowerControls"] = verifyPowerAndLabels(window,true); require(readyNarrow(),"Narrow display did not settle after color updates"); save("narrowband-power-axis-labels");
        QTest::keyClick(heat, Qt::Key_Escape);
        require(!window.session().linkedCursor(nbContext).pinned, "Narrowband Esc did not clear cursor");
        report["narrowbandHoverBefore"] = nbBefore; report["narrowbandHoverAfter"] = nbAfter;
        for (int page = 1; page < 4; ++page) {
            workspace->activatePageForAcceptance(page); require(readyNarrow(), "Narrowband page did not settle");
            if (page < 3) {
                auto* heatChart=window.findChild<QWidget*>(page==1 ? "narrowbandModulationStftChart" : "recognitionStftPanelChart");
                auto* surface=heatChart->findChild<AcceleratedSurface*>();
                const QRectF expected(66,14,heatChart->width()-82,heatChart->height()-55);
                require(surface&&surface->heatmapTargetRect()==expected,"Page heatmap did not follow the final chart geometry");
                const auto image=heatChart->grab().toImage();const double dpr=image.devicePixelRatio();
                const QPoint probe(qRound((expected.left()+expected.width()*.93)*dpr),qRound((expected.top()+expected.height()*.93)*dpr));
                require(image.pixelColor(probe)!=QColor("#0a1728"),"GPU heatmap left the resized chart background uncovered");
            }
            save("narrowband-page-" + QString::number(page));
        }
        report["resizedPageHeatmapsVerified"]=true;
        if (!largeIq.isEmpty()) {
            require(QFileInfo::exists(largeIq), "Requested large IQ fixture is missing");
            window.session().newProject(); window.refresh();
            require(window.addIqFile(largeIq, &error), error);
            require(waitUntil([&]{return window.session().activeFile()->metadata.availability.status!=LoadStatus::Loading;},120000),"Large IQ loading did not complete"); require(readyWide(), "Large IQ wideband did not settle");
            source = window.session().activeFile();
            const auto begin = std::min<SampleIndex>(source->metadata.sampleCount / 8, 128000000);
            const auto span = static_cast<SampleIndex>(source->metadata.sampleRateHz * .025);
            const auto center = source->metadata.centerFrequencyHz;
            const auto mark = window.session().addMark({{begin, begin + span}, {center - 500000, center + 500000}});
            const auto channel = window.session().createChannel("Large IQ acceptance", mark, center, 1e6, 2e6,
                {begin, begin + span}, ChannelFilter::Standard, false, true);
            require(!channel.empty(), "Large IQ channel creation failed");
            QElapsedTimer timer; timer.start(); window.refresh(); require(readyNarrow(), "Large IQ channel did not settle");
            report["largeIq"] = QJsonObject{{"path", largeIq}, {"bytes", QFileInfo(largeIq).size()},
                {"initialDisplayMs", timer.elapsed()}, {"initial", workspace->renderStatistics()}};
            const auto view = window.session().activeChannel()->visibleSourceTime;
            window.session().setChannelView({view.begin, view.begin + (view.end - view.begin) / 2}, {-500000, 500000}); window.refresh();
            QTest::qWait(80);
            window.session().setChannelView(view, {-750000, 750000}); window.refresh();
            require(readyNarrow(), "Large IQ cancellation/zoom did not settle");
            auto updated = *window.session().activeChannel(); updated.filter = ChannelFilter::HighRejection;
            require(window.session().updateChannel(channel, updated), "Large IQ configuration change failed");
            window.refresh(); QTest::qWait(80); // Start cold DSP, then replace it while processing.
            updated = *window.session().activeChannel(); updated.filter = ChannelFilter::FastPreview;
            require(window.session().updateChannel(channel, updated), "Large IQ replacement configuration failed");
            window.refresh(); require(readyNarrow(), "Large IQ changed configuration did not settle");
            report["largeIqFinalConfigVersion"] = QString::number(window.session().activeChannel()->configVersion);
            report["largeIqFinal"] = workspace->renderStatistics(); save("large-iq-narrowband");
        }
        report["pass"] = true;
    } catch (const std::exception& error) { report["error"] = QString::fromUtf8(error.what()); }
    report["scenes"] = scenes;
    QFile result(output.filePath("linked-cursor-report.json"));
    if (result.open(QIODevice::WriteOnly)) result.write(QJsonDocument(report).toJson());
    return report["pass"].toBool() ? 0 : 1;
}
