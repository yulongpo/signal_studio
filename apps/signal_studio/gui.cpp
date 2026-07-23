#include "main_window.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>

namespace signal::studio {

int runGui(int argc, char** argv) {
  QApplication app(argc, argv);
  QCommandLineParser parser;
  parser.setApplicationDescription("Signal Studio");
  QCommandLineOption selfTest("self-test", "run headless self-test and exit");
  parser.addOption(selfTest);
  parser.addHelpOption();
  parser.process(app);

  Application studioApp;
  auto* window = new MainWindow(&studioApp);
  window->show();
  return app.exec();
}

}  // namespace signal::studio
