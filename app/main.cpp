#include "app/main_window.h"
#include "ui/charts/plot_widget.h"
#include "ui/display_target.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QScreen>
#include <QTimer>
#include <QTextStream>
#include <QWindow>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <memory>

namespace {
QJsonObject screenInfo(const QScreen* screen) {
    if (!screen) return {};
    const auto geometry = screen->geometry();
    const auto dpr = screen->devicePixelRatio();
    const auto pixels=signalstudio::displayPixelSize(screen);
    return {{"name", screen->name()}, {"deviceName",signalstudio::displayDeviceName(screen)},
        {"connectedIndex",QGuiApplication::screens().indexOf(const_cast<QScreen*>(screen))+1},
        {"logicalGeometry", QJsonArray{geometry.x(),geometry.y(),geometry.width(),geometry.height()}},
        {"pixelSize", QJsonArray{pixels.width(),pixels.height()}},
        {"devicePixelRatio",dpr},{"logicalDpi",screen->logicalDotsPerInch()},
        {"primary",screen==QGuiApplication::primaryScreen()}};
}
QJsonArray screenInventory() {
    QJsonArray result;
    for (const auto* screen : QGuiApplication::screens()) result.append(screenInfo(screen));
    return result;
}
}

int main(int argc,char* argv[]) {
    QApplication app(argc,argv);QApplication::setOrganizationName("Signal Studio");QApplication::setApplicationName("SignalStudio");QApplication::setApplicationVersion("0.1.0");
    QCommandLineParser parser;parser.setApplicationDescription("A1.4.3 native UI with explicit demo charts");parser.addHelpOption();parser.addVersionOption();
    parser.addOption({"smoke-test","Render the workspace, validate the three charts and exit."});
    parser.addOption({"screenshot","Save a Qt-rendered workspace PNG and exit.","path"});
    parser.addOption({"size","Logical workspace dimensions (2560x1440 at 150% produces a 4K capture).","WxH","2560x1440"});
    parser.addOption({"verify-4k-150","Require a 2560x1440 logical workspace at device pixel ratio 1.5."});
    parser.addOption({"screen","Select the 1-based connected screen number from --list-screens (validation uses 2).","number"});
    parser.addOption({"full-screen","Display the workspace full screen on the selected monitor."});
    parser.addOption({"list-screens","Print monitor names, dimensions and DPI, then exit."});
    parser.addOption({"project","Open a native project folder or JSON at startup.","path"});
    parser.addOption({"demo-data","Load the three prototype demo files; normal startup is an empty project."});
    parser.addOption({"iq-file","Open an interleaved int16 IQ file at startup; FS/FC/BW are read from its filename.","path"});
    parser.addOption({"psd-view","Start the auxiliary chart in real-IQ PSD mode (used with --iq-file)."});
    parser.addOption({"iq-interaction-test","Send one real wheel-zoom event after the IQ graphs settle; requires --iq-file."});
    parser.addOption({"software-renderer","Use the explicit QPainter fallback."});
    parser.addOption({"require-gpu","Fail the smoke test unless hardware rendering and texture reuse succeed."});
    parser.addOption({"renderer","QRhi API: auto, d3d11 (Windows), or opengl.","api","auto"});
    parser.addOption({"render-report","Write smoke-test renderer and cache evidence as JSON.","path"});parser.process(app);
    const bool interactiveLaunch = !parser.isSet("list-screens") && !parser.isSet("smoke-test") &&
        !parser.isSet("screenshot") && !parser.isSet("iq-interaction-test");
    app.setProperty("uiStatePersistenceEnabled", interactiveLaunch);
    if (parser.isSet("list-screens")) {
        const auto bytes=QJsonDocument(screenInventory()).toJson();
        if (parser.isSet("render-report")) {
            const auto path=parser.value("render-report");QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);
            if (!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())return 1;
        }
        QTextStream(stdout)<<bytes;return 0;
    }
    QScreen* targetScreen = nullptr;
    const auto targetNumber = parser.value("screen");
    if (parser.isSet("screen")) {
        bool valid = false; const auto number = targetNumber.toInt(&valid);
        if (!valid || number < 1) {QTextStream(stderr)<<"Invalid --screen\n";return 2;}
        for (auto* screen : QGuiApplication::screens())
            if (signalstudio::isDisplayNumber(screen,number)) targetScreen=screen;
        if (!targetScreen) {QTextStream(stderr)<<"Requested connected screen "<<number<<" is unavailable\n";return 2;}
    }
    if (parser.isSet("verify-4k-150")) {
        if (!targetScreen || !signalstudio::isDisplayNumber(targetScreen,2) ||
            targetScreen->geometry().size()!=QSize(2560,1440) || signalstudio::displayPixelSize(targetScreen)!=QSize(3840,2160) || qAbs(targetScreen->devicePixelRatio()-1.5)>.001) {
            QTextStream(stderr)<<"4K/150% validation requires --screen 2 at native 3840x2160 / 150%\n";return 2;
        }
    }
    // All QRhi widgets in a window share one graphics API.
    const auto renderer=parser.value("renderer");
    if(renderer!="auto"&&renderer!="d3d11"&&renderer!="opengl"){QTextStream(stderr)<<"Unsupported --renderer\n";return 2;}
    if(parser.isSet("require-gpu")&&parser.isSet("software-renderer")){QTextStream(stderr)<<"Conflicting renderer options\n";return 2;}
    if(parser.isSet("iq-interaction-test")&&!parser.isSet("iq-file")){QTextStream(stderr)<<"--iq-interaction-test requires --iq-file\n";return 2;}
    app.setProperty("softwareRenderer",parser.isSet("software-renderer"));app.setProperty("renderApi",renderer);
    signalstudio::MainWindow window;
    if(parser.isSet("demo-data")&&!parser.isSet("project")){
        const auto first=window.session().addDemoFile();window.session().addDemoFile();window.session().addDemoFile();
        window.session().activateFile(first);window.refresh();
    }
    const auto size=parser.value("size").split('x');if(size.size()!=2||size[0].toInt()<1050||size[1].toInt()<650){QTextStream(stderr)<<"Invalid --size: minimum 1050x650\n";return 2;}
    window.resize(size[0].toInt(),size[1].toInt());
    if(parser.isSet("project")&&!window.openProject(parser.value("project")))return 2;
    if(parser.isSet("iq-file")){
        QString error;
        if(!window.addIqFile(parser.value("iq-file"),&error)){QTextStream(stderr)<<error<<'\n';return 2;}
        if(parser.isSet("psd-view")) { window.session().setAuxiliaryMode(signalstudio::AuxiliaryMode::Psd); window.refresh(); }
    }
    if (targetScreen) {
        window.setScreen(targetScreen);
        window.winId();window.windowHandle()->setScreen(targetScreen);
        window.move(targetScreen->geometry().topLeft());
    }
    if (parser.isSet("full-screen")&&targetScreen) {
        signalstudio::showFullScreenOnScreen(window,targetScreen);
    }
    else if (parser.isSet("full-screen")) window.showFullScreen();
    else if (!parser.isSet("size")) window.showMaximized();
    else window.show();
    if(parser.isSet("smoke-test")||parser.isSet("screenshot")||parser.isSet("iq-interaction-test")) {
        auto* readinessTimer=new QTimer(&window);readinessTimer->setInterval(50);
        auto elapsed=std::make_shared<QElapsedTimer>();elapsed->start();
        bool interactionSent=false,iqWheelZoomPassed=false;
        QJsonObject iqInteractionEvidence;
        QObject::connect(readinessTimer,&QTimer::timeout,&window,[&,readinessTimer,elapsed]{
            auto* main=window.findChild<signalstudio::PlotWidget*>("mainPlot");
            auto* aux=window.findChild<signalstudio::PlotWidget*>("auxPlot");
            auto* nav=window.findChild<signalstudio::PlotWidget*>("navigationPlot");
            const bool chartsReady=(!parser.isSet("full-screen")||window.isFullScreen())&&
                (!targetScreen||window.screen()==targetScreen)&&main&&aux&&nav&&main->isDisplaySettled()&&aux->isDisplaySettled()&&nav->isDisplaySettled()&&
                (!parser.isSet("require-gpu")||(main->gpuReady()&&aux->gpuReady()&&nav->gpuReady()&&main->textureUploadCount()>0));
            if(parser.isSet("iq-interaction-test")&&chartsReady&&!interactionSent){
                interactionSent=true;
                const auto* active=window.session().activeFile();
                const auto before=active?active->view:signalstudio::ViewRange{};
                const auto local=main->plotRect().center();
                QWheelEvent wheel(local,main->mapToGlobal(local.toPoint()),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::ScrollBegin,false);
                QApplication::sendEvent(main,&wheel);
                const auto* after=window.session().activeFile();
                iqWheelZoomPassed=after&&active&&after->view.time!=before.time&&main->hasPendingInteraction();
                iqInteractionEvidence={{"wheelEventSent",true},{"timeRangeChanged",after&&after->view.time!=before.time},
                    {"interactionPending",main->hasPendingInteraction()}};
                return;
            }
            if (!chartsReady&&elapsed->elapsed()<10000)return;
            readinessTimer->stop();readinessTimer->deleteLater();
            const quint64 uploaded=main?main->textureUploadCount():0,powers=main?main->powerGenerationCount():0;
            // A new overlay frame must reuse the existing heatmap texture and power matrix.
            if(main) {main->setCreating(true);main->setCreating(false);}
            QTimer::singleShot(150,&window,[&,main,uploaded,powers]{
                auto* aux=window.findChild<signalstudio::PlotWidget*>("auxPlot");auto* nav=window.findChild<signalstudio::PlotWidget*>("navigationPlot");
                bool ok=main&&aux&&nav&&main->isDisplaySettled()&&aux->isDisplaySettled()&&nav->isDisplaySettled()&&main->plotRect().height()>=200&&(!targetScreen||window.screen()==targetScreen);
                if(parser.isSet("iq-interaction-test")) ok=ok&&interactionSent&&iqWheelZoomPassed;
                const bool reuse=main&&main->textureUploadCount()==uploaded&&main->powerGenerationCount()==powers;
                const bool gpu=main&&aux&&nav&&main->gpuReady()&&aux->gpuReady()&&nav->gpuReady()&&uploaded>0&&main->completedFrameCount()>0&&aux->completedFrameCount()>0&&nav->completedFrameCount()>0;
                if(parser.isSet("require-gpu"))ok=ok&&gpu&&reuse;
                const auto capture=window.grab();
                const bool fourK=window.screen()==targetScreen&&targetScreen&&signalstudio::isDisplayNumber(targetScreen,2)&&
                    targetScreen->geometry().size()==QSize(2560,1440)&&signalstudio::displayPixelSize(targetScreen)==QSize(3840,2160)&&window.isFullScreen()&&window.size()==QSize(2560,1440)&&
                    qAbs(window.devicePixelRatioF()-1.5)<.001&&capture.size()==QSize(3840,2160);
                if(parser.isSet("verify-4k-150"))ok=ok&&fourK;
                if(parser.isSet("screenshot")) {const auto path=parser.value("screenshot");QDir().mkpath(QFileInfo(path).absolutePath());ok=capture.save(path)&&ok;}
                const auto* activeFile=window.session().activeFile();
                QJsonObject activeFileEvidence;
                if(activeFile) activeFileEvidence={{"name",QString::fromUtf8(activeFile->metadata.name.data(),static_cast<qsizetype>(activeFile->metadata.name.size()))},
                    {"path",QString::fromUtf8(activeFile->metadata.path.data(),static_cast<qsizetype>(activeFile->metadata.path.size()))},
                    {"sampleRateHz",activeFile->metadata.sampleRateHz},{"centerFrequencyHz",activeFile->metadata.centerFrequencyHz},
                    {"declaredBandwidthHz",activeFile->metadata.declaredBandwidthHz},{"sampleCount",QString::number(activeFile->metadata.sampleCount)},
                    {"durationSeconds",static_cast<double>(activeFile->metadata.sampleCount)/activeFile->metadata.sampleRateHz},
                    {"realIq",!activeFile->metadata.demo},{"mainMode",activeFile->display.mainMode==signalstudio::MainMode::Waterfall?"waterfall":"timeFrequency"},
                    {"auxiliaryMode",activeFile->display.auxiliaryMode==signalstudio::AuxiliaryMode::Psd?"psd":"waveform"}};
                QJsonObject report{{"pass",ok},{"hardwareRenderer",gpu},{"backend",main?main->renderingBackend():"missing"},{"heatmapUploads",static_cast<qint64>(uploaded)},
                    {"uploadsAfterOverlay",static_cast<qint64>(main?main->textureUploadCount():0)},{"powerGenerations",static_cast<qint64>(powers)},{"overlayReusesHeatmap",reuse},
                    {"completedFrames",static_cast<qint64>(main?main->completedFrameCount():0)},{"mainPlotHeight",main?main->plotRect().height():0},{"files",static_cast<int>(window.session().project().files.size())},
                    {"devicePixelRatio",window.devicePixelRatioF()},{"logicalWindowSize",QJsonArray{window.width(),window.height()}},{"capturePixelSize",QJsonArray{capture.width(),capture.height()}},{"fourK150Verified",fourK},
                    {"screen",screenInfo(window.screen())},{"screens",screenInventory()},{"fullScreen",window.isFullScreen()},{"activeFile",activeFileEvidence},
                    {"iqInteraction",iqInteractionEvidence},
                    {"mainRendering",main?main->renderStatistics():QJsonObject{}},{"auxiliaryRendering",aux?aux->renderStatistics():QJsonObject{}},
                    {"screenLogicalSize",QJsonArray{window.screen()->geometry().width(),window.screen()->geometry().height()}},{"screenLogicalDpi",window.screen()->logicalDotsPerInch()}};
                if(parser.isSet("render-report")) {const auto path=parser.value("render-report");QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(report).toJson())<0)ok=false;}
                QTextStream(stdout)<<QJsonDocument(report).toJson(QJsonDocument::Compact)<<"\n";app.exit(ok?0:1);
            });
        });readinessTimer->start();
    }
    return app.exec();
}
