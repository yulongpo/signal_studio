#pragma once

#include "application.hpp"
#include "signal_studio/visualization/chart_view.hpp"
#include "signal_studio/visualization/viewport.hpp"

#include <QMainWindow>
#include <memory>

class QLabel;
class QAction;

namespace signal::studio {

class MainWindow final : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(Application* app, QWidget* parent = nullptr);
  ~MainWindow() override;

 private slots:
  void openWav();
  void openSc16();
  void toggleSpectrumVisible(bool visible);
  void toggleSpectrogramVisible(bool visible);
  void about();

 private:
  void buildMenu();
  void buildCentral();
  void loadAndAnalyze(const std::filesystem::path& path, bool is_sc16);
  void bindAnalysis(const data::SignalSlice& slice, double sample_rate_hz);

  Application* app_;
  std::shared_ptr<visualization::IChartView> timeView_;
  std::shared_ptr<visualization::IChartView> spectrumView_;
  std::shared_ptr<visualization::IChartView> spectrogramView_;
  std::unique_ptr<visualization::ViewportController> viewport_;
  QLabel* statusLabel_{nullptr};
  QAction* spectrumAction_{nullptr};
  QAction* spectrogramAction_{nullptr};
  std::optional<ImportResult> currentImport_;
};

}  // namespace signal::studio
