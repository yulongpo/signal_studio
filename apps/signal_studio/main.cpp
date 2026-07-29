#include "application.hpp"
#include "qt_application.hpp"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

[[nodiscard]] std::optional<std::string_view> raw_option(int argc, char* argv[], std::string_view option) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::string_view{argv[index]} == option) {
      return argv[index + 1];
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool has_raw_option(int argc, char* argv[], std::string_view option) {
  for (int index = 1; index < argc; ++index) {
    if (std::string_view{argv[index]} == option) {
      return true;
    }
  }
  return false;
}

} // namespace

int main(int argc, char* argv[]) {
  if (has_raw_option(argc, argv, "--self-test")) {
    const auto scratch_option = raw_option(argc, argv, "--scratch");
    const auto scratch = scratch_option ? std::filesystem::u8path(*scratch_option)
                                        : std::filesystem::temp_directory_path() / "signal-studio-self-test";
    const auto status = signal::studio::run_headless_self_test(scratch);
    if (!status) {
      std::cerr << status.code().stable_text() << ": " << status.message() << '\n' << status.diagnostic() << '\n';
      return 2;
    }
    std::cout << "Signal Studio headless self-test passed\n";
    return 0;
  }

  QApplication qt_application(argc, argv);
  QCoreApplication::setOrganizationName("Signal Studio");
  QCoreApplication::setApplicationName("Signal Studio");
  QCoreApplication::setApplicationVersion("1.0.0");

  QCommandLineParser parser;
  parser.setApplicationDescription("Signal Studio 离线 IQ 信号分析");
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption state_option("state-dir", "应用状态目录", "path");
  const QCommandLineOption input_option("input", "自动化使用的真实输入文件", "path");
  const QCommandLineOption screenshot_option("screenshot", "保存完整窗口截图", "path");
  const QCommandLineOption page_option("page", "页面：p01、p02、p03、p05、w01、w05", "page", "p01");
  const QCommandLineOption width_option("width", "逻辑窗口宽度", "pixels", "1600");
  const QCommandLineOption height_option("height", "逻辑窗口高度", "pixels", "900");
  const QCommandLineOption startup_smoke_option("startup-smoke", "验证默认 Qt 平台和窗口构造");
  parser.addOption(state_option);
  parser.addOption(input_option);
  parser.addOption(screenshot_option);
  parser.addOption(page_option);
  parser.addOption(width_option);
  parser.addOption(height_option);
  parser.addOption(startup_smoke_option);
  parser.process(qt_application);

  auto state_directory = parser.isSet(state_option)
                             ? std::filesystem::path{parser.value(state_option).toStdWString()}
                             : std::filesystem::path{QDir::homePath().toStdWString()} / ".signal-studio";
  signal::studio::QtApplication application{qt_application, std::move(state_directory)};
  if (const auto initialized = application.initialize(); !initialized) {
    std::cerr << initialized.code().stable_text() << ": " << initialized.message() << '\n';
    return 3;
  }
  if (parser.isSet(input_option)) {
    const auto source = std::filesystem::path{parser.value(input_option).toStdWString()};
    if (const auto prepared = application.prepare_automation_input(source); !prepared) {
      std::cerr << prepared.code().stable_text() << ": " << prepared.message() << '\n' << prepared.diagnostic() << '\n';
      return 4;
    }
  }

  auto* window = static_cast<QWidget*>(application.native_handle());
  bool width_ok{};
  bool height_ok{};
  const auto width = parser.value(width_option).toInt(&width_ok);
  const auto height = parser.value(height_option).toInt(&height_ok);
  if (!width_ok || !height_ok || width < 960 || height < 640) {
    std::cerr << "窗口逻辑尺寸无效\n";
    return 5;
  }
  if (parser.isSet(screenshot_option)) {
    // 自动化截图由 QWidget::grab/render 完成，不需要把超大测试窗口交给桌面窗口管理器裁剪。
    window->setAttribute(Qt::WA_DontShowOnScreen);
  }
  window->resize(width, height);
  application.show();

  const auto page = parser.value(page_option).toLower();
  if (page == "w01") {
    application.show_import_wizard();
  } else if (page == "w05") {
    if (!parser.isSet(input_option)) {
      std::cerr << "W05 截图必须提供 --input\n";
      return 6;
    }
    application.show_progress_preview(parser.value(input_option).toStdWString());
  } else if (const auto shown = application.show_page(page.toStdString()); !shown) {
    std::cerr << shown.message() << '\n';
    return 7;
  }
  if (parser.isSet(startup_smoke_option)) {
    std::cout << "platform=" << QGuiApplication::platformName().toStdString() << " dpr=" << window->devicePixelRatioF()
              << '\n';
    QTimer::singleShot(0, &qt_application, &QCoreApplication::quit);
  } else if (parser.isSet(screenshot_option)) {
    const auto path = QFileInfo(parser.value(screenshot_option)).absoluteFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QTimer::singleShot(700, window, [&application, path, &qt_application] {
      if (const auto saved = application.save_screenshot(path.toStdWString()); !saved) {
        qt_application.exit(8);
      } else {
        qt_application.quit();
      }
    });
  }
  return qt_application.exec();
}
