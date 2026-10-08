#include "app/main_window.h"
#include "ui/charts/plot_widget.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QTextStream>

int main(int argc,char* argv[]) {
    QApplication app(argc,argv);QApplication::setApplicationName("SignalStudio");QApplication::setApplicationVersion("0.1.0");
    QCommandLineParser parser;parser.setApplicationDescription("A1.4.3 native foundation with explicit demo charts");parser.addHelpOption();parser.addVersionOption();
    parser.addOption({"smoke-test","Render the workspace, validate the three charts and exit."});
    parser.addOption({"screenshot","Save a Qt-rendered workspace PNG and exit.","path"});
    parser.addOption({"size","Workspace dimensions for rendering (e.g. 1366x768).","WxH","1600x960"});
    parser.addOption({"project","Open a native project JSON at startup.","path"});
    parser.addOption({"software-renderer","Use the explicit QPainter fallback."});
    parser.addOption({"require-gpu","Fail the smoke test unless hardware rendering and texture reuse succeed."});
    parser.addOption({"renderer","QRhi API: auto, d3d11 (Windows), or opengl.","api","auto"});
    parser.addOption({"render-report","Write smoke-test renderer and cache evidence as JSON.","path"});parser.process(app);
    // All QRhi widgets in a window share one graphics API.
    const auto renderer=parser.value("renderer");
    if(renderer!="auto"&&renderer!="d3d11"&&renderer!="opengl"){QTextStream(stderr)<<"Unsupported --renderer\n";return 2;}
    if(parser.isSet("require-gpu")&&parser.isSet("software-renderer")){QTextStream(stderr)<<"Conflicting renderer options\n";return 2;}
    app.setProperty("softwareRenderer",parser.isSet("software-renderer"));app.setProperty("renderApi",renderer);
    signalstudio::MainWindow window;
    const auto size=parser.value("size").split('x');if(size.size()!=2||size[0].toInt()<1050||size[1].toInt()<650){QTextStream(stderr)<<"Invalid --size: minimum 1050x650\n";return 2;}
    window.resize(size[0].toInt(),size[1].toInt());
    if(parser.isSet("project")&&!window.openProject(parser.value("project")))return 2;
    window.show();
    if(parser.isSet("smoke-test")||parser.isSet("screenshot")) {
        QTimer::singleShot(500,&window,[&]{
            auto* main=window.findChild<signalstudio::PlotWidget*>("mainPlot");
            const quint64 uploaded=main?main->textureUploadCount():0,powers=main?main->powerGenerationCount():0;
            // A new overlay frame must reuse the existing heatmap texture and power matrix.
            if(main) {main->setCreating(true);main->setCreating(false);}
            QTimer::singleShot(150,&window,[&,main,uploaded,powers]{
                auto* aux=window.findChild<signalstudio::PlotWidget*>("auxPlot");auto* nav=window.findChild<signalstudio::PlotWidget*>("navigationPlot");
                bool ok=main&&aux&&nav&&main->plotRect().height()>=200;
                const bool reuse=main&&main->textureUploadCount()==uploaded&&main->powerGenerationCount()==powers;
                const bool gpu=main&&main->gpuReady()&&aux->gpuReady()&&nav->gpuReady()&&uploaded>0;
                if(parser.isSet("require-gpu"))ok=ok&&gpu&&reuse;
                if(parser.isSet("screenshot")) {const auto path=parser.value("screenshot");QDir().mkpath(QFileInfo(path).absolutePath());ok=window.grab().save(path)&&ok;}
                QJsonObject report{{"pass",ok},{"hardwareRenderer",gpu},{"backend",main?main->renderingBackend():"missing"},{"heatmapUploads",static_cast<qint64>(uploaded)},
                    {"uploadsAfterOverlay",static_cast<qint64>(main?main->textureUploadCount():0)},{"powerGenerations",static_cast<qint64>(powers)},{"overlayReusesHeatmap",reuse},
                    {"mainPlotHeight",main?main->plotRect().height():0},{"files",static_cast<int>(window.session().project().files.size())}};
                if(parser.isSet("render-report")) {const auto path=parser.value("render-report");QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(QJsonDocument(report).toJson())<0)ok=false;}
                QTextStream(stdout)<<QJsonDocument(report).toJson(QJsonDocument::Compact)<<"\n";app.exit(ok?0:1);
            });
        });
    }
    return app.exec();
}
