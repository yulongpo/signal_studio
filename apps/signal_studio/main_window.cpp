#include "main_window.hpp"
#include "import_wizard.hpp"

#include "signal_studio/visualization/data_series.hpp"
#include "signal_studio/visualization/views.hpp"

#include <QAction>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <filesystem>

namespace signal::studio {

MainWindow::MainWindow(Application* app, QWidget* parent)
    : QMainWindow(parent), app_(app) {
  setWindowTitle("Signal Studio");
  resize(1280, 720);
  buildCentral();
  buildMenu();
  statusLabel_ = new QLabel("Ready", this);
  statusBar()->addWidget(statusLabel_);
}

MainWindow::~MainWindow() = default;

void MainWindow::buildCentral() {
  auto* central = new QWidget(this);
  auto* layout = new QVBoxLayout(central);
  layout->setContentsMargins(2, 2, 2, 2);
  layout->setSpacing(2);

  auto tw = visualization::make_time_waveform_view();
  auto sp = visualization::make_spectrum_view();
  auto sg = visualization::make_spectrogram_view();
  if (tw.ok()) timeView_ = std::shared_ptr<visualization::IChartView>(std::move(*tw));
  if (sp.ok()) spectrumView_ = std::shared_ptr<visualization::IChartView>(std::move(*sp));
  if (sg.ok()) spectrogramView_ = std::shared_ptr<visualization::IChartView>(std::move(*sg));

  if (timeView_) {
    auto* w = static_cast<QWidget*>(timeView_->native_widget());
    w->setMinimumHeight(120);
    layout->addWidget(w, 1);
  }
  if (spectrumView_) {
    auto* w = static_cast<QWidget*>(spectrumView_->native_widget());
    w->setMinimumHeight(160);
    layout->addWidget(w, 2);
  }
  if (spectrogramView_) {
    auto* w = static_cast<QWidget*>(spectrogramView_->native_widget());
    w->setMinimumHeight(220);
    layout->addWidget(w, 3);
  }
  setCentralWidget(central);
}

void MainWindow::buildMenu() {
  auto* fileMenu = menuBar()->addMenu(tr("&File"));
  auto* openWavAct = fileMenu->addAction(tr("Open &WAV..."));
  connect(openWavAct, &QAction::triggered, this, &MainWindow::openWav);
  auto* openSc16Act = fileMenu->addAction(tr("Open &SC16 (RAW IQ)..."));
  connect(openSc16Act, &QAction::triggered, this, &MainWindow::openSc16);
  fileMenu->addSeparator();
  auto* exitAct = fileMenu->addAction(tr("E&xit"));
  connect(exitAct, &QAction::triggered, this, &QWidget::close);

  auto* viewMenu = menuBar()->addMenu(tr("&View"));
  spectrumAction_ = viewMenu->addAction(tr("Show &Spectrum"));
  spectrumAction_->setCheckable(true);
  spectrumAction_->setChecked(true);
  connect(spectrumAction_, &QAction::toggled, this, &MainWindow::toggleSpectrumVisible);
  spectrogramAction_ = viewMenu->addAction(tr("Show &Spectrogram"));
  spectrogramAction_->setCheckable(true);
  spectrogramAction_->setChecked(true);
  connect(spectrogramAction_, &QAction::toggled, this, &MainWindow::toggleSpectrogramVisible);

  auto* helpMenu = menuBar()->addMenu(tr("&Help"));
  auto* aboutAct = helpMenu->addAction(tr("&About"));
  connect(aboutAct, &QAction::triggered, this, &MainWindow::about);
}

void MainWindow::openWav() {
  const QString path = QFileDialog::getOpenFileName(this, tr("Open WAV"), {}, tr("WAV files (*.wav)"));
  if (path.isEmpty()) return;
  loadAndAnalyze(std::filesystem::path(path.toStdU16String()), false);
}

void MainWindow::openSc16() {
  const QString path = QFileDialog::getOpenFileName(this, tr("Open SC16"), {}, tr("SC16 files (*.sc16)"));
  if (path.isEmpty()) return;
  ImportWizard wiz(this);
  const FilenameHint hint = parse_capture_filename(std::filesystem::path(path.toStdU16String()));
  wiz.applyFilenameHint(hint);
  if (wiz.exec() != QDialog::Accepted) return;
  loadAndAnalyze(std::filesystem::path(path.toStdU16String()), true);
}

void MainWindow::toggleSpectrumVisible(bool visible) {
  if (spectrumView_) spectrumView_->set_visible(visible);
}
void MainWindow::toggleSpectrogramVisible(bool visible) {
  if (spectrogramView_) spectrogramView_->set_visible(visible);
}

void MainWindow::about() {
  QMessageBox::information(this, tr("About Signal Studio"),
                           tr("Signal Studio 1.0.0\nSignal Processing Platform\nQt %1\nFFT backend: %2")
                               .arg(QLatin1String(qVersion()))
                               .arg(app_->fft_available() ? QLatin1String("CUDA (cuFFT)") : QLatin1String("none")));
}

void MainWindow::loadAndAnalyze(const std::filesystem::path& path, bool is_sc16) {
  core::Result<ImportResult> import = is_sc16 ? app_->import_sc16(path) : app_->import_wav(path);
  if (!import.ok()) {
    statusLabel_->setText(QString("Import failed: %1").arg(QString::fromStdString(std::string(import.error().message()))));
    return;
  }
  currentImport_ = *import;
  const double sr = import->descriptor.sample_rate_hz;
  const std::uint64_t count = std::min<std::uint64_t>(import->total_samples, 1 << 16);
  auto slice = app_->read_samples(*import->source, 0, count, 64 * 1024 * 1024);
  if (!slice.ok()) {
    statusLabel_->setText(QString("Read failed: %1").arg(QString::fromStdString(std::string(slice.error().message()))));
    return;
  }
  viewport_ = std::make_unique<visualization::ViewportController>(
      visualization::LoadedDataRange{0, import->total_samples, sr}, sr / 2.0);
  bindAnalysis(*slice, sr);
  statusLabel_->setText(QString("Loaded %1 samples @ %2 Hz").arg(quint64(import->total_samples)).arg(sr));
}

void MainWindow::bindAnalysis(const data::SignalSlice& slice, double sample_rate_hz) {
  if (timeView_ && slice.kind() == data::SignalKind::real) {
    std::vector<double> samples(slice.real_values().begin(), slice.real_values().end());
    auto series = std::make_shared<visualization::RealSeries>("time", std::move(samples), sample_rate_hz);
    timeView_->bind(series);
  }
  if (app_->fft_available()) {
    auto psd = app_->analyze_psd(slice, sample_rate_hz, 1024, 512);
    if (psd.ok() && spectrumView_) {
      auto db = dsp::to_db_hz(psd->power);
      auto series = std::make_shared<visualization::SpectrumSeries>("psd", psd->frequencies_hz, std::move(db), sample_rate_hz);
      spectrumView_->bind(series);
    }
    auto stft = app_->analyze_stft(slice, sample_rate_hz, 512, 128);
    if (stft.ok() && spectrogramView_) {
      std::vector<double> mag(stft->matrix.begin(), stft->matrix.end());
      auto series = std::make_shared<visualization::SpectrogramSeries>("stft", stft->time_bins, stft->freq_bins,
                                                                       std::move(mag), stft->frame_count, stft->freq_count);
      spectrogramView_->bind(series);
    }
  }
}

}  // namespace signal::studio
